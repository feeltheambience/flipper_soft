#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <infrared.h>
#include <infrared_transmit.h>

typedef struct {
    const char* brand;
    InfraredProtocol protocol;
    uint32_t address;
    uint32_t command;
} TvCode;

static const TvCode tv_codes[] = {
    {"Samsung",    InfraredProtocolSamsung32, 0x07,   0x02},
    {"LG",         InfraredProtocolNEC,       0x04,   0x08},
    {"Sony 12b",   InfraredProtocolSIRC,      0x01,   0x15},
    {"Sony 15b",   InfraredProtocolSIRC15,    0xA4,   0x15},
    {"Philips",    InfraredProtocolRC5,       0x00,   0x0C},
    {"Sharp",      InfraredProtocolNECext,    0x4054, 0x505F},
    {"Toshiba",    InfraredProtocolNEC,       0x40,   0x12},
    {"JVC",        InfraredProtocolNEC,       0x03,   0x16},
    {"Hisense",    InfraredProtocolNEC,       0x00,   0xE3},
    {"TCL",        InfraredProtocolNEC,       0x06,   0x4D},
    {"Hitachi",    InfraredProtocolNEC,       0x40,   0x15},
    {"Sanyo",      InfraredProtocolNEC,       0x1C,   0x16},
    {"Mitsubishi", InfraredProtocolNEC,       0x23,   0x40},
    {"Xiaomi",     InfraredProtocolNEC,       0x40,   0x0A},
};
#define NUM_CODES (sizeof(tv_codes) / sizeof(tv_codes[0]))

typedef enum {
    StateIdle,
    StateFiring,
    StateDone,
} AppState;

typedef struct {
    FuriMutex* mutex;
    AppState state;
    int current;
    volatile bool stop;
    FuriThread* worker;
} KillApp;

static int32_t worker_thread(void* ctx) {
    KillApp* app = ctx;
    for(size_t i = 0; i < NUM_CODES && !app->stop; i++) {
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->current = (int)i;
        furi_mutex_release(app->mutex);

        InfraredMessage msg = {
            .protocol = tv_codes[i].protocol,
            .address = tv_codes[i].address,
            .command = tv_codes[i].command,
            .repeat = false,
        };
        infrared_send(&msg, 3);
        furi_delay_ms(80);
    }
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->state = StateDone;
    furi_mutex_release(app->mutex);
    return 0;
}

static void draw_cb(Canvas* canvas, void* ctx) {
    KillApp* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 10, AlignCenter, AlignCenter, "IR Kill TV");
    canvas_draw_line(canvas, 0, 14, 128, 14);

    canvas_set_font(canvas, FontSecondary);
    char buf[40];

    if(app->state == StateIdle) {
        canvas_draw_str_aligned(canvas, 64, 26, AlignCenter, AlignCenter,
                                "Aim at TV, press OK");
        snprintf(buf, sizeof(buf), "%u brands loaded", (unsigned)NUM_CODES);
        canvas_draw_str_aligned(canvas, 64, 42, AlignCenter, AlignCenter, buf);
        canvas_draw_str_aligned(canvas, 64, 58, AlignCenter, AlignCenter, "Back = exit");
    } else if(app->state == StateFiring) {
        canvas_draw_str_aligned(canvas, 64, 26, AlignCenter, AlignCenter, "Firing...");
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 42, AlignCenter, AlignCenter,
                                tv_codes[app->current].brand);
        canvas_set_font(canvas, FontSecondary);
        snprintf(buf, sizeof(buf), "%d / %u", app->current + 1, (unsigned)NUM_CODES);
        canvas_draw_str_aligned(canvas, 64, 58, AlignCenter, AlignCenter, buf);
    } else {
        canvas_draw_str_aligned(canvas, 64, 28, AlignCenter, AlignCenter, "Done.");
        canvas_draw_str_aligned(canvas, 64, 44, AlignCenter, AlignCenter, "OK = fire again");
        canvas_draw_str_aligned(canvas, 64, 58, AlignCenter, AlignCenter, "Back = exit");
    }

    furi_mutex_release(app->mutex);
}

static void input_cb(InputEvent* e, void* ctx) {
    FuriMessageQueue* q = ctx;
    furi_message_queue_put(q, e, FuriWaitForever);
}

int32_t ir_killtv_app(void* p) {
    UNUSED(p);
    KillApp* app = malloc(sizeof(KillApp));
    memset(app, 0, sizeof(*app));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->state = StateIdle;

    FuriMessageQueue* q = furi_message_queue_alloc(8, sizeof(InputEvent));
    ViewPort* vp = view_port_alloc();
    view_port_draw_callback_set(vp, draw_cb, app);
    view_port_input_callback_set(vp, input_cb, q);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, vp, GuiLayerFullscreen);

    InputEvent e;
    bool running = true;
    while(running) {
        FuriStatus status = furi_message_queue_get(q, &e, 100);
        if(status == FuriStatusOk && e.type == InputTypeShort) {
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            AppState s = app->state;
            furi_mutex_release(app->mutex);

            if(e.key == InputKeyBack) {
                if(s == StateFiring) {
                    app->stop = true;
                    if(app->worker) {
                        furi_thread_join(app->worker);
                        furi_thread_free(app->worker);
                        app->worker = NULL;
                    }
                    app->stop = false;
                    furi_mutex_acquire(app->mutex, FuriWaitForever);
                    app->state = StateIdle;
                    furi_mutex_release(app->mutex);
                } else {
                    running = false;
                }
            } else if(e.key == InputKeyOk && (s == StateIdle || s == StateDone)) {
                if(app->worker) {
                    furi_thread_join(app->worker);
                    furi_thread_free(app->worker);
                    app->worker = NULL;
                }
                furi_mutex_acquire(app->mutex, FuriWaitForever);
                app->state = StateFiring;
                app->current = 0;
                app->stop = false;
                furi_mutex_release(app->mutex);
                app->worker = furi_thread_alloc();
                furi_thread_set_name(app->worker, "IRWorker");
                furi_thread_set_stack_size(app->worker, 2048);
                furi_thread_set_context(app->worker, app);
                furi_thread_set_callback(app->worker, worker_thread);
                furi_thread_start(app->worker);
            }
        }
        view_port_update(vp);
    }

    if(app->worker) {
        app->stop = true;
        furi_thread_join(app->worker);
        furi_thread_free(app->worker);
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
