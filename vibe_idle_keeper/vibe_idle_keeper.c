#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_usb.h>
#include <furi_hal_usb_hid.h>
#include <furi_hal_random.h>
#include <gui/gui.h>
#include <input/input.h>

// Modes tuned for different workflows.
// (mean_interval_ms, jitter_pct, action_kind)
typedef enum {
    ActionShift,         // Shift keypress only (totally invisible)
    ActionMouse,         // Mouse nudge only
    ActionShiftAndMouse, // Both
} ActionKind;

typedef struct {
    const char* name;
    uint32_t mean_interval_ms;
    uint8_t jitter_pct;
    ActionKind action;
} Preset;

static const Preset PRESETS[] = {
    {"AI Generation", 30000,  30, ActionShiftAndMouse}, // 21-39 sec
    {"Coding",        60000,  25, ActionShift},          // 45-75 sec, invisible
    {"Watching",      120000, 25, ActionMouse},          // 90-150 sec, just mouse
    {"Aggressive",    15000,  20, ActionShiftAndMouse}, // 12-18 sec
};
#define NUM_PRESETS 4

typedef struct {
    FuriMutex* mutex;
    bool enabled;
    bool usb_switched;
    FuriHalUsbInterface* prev_usb;
    uint32_t preset_idx;
    uint32_t event_count;
    uint32_t shift_count;
    uint32_t mouse_count;
    uint32_t next_in_sec;
    FuriThread* worker;
    volatile bool stop;
} App;

static uint32_t random_interval_ms(const Preset* p) {
    uint32_t base = p->mean_interval_ms;
    uint32_t jitter_range = (base * p->jitter_pct) / 100;
    uint32_t r = furi_hal_random_get() % (2 * jitter_range);
    return base - jitter_range + r;
}

static void do_shift_press(void) {
    furi_hal_hid_kb_press(HID_KEYBOARD_L_SHIFT);
    furi_delay_ms(40);
    furi_hal_hid_kb_release_all();
}

static void do_mouse_nudge(void) {
    // Tiny diagonal, net zero
    int8_t d = (furi_hal_random_get() % 2) ? 2 : -2;
    furi_hal_hid_mouse_move(d, d);
    furi_delay_ms(60);
    furi_hal_hid_mouse_move(-d, -d);
}

static int32_t worker_thread(void* ctx) {
    App* app = ctx;
    uint32_t next_at = furi_get_tick();
    uint32_t interval = random_interval_ms(&PRESETS[app->preset_idx]);

    while(!app->stop) {
        furi_delay_ms(200);

        if(!app->enabled) {
            next_at = furi_get_tick();
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            app->next_in_sec = 0;
            furi_mutex_release(app->mutex);
            continue;
        }

        uint32_t now = furi_get_tick();
        uint32_t remaining = (now < next_at + interval) ? (next_at + interval - now) : 0;

        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->next_in_sec = remaining / 1000;
        furi_mutex_release(app->mutex);

        if(now >= next_at + interval) {
            const Preset* p = &PRESETS[app->preset_idx];
            switch(p->action) {
            case ActionShift:
                do_shift_press();
                furi_mutex_acquire(app->mutex, FuriWaitForever);
                app->shift_count++;
                furi_mutex_release(app->mutex);
                break;
            case ActionMouse:
                do_mouse_nudge();
                furi_mutex_acquire(app->mutex, FuriWaitForever);
                app->mouse_count++;
                furi_mutex_release(app->mutex);
                break;
            case ActionShiftAndMouse:
                // Coin flip which one (variation)
                if(furi_hal_random_get() & 1) {
                    do_shift_press();
                    furi_mutex_acquire(app->mutex, FuriWaitForever);
                    app->shift_count++;
                    furi_mutex_release(app->mutex);
                } else {
                    do_mouse_nudge();
                    furi_mutex_acquire(app->mutex, FuriWaitForever);
                    app->mouse_count++;
                    furi_mutex_release(app->mutex);
                }
                break;
            }
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            app->event_count++;
            furi_mutex_release(app->mutex);

            next_at = now;
            interval = random_interval_ms(&PRESETS[app->preset_idx]);
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
    canvas_draw_str(canvas, 2, 10, "Vibe Idle Keeper");
    canvas_draw_line(canvas, 0, 13, 128, 13);

    canvas_set_font(canvas, FontSecondary);
    char buf[40];

    const Preset* p = &PRESETS[app->preset_idx];
    snprintf(buf, sizeof(buf), "< %s >", p->name);
    canvas_draw_str_aligned(canvas, 64, 22, AlignCenter, AlignCenter, buf);

    canvas_set_font(canvas, FontBigNumbers);
    canvas_draw_str_aligned(canvas, 64, 36, AlignCenter, AlignCenter,
                            app->enabled ? "ON" : "OFF");

    canvas_set_font(canvas, FontSecondary);
    if(app->enabled) {
        snprintf(buf, sizeof(buf), "next %lus / total %lu",
                 (unsigned long)app->next_in_sec, (unsigned long)app->event_count);
        canvas_draw_str_aligned(canvas, 64, 52, AlignCenter, AlignCenter, buf);
        snprintf(buf, sizeof(buf), "shift:%lu mouse:%lu",
                 (unsigned long)app->shift_count, (unsigned long)app->mouse_count);
        canvas_draw_str_aligned(canvas, 64, 60, AlignCenter, AlignCenter, buf);
    } else {
        canvas_draw_str_aligned(canvas, 64, 54, AlignCenter, AlignCenter,
                                "OK=start  L/R=preset");
        canvas_draw_str_aligned(canvas, 64, 62, AlignCenter, AlignCenter, "Back=exit");
    }

    furi_mutex_release(app->mutex);
}

static void input_cb(InputEvent* e, void* ctx) {
    FuriMessageQueue* q = ctx;
    furi_message_queue_put(q, e, FuriWaitForever);
}

int32_t vibe_idle_keeper_app(void* p) {
    UNUSED(p);
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(*app));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->preset_idx = 0; // AI Generation by default

    FuriMessageQueue* q = furi_message_queue_alloc(8, sizeof(InputEvent));
    ViewPort* vp = view_port_alloc();
    view_port_draw_callback_set(vp, draw_cb, app);
    view_port_input_callback_set(vp, input_cb, q);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, vp, GuiLayerFullscreen);

    app->worker = furi_thread_alloc();
    furi_thread_set_name(app->worker, "VibeWorker");
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
                if(app->preset_idx > 0) app->preset_idx--;
                break;
            case InputKeyRight:
                if(app->preset_idx < NUM_PRESETS - 1) app->preset_idx++;
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
