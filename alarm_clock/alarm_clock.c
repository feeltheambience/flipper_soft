#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_speaker.h>
#include <furi_hal_vibro.h>
#include <furi_hal_rtc.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification_messages.h>

typedef enum {
    StateIdle,
    StateRinging,
} AppState;

typedef struct {
    FuriMutex* mutex;
    AppState state;
    uint8_t alarm_hour;
    uint8_t alarm_minute;
    bool alarm_enabled;
    uint8_t cursor;            // 0 = hours, 1 = minutes
    bool triggered_this_minute;
    DateTime now;
} AlarmApp;

static void draw_callback(Canvas* canvas, void* ctx) {
    AlarmApp* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);

    canvas_clear(canvas);

    if(app->state == StateRinging) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 18, AlignCenter, AlignCenter, "!!! WAKE UP !!!");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 38, AlignCenter, AlignCenter, "Press ANY key");
        canvas_draw_str_aligned(canvas, 64, 50, AlignCenter, AlignCenter, "to stop");
    } else {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 8, AlignCenter, AlignCenter, "Alarm Clock");
        canvas_draw_line(canvas, 0, 13, 128, 13);

        canvas_set_font(canvas, FontSecondary);
        char buf[32];
        snprintf(
            buf,
            sizeof(buf),
            "Now: %02u:%02u:%02u",
            app->now.hour,
            app->now.minute,
            app->now.second);
        canvas_draw_str_aligned(canvas, 64, 22, AlignCenter, AlignCenter, buf);

        canvas_set_font(canvas, FontBigNumbers);
        snprintf(buf, sizeof(buf), "%02u:%02u", app->alarm_hour, app->alarm_minute);
        canvas_draw_str_aligned(canvas, 64, 42, AlignCenter, AlignCenter, buf);

        // cursor underline
        if(app->cursor == 0) {
            canvas_draw_line(canvas, 38, 54, 60, 54);
        } else {
            canvas_draw_line(canvas, 68, 54, 90, 54);
        }

        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(
            canvas,
            64,
            62,
            AlignCenter,
            AlignCenter,
            app->alarm_enabled ? "[ON]   OK = disable" : "[OFF]  OK = enable");
    }

    furi_mutex_release(app->mutex);
}

static void input_callback(InputEvent* event, void* ctx) {
    FuriMessageQueue* q = ctx;
    furi_message_queue_put(q, event, FuriWaitForever);
}

static void start_ringing(AlarmApp* app, bool* speaker_acquired) {
    app->state = StateRinging;
    if(furi_hal_speaker_acquire(1000)) {
        *speaker_acquired = true;
    }
}

static void stop_ringing(AlarmApp* app, bool* speaker_acquired) {
    furi_hal_vibro_on(false);
    if(*speaker_acquired) {
        furi_hal_speaker_stop();
        furi_hal_speaker_release();
        *speaker_acquired = false;
    }
    app->state = StateIdle;
    app->alarm_enabled = false; // disarm after dismiss
}

int32_t alarm_clock_app(void* p) {
    UNUSED(p);

    AlarmApp* app = malloc(sizeof(AlarmApp));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->state = StateIdle;
    app->alarm_hour = 7;
    app->alarm_minute = 0;
    app->alarm_enabled = false;
    app->cursor = 0;
    app->triggered_this_minute = false;

    FuriMessageQueue* queue = furi_message_queue_alloc(8, sizeof(InputEvent));

    ViewPort* vp = view_port_alloc();
    view_port_draw_callback_set(vp, draw_callback, app);
    view_port_input_callback_set(vp, input_callback, queue);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, vp, GuiLayerFullscreen);

    NotificationApp* notif = furi_record_open(RECORD_NOTIFICATION);

    bool running = true;
    bool speaker_acquired = false;
    bool tone_high = true;
    uint32_t last_tone_tick = 0;
    uint32_t last_backlight_tick = 0;

    InputEvent event;

    while(running) {
        FuriStatus status = furi_message_queue_get(queue, &event, 50);

        furi_mutex_acquire(app->mutex, FuriWaitForever);

        furi_hal_rtc_get_datetime(&app->now);

        // arm/disarm trigger detection
        if(app->state == StateIdle && app->alarm_enabled &&
           app->now.hour == app->alarm_hour && app->now.minute == app->alarm_minute) {
            if(!app->triggered_this_minute) {
                app->triggered_this_minute = true;
                start_ringing(app, &speaker_acquired);
            }
        } else if(app->now.minute != app->alarm_minute) {
            app->triggered_this_minute = false;
        }

        // ringing actions
        if(app->state == StateRinging) {
            furi_hal_vibro_on(true);

            uint32_t now_tick = furi_get_tick();
            if(speaker_acquired && (now_tick - last_tone_tick) > 180) {
                tone_high = !tone_high;
                furi_hal_speaker_start(tone_high ? 1400.0f : 900.0f, 1.0f);
                last_tone_tick = now_tick;
            }
            if((now_tick - last_backlight_tick) > 400) {
                notification_message(notif, &sequence_display_backlight_on);
                last_backlight_tick = now_tick;
            }
        }

        // input
        if(status == FuriStatusOk) {
            if(app->state == StateRinging) {
                if(event.type == InputTypePress || event.type == InputTypeShort) {
                    stop_ringing(app, &speaker_acquired);
                }
            } else if(event.type == InputTypeShort || event.type == InputTypeRepeat) {
                switch(event.key) {
                case InputKeyUp:
                    if(app->cursor == 0)
                        app->alarm_hour = (app->alarm_hour + 1) % 24;
                    else
                        app->alarm_minute = (app->alarm_minute + 1) % 60;
                    app->triggered_this_minute = false;
                    break;
                case InputKeyDown:
                    if(app->cursor == 0)
                        app->alarm_hour = (app->alarm_hour + 23) % 24;
                    else
                        app->alarm_minute = (app->alarm_minute + 59) % 60;
                    app->triggered_this_minute = false;
                    break;
                case InputKeyLeft:
                case InputKeyRight:
                    app->cursor = app->cursor ? 0 : 1;
                    break;
                case InputKeyOk:
                    app->alarm_enabled = !app->alarm_enabled;
                    if(app->alarm_enabled) {
                        // if armed inside the same minute, don't fire immediately
                        if(app->now.hour == app->alarm_hour &&
                           app->now.minute == app->alarm_minute) {
                            app->triggered_this_minute = true;
                        } else {
                            app->triggered_this_minute = false;
                        }
                    }
                    break;
                case InputKeyBack:
                    running = false;
                    break;
                default:
                    break;
                }
            }
        }

        furi_mutex_release(app->mutex);
        view_port_update(vp);
    }

    // cleanup
    if(speaker_acquired) {
        furi_hal_speaker_stop();
        furi_hal_speaker_release();
    }
    furi_hal_vibro_on(false);

    view_port_enabled_set(vp, false);
    gui_remove_view_port(gui, vp);
    view_port_free(vp);
    furi_message_queue_free(queue);
    furi_mutex_free(app->mutex);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);
    free(app);

    return 0;
}
