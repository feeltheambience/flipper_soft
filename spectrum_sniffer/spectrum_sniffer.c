#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_subghz.h>
#include <gui/gui.h>
#include <input/input.h>
#include <lib/subghz/devices/devices.h>
#include <lib/subghz/devices/cc1101_int/cc1101_int_interconnect.h>

#define NUM_FREQS 4
static const uint32_t freqs_hz[NUM_FREQS] = {
    315000000UL, 433920000UL, 868350000UL, 915000000UL};
static const char* freq_labels[NUM_FREQS] = {"315", "434", "868", "915"};

typedef struct {
    FuriMutex* mutex;
    FuriThread* worker;
    volatile bool stop;
    float rssi[NUM_FREQS];
    float peak[NUM_FREQS];
} SpectrumApp;

static int32_t worker_thread(void* ctx) {
    SpectrumApp* app = ctx;
    subghz_devices_init();
    const SubGhzDevice* dev = subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_INT_NAME);
    subghz_devices_begin(dev);
    subghz_devices_reset(dev);
    subghz_devices_load_preset(dev, FuriHalSubGhzPresetOok650Async, NULL);

    while(!app->stop) {
        for(int i = 0; i < NUM_FREQS && !app->stop; i++) {
            subghz_devices_idle(dev);
            subghz_devices_set_frequency(dev, freqs_hz[i]);
            subghz_devices_set_rx(dev);
            furi_delay_ms(25);
            float r = subghz_devices_get_rssi(dev);
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            app->rssi[i] = r;
            if(r > app->peak[i]) {
                app->peak[i] = r;
            } else {
                app->peak[i] -= 0.4f;
                if(app->peak[i] < -100) app->peak[i] = -100;
            }
            furi_mutex_release(app->mutex);
        }
    }

    subghz_devices_idle(dev);
    subghz_devices_sleep(dev);
    subghz_devices_end(dev);
    subghz_devices_deinit();
    return 0;
}

static void draw_cb(Canvas* canvas, void* ctx) {
    SpectrumApp* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    canvas_clear(canvas);

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 8, "Spectrum Sniffer");
    canvas_draw_str(canvas, 92, 8, "OK=reset");
    canvas_draw_line(canvas, 0, 10, 128, 10);

    const int bar_w = 24;
    const int bar_step = 32;
    const int graph_y = 14;
    const int graph_h = 36;

    for(int i = 0; i < NUM_FREQS; i++) {
        int x = 4 + i * bar_step;
        float r = app->rssi[i];
        if(r < -100) r = -100;
        if(r > -30) r = -30;
        int h = (int)((r + 100.0f) * graph_h / 70.0f);
        if(h < 0) h = 0;
        if(h > graph_h) h = graph_h;

        canvas_draw_frame(canvas, x, graph_y, bar_w, graph_h);
        canvas_draw_box(canvas, x + 1, graph_y + graph_h - h, bar_w - 2, h);

        float p = app->peak[i];
        if(p < -100) p = -100;
        if(p > -30) p = -30;
        int ph = (int)((p + 100.0f) * graph_h / 70.0f);
        if(ph >= 0 && ph <= graph_h) {
            canvas_draw_line(canvas, x, graph_y + graph_h - ph,
                             x + bar_w - 1, graph_y + graph_h - ph);
        }

        canvas_draw_str(canvas, x + 6, graph_y + graph_h + 8, freq_labels[i]);

        char buf[12];
        snprintf(buf, sizeof(buf), "%d", (int)app->rssi[i]);
        canvas_draw_str(canvas, x + 2, graph_y + graph_h + 16, buf);
    }

    furi_mutex_release(app->mutex);
}

static void input_cb(InputEvent* e, void* ctx) {
    FuriMessageQueue* q = ctx;
    furi_message_queue_put(q, e, FuriWaitForever);
}

int32_t spectrum_sniffer_app(void* p) {
    UNUSED(p);
    SpectrumApp* app = malloc(sizeof(SpectrumApp));
    memset(app, 0, sizeof(*app));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    for(int i = 0; i < NUM_FREQS; i++) {
        app->rssi[i] = -100;
        app->peak[i] = -100;
    }

    FuriMessageQueue* q = furi_message_queue_alloc(8, sizeof(InputEvent));
    ViewPort* vp = view_port_alloc();
    view_port_draw_callback_set(vp, draw_cb, app);
    view_port_input_callback_set(vp, input_cb, q);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, vp, GuiLayerFullscreen);

    app->stop = false;
    app->worker = furi_thread_alloc();
    furi_thread_set_name(app->worker, "SpectrumWorker");
    furi_thread_set_stack_size(app->worker, 2048);
    furi_thread_set_context(app->worker, app);
    furi_thread_set_callback(app->worker, worker_thread);
    furi_thread_start(app->worker);

    InputEvent e;
    bool running = true;
    uint32_t last_redraw = 0;
    while(running) {
        FuriStatus status = furi_message_queue_get(q, &e, 50);
        if(status == FuriStatusOk && e.type == InputTypeShort) {
            if(e.key == InputKeyBack) {
                running = false;
            } else if(e.key == InputKeyOk) {
                furi_mutex_acquire(app->mutex, FuriWaitForever);
                for(int i = 0; i < NUM_FREQS; i++) app->peak[i] = app->rssi[i];
                furi_mutex_release(app->mutex);
            }
        }
        uint32_t now = furi_get_tick();
        if(now - last_redraw > 80) {
            view_port_update(vp);
            last_redraw = now;
        }
    }

    app->stop = true;
    furi_thread_join(app->worker);
    furi_thread_free(app->worker);

    view_port_enabled_set(vp, false);
    gui_remove_view_port(gui, vp);
    view_port_free(vp);
    furi_message_queue_free(q);
    furi_mutex_free(app->mutex);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
