#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_usb.h>
#include <furi_hal_usb_cdc.h>
#include <gui/gui.h>
#include <input/input.h>

#define CDC_IF 1
#define LINE_BUF_SZ 128

typedef struct {
    FuriMutex* mutex;
    FuriMessageQueue* rx_queue;
    FuriHalUsbInterface* prev_usb;
    bool usb_switched;
    bool got_data;

    float daily_usd;
    float monthly_usd;
    float budget_usd;
    uint32_t requests_today;
    uint32_t last_update_sec_ago;
    uint32_t last_update_tick;

    char line_buf[LINE_BUF_SZ];
    int line_pos;
} App;

static void rx_isr_cb(void* ctx) {
    App* app = ctx;
    uint8_t buf[CDC_DATA_SZ];
    int32_t len = furi_hal_cdc_receive(CDC_IF, buf, sizeof(buf));
    for(int32_t i = 0; i < len; i++) {
        furi_message_queue_put(app->rx_queue, &buf[i], 0);
    }
}

static CdcCallbacks cdc_cbs = {
    .tx_ep_callback = NULL,
    .rx_ep_callback = rx_isr_cb,
    .state_callback = NULL,
    .ctrl_line_callback = NULL,
    .config_callback = NULL,
};

static void parse_line(App* app, const char* line) {
    // Format: TOKENS,<daily_usd>,<monthly_usd>,<budget_usd>,<requests_today>
    if(strncmp(line, "TOKENS,", 7) != 0) return;
    float d = 0, m = 0, b = 0;
    unsigned int rq = 0;
    if(sscanf(line + 7, "%f,%f,%f,%u", &d, &m, &b, &rq) == 4) {
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->daily_usd = d;
        app->monthly_usd = m;
        app->budget_usd = b;
        app->requests_today = rq;
        app->got_data = true;
        app->last_update_tick = furi_get_tick();
        furi_mutex_release(app->mutex);
    }
}

static void draw_cb(Canvas* canvas, void* ctx) {
    App* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Token Tracker");
    canvas_draw_line(canvas, 0, 13, 128, 13);

    canvas_set_font(canvas, FontSecondary);
    char buf[48];

    if(!app->got_data) {
        canvas_draw_str_aligned(canvas, 64, 26, AlignCenter, AlignCenter,
                                "Waiting for daemon");
        canvas_draw_str_aligned(canvas, 64, 40, AlignCenter, AlignCenter,
                                "Run on PC:");
        canvas_draw_str_aligned(canvas, 64, 52, AlignCenter, AlignCenter,
                                "python tracker.py");
    } else {
        // Daily bar
        snprintf(buf, sizeof(buf), "Day: $%.2f / $%.0f",
                 (double)app->daily_usd, (double)app->budget_usd);
        canvas_draw_str(canvas, 2, 23, buf);
        canvas_draw_frame(canvas, 2, 26, 124, 8);
        float pct = (app->budget_usd > 0) ? (app->daily_usd / app->budget_usd) : 0;
        if(pct > 1.0f) pct = 1.0f;
        int fill = (int)(pct * 122);
        canvas_draw_box(canvas, 3, 27, fill, 6);
        if(pct >= 1.0f) {
            canvas_draw_str(canvas, 100, 23, "OVER!");
        }

        // Month total
        snprintf(buf, sizeof(buf), "Month: $%.2f", (double)app->monthly_usd);
        canvas_draw_str(canvas, 2, 44, buf);

        // Requests + freshness
        snprintf(buf, sizeof(buf), "Reqs today: %lu",
                 (unsigned long)app->requests_today);
        canvas_draw_str(canvas, 2, 54, buf);

        uint32_t age_sec = (furi_get_tick() - app->last_update_tick) / 1000;
        snprintf(buf, sizeof(buf), "upd %lus ago", (unsigned long)age_sec);
        canvas_draw_str(canvas, 2, 62, buf);
    }

    furi_mutex_release(app->mutex);
}

static void input_cb(InputEvent* e, void* ctx) {
    FuriMessageQueue* q = ctx;
    furi_message_queue_put(q, e, FuriWaitForever);
}

int32_t token_tracker_app(void* p) {
    UNUSED(p);
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(*app));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->rx_queue = furi_message_queue_alloc(256, sizeof(uint8_t));
    app->budget_usd = 20.0f; // sensible default if no daemon

    app->prev_usb = furi_hal_usb_get_config();
    furi_hal_usb_unlock();
    if(furi_hal_usb_set_config(&usb_cdc_dual, NULL)) {
        app->usb_switched = true;
        furi_hal_cdc_set_callbacks(CDC_IF, &cdc_cbs, app);
    }

    FuriMessageQueue* input_q = furi_message_queue_alloc(8, sizeof(InputEvent));
    ViewPort* vp = view_port_alloc();
    view_port_draw_callback_set(vp, draw_cb, app);
    view_port_input_callback_set(vp, input_cb, input_q);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, vp, GuiLayerFullscreen);

    InputEvent e;
    bool running = true;
    uint32_t last_redraw = 0;

    while(running) {
        uint8_t byte;
        while(furi_message_queue_get(app->rx_queue, &byte, 0) == FuriStatusOk) {
            if(byte == '\n' || byte == '\r') {
                if(app->line_pos > 0) {
                    app->line_buf[app->line_pos] = '\0';
                    parse_line(app, app->line_buf);
                    app->line_pos = 0;
                }
            } else if(app->line_pos < LINE_BUF_SZ - 1) {
                app->line_buf[app->line_pos++] = (char)byte;
            } else {
                app->line_pos = 0;
            }
        }

        if(furi_message_queue_get(input_q, &e, 50) == FuriStatusOk) {
            if(e.type == InputTypeShort && e.key == InputKeyBack) {
                running = false;
            }
        }

        uint32_t now = furi_get_tick();
        if(now - last_redraw > 250) {
            view_port_update(vp);
            last_redraw = now;
        }
    }

    if(app->usb_switched) {
        furi_hal_cdc_set_callbacks(CDC_IF, NULL, NULL);
        furi_hal_usb_set_config(app->prev_usb, NULL);
    }

    view_port_enabled_set(vp, false);
    gui_remove_view_port(gui, vp);
    view_port_free(vp);
    furi_message_queue_free(input_q);
    furi_message_queue_free(app->rx_queue);
    furi_mutex_free(app->mutex);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
