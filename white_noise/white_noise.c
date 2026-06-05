#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_speaker.h>
#include <furi_hal_random.h>
#include <gui/gui.h>
#include <input/input.h>

// mode: 0 = noise band, 1 = ocean (slow amplitude swell), 2 = rain (sparse clicks)
typedef struct {
    const char* name;
    uint16_t f_lo;
    uint16_t f_hi;
    uint8_t step_ms;
    float vol;
    uint8_t mode;
} NoisePreset;

static const NoisePreset PRESETS[] = {
    {"White",  300, 6000, 4,  0.80f, 0},
    {"Pink",   200, 3000, 5,  0.85f, 0},
    {"Brown",   80, 1200, 8,  1.00f, 0},
    {"Fan",    120,  420, 14, 0.90f, 0},
    {"Ocean",  200, 2200, 6,  0.95f, 1},
    {"Rain",  2000, 6000, 0,  0.70f, 2},
};
#define NUM_PRESETS (sizeof(PRESETS) / sizeof(PRESETS[0]))

// Sleep timer presets (minutes); 0 = off
static const uint16_t TIMERS[] = {0, 15, 30, 45, 60, 90};
static const char* TIMER_LABELS[] = {"Off", "15m", "30m", "45m", "60m", "90m"};
#define NUM_TIMERS (sizeof(TIMERS) / sizeof(TIMERS[0]))

typedef struct {
    FuriMutex* mutex;
    bool playing;
    bool speaker_acquired;
    uint32_t preset_idx;
    uint32_t timer_idx;
    uint32_t remaining_sec; // counts down when playing & timer set
    bool timer_done;
    FuriThread* worker;
    volatile bool stop;
} App;

static int32_t worker_thread(void* ctx) {
    App* app = ctx;
    uint32_t phase = 0;

    while(!app->stop) {
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        bool playing = app->playing;
        bool acquired = app->speaker_acquired;
        NoisePreset p = PRESETS[app->preset_idx];
        furi_mutex_release(app->mutex);

        if(!playing || !acquired) {
            furi_hal_speaker_stop();
            furi_delay_ms(40);
            continue;
        }

        if(p.mode == 2) {
            // Rain: mostly silence, occasional short high-freq click
            uint32_t r = furi_hal_random_get() % 100;
            if(r < 18) {
                uint16_t span = p.f_hi - p.f_lo;
                float freq = p.f_lo + (furi_hal_random_get() % span);
                furi_hal_speaker_start(freq, p.vol);
                furi_delay_ms(12 + (furi_hal_random_get() % 28));
                furi_hal_speaker_stop();
            } else {
                furi_delay_ms(18 + (furi_hal_random_get() % 45));
            }
        } else {
            uint16_t span = p.f_hi - p.f_lo;
            float freq = p.f_lo + (furi_hal_random_get() % span);
            float vol = p.vol;
            if(p.mode == 1) {
                // Ocean: slow triangular amplitude envelope (~6 sec period)
                phase = (phase + 1) % 1000;
                float tri = (phase < 500) ? (phase / 500.0f) : ((1000 - phase) / 500.0f);
                vol = p.vol * (0.30f + 0.70f * tri);
            }
            furi_hal_speaker_start(freq, vol);
            furi_delay_ms(p.step_ms);
        }
    }

    furi_hal_speaker_stop();
    return 0;
}

static void start_playing(App* app) {
    if(!app->speaker_acquired) {
        if(furi_hal_speaker_acquire(1000)) {
            app->speaker_acquired = true;
        } else {
            return;
        }
    }
    app->playing = true;
    app->timer_done = false;
    if(TIMERS[app->timer_idx] > 0) {
        app->remaining_sec = (uint32_t)TIMERS[app->timer_idx] * 60;
    } else {
        app->remaining_sec = 0;
    }
}

static void stop_playing(App* app) {
    app->playing = false;
    if(app->speaker_acquired) {
        furi_hal_speaker_stop();
        furi_hal_speaker_release();
        app->speaker_acquired = false;
    }
}

static void draw_cb(Canvas* canvas, void* ctx) {
    App* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "White Noise");
    canvas_draw_line(canvas, 0, 13, 128, 13);

    canvas_set_font(canvas, FontSecondary);
    char buf[40];
    snprintf(buf, sizeof(buf), "< %s >", PRESETS[app->preset_idx].name);
    canvas_draw_str_aligned(canvas, 64, 23, AlignCenter, AlignCenter, buf);

    canvas_set_font(canvas, FontBigNumbers);
    canvas_draw_str_aligned(canvas, 64, 38, AlignCenter, AlignCenter,
                            app->playing ? "ON" : "OFF");

    canvas_set_font(canvas, FontSecondary);
    if(app->timer_done) {
        canvas_draw_str_aligned(canvas, 64, 52, AlignCenter, AlignCenter,
                                "Sleep timer done");
    } else if(app->playing && app->remaining_sec > 0) {
        uint32_t mm = app->remaining_sec / 60;
        uint32_t ss = app->remaining_sec % 60;
        snprintf(buf, sizeof(buf), "Sleep in %02lu:%02lu",
                 (unsigned long)mm, (unsigned long)ss);
        canvas_draw_str_aligned(canvas, 64, 52, AlignCenter, AlignCenter, buf);
    } else {
        snprintf(buf, sizeof(buf), "Timer: %s", TIMER_LABELS[app->timer_idx]);
        canvas_draw_str_aligned(canvas, 64, 52, AlignCenter, AlignCenter, buf);
    }

    canvas_draw_str_aligned(canvas, 64, 62, AlignCenter, AlignCenter,
                            "OK play  L/R sound  U/D timer");

    furi_mutex_release(app->mutex);
}

static void input_cb(InputEvent* e, void* ctx) {
    FuriMessageQueue* q = ctx;
    furi_message_queue_put(q, e, FuriWaitForever);
}

int32_t white_noise_app(void* p) {
    UNUSED(p);
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(*app));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->preset_idx = 0;
    app->timer_idx = 2; // 30 min default

    FuriMessageQueue* q = furi_message_queue_alloc(8, sizeof(InputEvent));
    ViewPort* vp = view_port_alloc();
    view_port_draw_callback_set(vp, draw_cb, app);
    view_port_input_callback_set(vp, input_cb, q);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, vp, GuiLayerFullscreen);

    app->worker = furi_thread_alloc();
    furi_thread_set_name(app->worker, "NoiseWorker");
    furi_thread_set_stack_size(app->worker, 1024);
    furi_thread_set_context(app->worker, app);
    furi_thread_set_callback(app->worker, worker_thread);
    furi_thread_start(app->worker);

    InputEvent e;
    bool running = true;
    uint32_t last_tick = furi_get_tick();
    uint32_t last_redraw = 0;

    while(running) {
        FuriStatus s = furi_message_queue_get(q, &e, 100);
        if(s == FuriStatusOk && (e.type == InputTypeShort || e.type == InputTypeRepeat)) {
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            switch(e.key) {
            case InputKeyOk:
                if(app->playing) {
                    stop_playing(app);
                } else {
                    start_playing(app);
                }
                break;
            case InputKeyLeft:
                if(app->preset_idx > 0) app->preset_idx--;
                break;
            case InputKeyRight:
                if(app->preset_idx < NUM_PRESETS - 1) app->preset_idx++;
                break;
            case InputKeyUp:
                if(app->timer_idx < NUM_TIMERS - 1) app->timer_idx++;
                if(app->playing && TIMERS[app->timer_idx] > 0)
                    app->remaining_sec = (uint32_t)TIMERS[app->timer_idx] * 60;
                break;
            case InputKeyDown:
                if(app->timer_idx > 0) app->timer_idx--;
                if(app->playing && TIMERS[app->timer_idx] > 0)
                    app->remaining_sec = (uint32_t)TIMERS[app->timer_idx] * 60;
                else if(app->playing)
                    app->remaining_sec = 0;
                break;
            case InputKeyBack:
                running = false;
                break;
            default:
                break;
            }
            furi_mutex_release(app->mutex);
        }

        // 1-second tick for the sleep timer
        uint32_t now = furi_get_tick();
        if(now - last_tick >= 1000) {
            last_tick += 1000;
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            if(app->playing && app->remaining_sec > 0) {
                app->remaining_sec--;
                if(app->remaining_sec == 0) {
                    stop_playing(app);
                    app->timer_done = true;
                }
            }
            furi_mutex_release(app->mutex);
        }

        if(now - last_redraw > 200) {
            view_port_update(vp);
            last_redraw = now;
        }
    }

    app->stop = true;
    furi_thread_join(app->worker);
    furi_thread_free(app->worker);

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    stop_playing(app);
    furi_mutex_release(app->mutex);

    view_port_enabled_set(vp, false);
    gui_remove_view_port(gui, vp);
    view_port_free(vp);
    furi_message_queue_free(q);
    furi_mutex_free(app->mutex);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
