#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_usb.h>
#include <furi_hal_usb_cdc.h>
#include <gui/gui.h>
#include <input/input.h>

#define CDC_IF 1
#define WORK_SECONDS (25 * 60)
#define BREAK_SECONDS (5 * 60)

typedef enum {
    PhaseIdle,
    PhaseWork,
    PhaseBreak,
} Phase;

typedef struct {
    FuriMutex* mutex;
    Phase phase;
    uint32_t seconds_left;
    uint32_t cycle_count;
    bool paused;
    FuriHalUsbInterface* prev_usb;
    bool usb_switched;
} App;

static void send_msg(const char* msg) {
    furi_hal_cdc_send(CDC_IF, (uint8_t*)msg, (uint16_t)strlen(msg));
}

static void start_work(App* app) {
    app->phase = PhaseWork;
    app->seconds_left = WORK_SECONDS;
    app->paused = false;
    send_msg("PHASE,WORK,25\n");
}

static void start_break(App* app) {
    app->phase = PhaseBreak;
    app->seconds_left = BREAK_SECONDS;
    app->paused = false;
    send_msg("PHASE,BREAK,5\n");
}

static void stop(App* app) {
    app->phase = PhaseIdle;
    app->seconds_left = 0;
    app->paused = false;
    send_msg("PHASE,STOP\n");
}

static void draw_cb(Canvas* canvas, void* ctx) {
    App* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Pomodoro Sync");
    canvas_draw_line(canvas, 0, 13, 128, 13);

    canvas_set_font(canvas, FontSecondary);
    const char* label;
    switch(app->phase) {
    case PhaseWork: label = "WORK"; break;
    case PhaseBreak: label = "BREAK"; break;
    default: label = "IDLE"; break;
    }
    canvas_draw_str_aligned(canvas, 64, 22, AlignCenter, AlignCenter, label);

    canvas_set_font(canvas, FontBigNumbers);
    char buf[16];
    uint32_t mins = app->seconds_left / 60;
    uint32_t secs = app->seconds_left % 60;
    snprintf(buf, sizeof(buf), "%02lu:%02lu", (unsigned long)mins, (unsigned long)secs);
    canvas_draw_str_aligned(canvas, 64, 40, AlignCenter, AlignCenter, buf);

    canvas_set_font(canvas, FontSecondary);
    snprintf(buf, sizeof(buf), "Cycles: %lu", (unsigned long)app->cycle_count);
    canvas_draw_str_aligned(canvas, 64, 54, AlignCenter, AlignCenter, buf);

    const char* hint;
    if(app->phase == PhaseIdle) hint = "OK=start WORK  Back=exit";
    else if(app->paused) hint = "OK=resume  L=stop  Back=exit";
    else hint = "OK=pause  L=stop  Back=exit";
    canvas_draw_str_aligned(canvas, 64, 62, AlignCenter, AlignCenter, hint);

    furi_mutex_release(app->mutex);
}

static void input_cb(InputEvent* e, void* ctx) {
    FuriMessageQueue* q = ctx;
    furi_message_queue_put(q, e, FuriWaitForever);
}

int32_t pomodoro_sync_app(void* p) {
    UNUSED(p);
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(*app));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->phase = PhaseIdle;

    // Switch USB to dual CDC so interface 0 keeps qFlipper RPC alive
    app->prev_usb = furi_hal_usb_get_config();
    furi_hal_usb_unlock();
    if(furi_hal_usb_set_config(&usb_cdc_dual, NULL)) {
        app->usb_switched = true;
    }

    FuriMessageQueue* q = furi_message_queue_alloc(8, sizeof(InputEvent));
    ViewPort* vp = view_port_alloc();
    view_port_draw_callback_set(vp, draw_cb, app);
    view_port_input_callback_set(vp, input_cb, q);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, vp, GuiLayerFullscreen);

    InputEvent e;
    bool running = true;
    uint32_t last_tick = furi_get_tick();
    uint32_t last_redraw = 0;

    while(running) {
        // Tick timer once per second (when not paused and not idle)
        uint32_t now = furi_get_tick();
        if(now - last_tick >= 1000) {
            last_tick += 1000;
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            if(app->phase != PhaseIdle && !app->paused) {
                if(app->seconds_left > 0) {
                    app->seconds_left--;
                }
                if(app->seconds_left == 0) {
                    if(app->phase == PhaseWork) {
                        app->cycle_count++;
                        start_break(app);
                    } else { // PhaseBreak
                        start_work(app);
                    }
                }
            }
            furi_mutex_release(app->mutex);
        }

        FuriStatus s = furi_message_queue_get(q, &e, 50);
        if(s == FuriStatusOk && e.type == InputTypeShort) {
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            switch(e.key) {
            case InputKeyOk:
                if(app->phase == PhaseIdle) {
                    start_work(app);
                } else {
                    app->paused = !app->paused;
                    send_msg(app->paused ? "PHASE,PAUSE\n" : "PHASE,RESUME\n");
                }
                break;
            case InputKeyLeft:
                if(app->phase != PhaseIdle) stop(app);
                break;
            case InputKeyBack:
                if(app->phase != PhaseIdle) stop(app);
                running = false;
                break;
            default:
                break;
            }
            furi_mutex_release(app->mutex);
        }

        if(now - last_redraw > 250) {
            view_port_update(vp);
            last_redraw = now;
        }
    }

    if(app->usb_switched) {
        furi_hal_usb_set_config(app->prev_usb, NULL);
    }

    view_port_enabled_set(vp, false);
    gui_remove_view_port(gui, vp);
    view_port_free(vp);
    furi_message_queue_free(q);
    furi_mutex_free(app->mutex);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
