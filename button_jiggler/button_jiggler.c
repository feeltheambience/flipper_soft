#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_usb.h>
#include <furi_hal_usb_hid.h>
#include <gui/gui.h>
#include <input/input.h>

// Interval presets in seconds (default 4 minutes per user request)
static const uint32_t INTERVALS[] = {60, 120, 240, 360, 600};
static const char* INTERVAL_LABELS[] = {"1m", "2m", "4m", "6m", "10m"};
#define NUM_INTERVALS 5

// Key choices — SPACE may trigger play/pause or scroll; SHIFT and F15 are safer
typedef struct {
    const char* label;
    uint16_t keycode;
    const char* note;
} KeyChoice;

static const KeyChoice KEYS[] = {
    {"Space",  HID_KEYBOARD_SPACEBAR,  "may type / play-pause"},
    {"Shift",  HID_KEYBOARD_L_SHIFT, "invisible"},
    {"F15",    HID_KEYBOARD_F15,        "invisible, rare key"},
};
#define NUM_KEYS 3

typedef enum {
    FieldInterval,
    FieldKey,
    FieldCount,
} EditField;

typedef struct {
    FuriMutex* mutex;
    bool enabled;
    bool usb_switched;
    FuriHalUsbInterface* prev_usb;
    uint32_t interval_idx;
    uint32_t key_idx;
    EditField field;
    uint32_t press_count;
    uint32_t next_tick;
    FuriThread* worker;
    volatile bool stop;
} App;

static int32_t worker_thread(void* ctx) {
    App* app = ctx;
    uint32_t last_press = furi_get_tick();

    while(!app->stop) {
        furi_delay_ms(200);
        if(!app->enabled) {
            last_press = furi_get_tick();
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            app->next_tick = 0;
            furi_mutex_release(app->mutex);
            continue;
        }

        uint32_t interval_ms = INTERVALS[app->interval_idx] * 1000;
        uint32_t now = furi_get_tick();
        uint32_t elapsed = now - last_press;

        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->next_tick = (elapsed >= interval_ms) ? 0 : (interval_ms - elapsed) / 1000;
        furi_mutex_release(app->mutex);

        if(elapsed >= interval_ms) {
            uint16_t key = KEYS[app->key_idx].keycode;
            furi_hal_hid_kb_press(key);
            furi_delay_ms(40);
            furi_hal_hid_kb_release_all();
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            app->press_count++;
            furi_mutex_release(app->mutex);
            last_press = now;
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
    canvas_draw_str(canvas, 2, 10, "Button Jiggler");
    canvas_draw_line(canvas, 0, 13, 128, 13);

    canvas_set_font(canvas, FontBigNumbers);
    canvas_draw_str_aligned(canvas, 64, 28, AlignCenter, AlignCenter,
                            app->enabled ? "ON" : "OFF");

    canvas_set_font(canvas, FontSecondary);
    char buf[40];

    // Row 1: key | interval. Underline selected field.
    int key_x = 12, int_x = 80;
    snprintf(buf, sizeof(buf), "Key: %s", KEYS[app->key_idx].label);
    canvas_draw_str(canvas, key_x, 44, buf);
    snprintf(buf, sizeof(buf), "Int: %s", INTERVAL_LABELS[app->interval_idx]);
    canvas_draw_str(canvas, int_x, 44, buf);
    if(app->field == FieldKey) canvas_draw_line(canvas, key_x, 46, key_x + 50, 46);
    if(app->field == FieldInterval) canvas_draw_line(canvas, int_x, 46, int_x + 40, 46);

    if(app->enabled) {
        snprintf(buf, sizeof(buf), "Presses: %lu  Next: %lus",
                 (unsigned long)app->press_count, (unsigned long)app->next_tick);
    } else {
        snprintf(buf, sizeof(buf), "OK=start L/R=field Up/Dn=val");
    }
    canvas_draw_str_aligned(canvas, 64, 56, AlignCenter, AlignCenter, buf);
    canvas_draw_str_aligned(canvas, 64, 62, AlignCenter, AlignCenter, "Back=exit");

    furi_mutex_release(app->mutex);
}

static void input_cb(InputEvent* e, void* ctx) {
    FuriMessageQueue* q = ctx;
    furi_message_queue_put(q, e, FuriWaitForever);
}

int32_t button_jiggler_app(void* p) {
    UNUSED(p);
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(*app));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->interval_idx = 2; // 4 minutes default
    app->key_idx = 0;       // Space default
    app->field = FieldInterval;

    FuriMessageQueue* q = furi_message_queue_alloc(8, sizeof(InputEvent));
    ViewPort* vp = view_port_alloc();
    view_port_draw_callback_set(vp, draw_cb, app);
    view_port_input_callback_set(vp, input_cb, q);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, vp, GuiLayerFullscreen);

    app->worker = furi_thread_alloc();
    furi_thread_set_name(app->worker, "ButtonJiggler");
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
            case InputKeyLeft:
            case InputKeyRight:
                app->field = (app->field == FieldInterval) ? FieldKey : FieldInterval;
                break;
            case InputKeyUp:
                if(app->field == FieldInterval) {
                    if(app->interval_idx < NUM_INTERVALS - 1) app->interval_idx++;
                } else {
                    if(app->key_idx < NUM_KEYS - 1) app->key_idx++;
                }
                break;
            case InputKeyDown:
                if(app->field == FieldInterval) {
                    if(app->interval_idx > 0) app->interval_idx--;
                } else {
                    if(app->key_idx > 0) app->key_idx--;
                }
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
