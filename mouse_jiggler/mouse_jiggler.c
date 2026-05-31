#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_usb.h>
#include <furi_hal_usb_hid.h>
#include <gui/gui.h>
#include <input/input.h>

// Interval presets in seconds
static const uint32_t INTERVALS[] = {15, 30, 60, 120, 240};
static const char* INTERVAL_LABELS[] = {"15s", "30s", "1m", "2m", "4m"};
#define NUM_INTERVALS 5

typedef struct {
    FuriMutex* mutex;
    bool enabled;
    bool usb_switched;
    FuriHalUsbInterface* prev_usb;
    uint32_t interval_idx;
    uint32_t move_count;
    uint32_t next_tick;
    FuriThread* worker;
    volatile bool stop;
} App;

static int32_t worker_thread(void* ctx) {
    App* app = ctx;
    uint32_t last_move = furi_get_tick();

    while(!app->stop) {
        furi_delay_ms(200);
        if(!app->enabled) {
            last_move = furi_get_tick();
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            app->next_tick = 0;
            furi_mutex_release(app->mutex);
            continue;
        }

        uint32_t interval_ms = INTERVALS[app->interval_idx] * 1000;
        uint32_t now = furi_get_tick();
        uint32_t elapsed = now - last_move;

        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->next_tick = (elapsed >= interval_ms) ? 0 : (interval_ms - elapsed) / 1000;
        furi_mutex_release(app->mutex);

        if(elapsed >= interval_ms) {
            // Tiny diagonal nudge — net zero motion, subtle
            furi_hal_hid_mouse_move(2, 2);
            furi_delay_ms(60);
            furi_hal_hid_mouse_move(-2, -2);
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            app->move_count++;
            furi_mutex_release(app->mutex);
            last_move = now;
        }
    }
    return 0;
}

static void switch_to_hid(App* app) {
    if(app->usb_switched) return;
    app->prev_usb = furi_hal_usb_get_config();
    furi_hal_usb_unlock();
    if(furi_hal_usb_set_config(&usb_hid, NULL)) {
        app->usb_switched = true;
    }
}

static void restore_usb(App* app) {
    if(!app->usb_switched) return;
    furi_hal_usb_set_config(app->prev_usb, NULL);
    app->usb_switched = false;
}

static void draw_cb(Canvas* canvas, void* ctx) {
    App* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Mouse Jiggler");
    canvas_draw_line(canvas, 0, 13, 128, 13);

    canvas_set_font(canvas, FontBigNumbers);
    canvas_draw_str_aligned(canvas, 64, 30, AlignCenter, AlignCenter,
                            app->enabled ? "ON" : "OFF");

    canvas_set_font(canvas, FontSecondary);
    char buf[40];
    snprintf(buf, sizeof(buf), "Every %s", INTERVAL_LABELS[app->interval_idx]);
    canvas_draw_str_aligned(canvas, 64, 44, AlignCenter, AlignCenter, buf);

    if(app->enabled) {
        snprintf(buf, sizeof(buf), "Moves: %lu  Next: %lus",
                 (unsigned long)app->move_count, (unsigned long)app->next_tick);
    } else {
        snprintf(buf, sizeof(buf), "OK=start  Up/Dn=interval");
    }
    canvas_draw_str_aligned(canvas, 64, 56, AlignCenter, AlignCenter, buf);
    canvas_draw_str_aligned(canvas, 64, 62, AlignCenter, AlignCenter, "Back=exit");

    furi_mutex_release(app->mutex);
}

static void input_cb(InputEvent* e, void* ctx) {
    FuriMessageQueue* q = ctx;
    furi_message_queue_put(q, e, FuriWaitForever);
}

int32_t mouse_jiggler_app(void* p) {
    UNUSED(p);
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(*app));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->interval_idx = 1; // default 30s

    FuriMessageQueue* q = furi_message_queue_alloc(8, sizeof(InputEvent));
    ViewPort* vp = view_port_alloc();
    view_port_draw_callback_set(vp, draw_cb, app);
    view_port_input_callback_set(vp, input_cb, q);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, vp, GuiLayerFullscreen);

    app->worker = furi_thread_alloc();
    furi_thread_set_name(app->worker, "JigglerWorker");
    furi_thread_set_stack_size(app->worker, 1024);
    furi_thread_set_context(app->worker, app);
    furi_thread_set_callback(app->worker, worker_thread);
    furi_thread_start(app->worker);

    InputEvent e;
    bool running = true;
    uint32_t last_redraw = 0;
    while(running) {
        FuriStatus s = furi_message_queue_get(q, &e, 200);
        if(s == FuriStatusOk && (e.type == InputTypeShort || e.type == InputTypeRepeat)) {
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            switch(e.key) {
            case InputKeyOk:
                if(!app->enabled) {
                    switch_to_hid(app);
                    app->enabled = true;
                } else {
                    app->enabled = false;
                }
                break;
            case InputKeyUp:
                if(app->interval_idx < NUM_INTERVALS - 1) app->interval_idx++;
                break;
            case InputKeyDown:
                if(app->interval_idx > 0) app->interval_idx--;
                break;
            case InputKeyBack:
                running = false;
                break;
            default:
                break;
            }
            furi_mutex_release(app->mutex);
        }
        uint32_t now = furi_get_tick();
        if(now - last_redraw > 250) {
            view_port_update(vp);
            last_redraw = now;
        }
    }

    app->stop = true;
    furi_thread_join(app->worker);
    furi_thread_free(app->worker);
    restore_usb(app);

    view_port_enabled_set(vp, false);
    gui_remove_view_port(gui, vp);
    view_port_free(vp);
    furi_message_queue_free(q);
    furi_mutex_free(app->mutex);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
