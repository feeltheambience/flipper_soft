#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_usb.h>
#include <furi_hal_usb_cdc.h>
#include <gui/gui.h>
#include <input/input.h>

#define CDC_IF 1            // use second CDC interface; first stays for RPC
#define RX_BUF_SZ 128
#define LINE_BUF_SZ 96

typedef struct {
    FuriMutex* mutex;
    FuriMessageQueue* rx_queue; // bytes from ISR → main
    FuriHalUsbInterface* prev_usb;
    bool usb_switched;
    bool got_data;
    uint32_t cpu;
    uint32_t ram;
    uint32_t disk;
    uint32_t net_kbps;
    char line_buf[LINE_BUF_SZ];
    int line_pos;
} App;

static void rx_isr_cb(void* ctx) {
    App* app = ctx;
    uint8_t buf[CDC_DATA_SZ];
    int32_t len = furi_hal_cdc_receive(CDC_IF, buf, sizeof(buf));
    for(int32_t i = 0; i < len; i++) {
        // Best-effort: drop if queue full
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
    // Expected: "STATS,cpu,ram,disk,net_kbps"
    if(strncmp(line, "STATS,", 6) != 0) return;
    unsigned int cpu = 0, ram = 0, disk = 0, net = 0;
    if(sscanf(line + 6, "%u,%u,%u,%u", &cpu, &ram, &disk, &net) == 4) {
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->cpu = cpu > 100 ? 100 : cpu;
        app->ram = ram > 100 ? 100 : ram;
        app->disk = disk > 100 ? 100 : disk;
        app->net_kbps = net;
        app->got_data = true;
        furi_mutex_release(app->mutex);
    }
}

static void draw_bar(Canvas* canvas, int x, int y, int w, int h, uint32_t pct, const char* label, const char* value) {
    canvas_draw_str(canvas, x, y - 1, label);
    canvas_draw_frame(canvas, x + 22, y - 7, w, h);
    int fill = (int)((pct * (w - 2)) / 100);
    if(fill < 0) fill = 0;
    if(fill > w - 2) fill = w - 2;
    canvas_draw_box(canvas, x + 23, y - 6, fill, h - 2);
    canvas_draw_str(canvas, x + 22 + w + 3, y - 1, value);
}

static void draw_cb(Canvas* canvas, void* ctx) {
    App* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "System Dashboard");
    canvas_draw_line(canvas, 0, 13, 128, 13);

    canvas_set_font(canvas, FontSecondary);

    if(!app->got_data) {
        canvas_draw_str_aligned(canvas, 64, 30, AlignCenter, AlignCenter,
                                "Waiting for PC daemon");
        canvas_draw_str_aligned(canvas, 64, 44, AlignCenter, AlignCenter,
                                "Run: python dashboard.py");
        canvas_draw_str_aligned(canvas, 64, 56, AlignCenter, AlignCenter,
                                "on the connected PC");
    } else {
        char val[16];
        snprintf(val, sizeof(val), "%lu%%", (unsigned long)app->cpu);
        draw_bar(canvas, 2, 26, 60, 8, app->cpu, "CPU", val);
        snprintf(val, sizeof(val), "%lu%%", (unsigned long)app->ram);
        draw_bar(canvas, 2, 38, 60, 8, app->ram, "RAM", val);
        snprintf(val, sizeof(val), "%lu%%", (unsigned long)app->disk);
        draw_bar(canvas, 2, 50, 60, 8, app->disk, "DSK", val);
        // Network — show kbps as text (no good cap to bar against)
        snprintf(val, sizeof(val), "NET: %lu KB/s", (unsigned long)app->net_kbps);
        canvas_draw_str(canvas, 2, 62, val);
    }

    furi_mutex_release(app->mutex);
}

static void input_cb(InputEvent* e, void* ctx) {
    FuriMessageQueue* q = ctx;
    furi_message_queue_put(q, e, FuriWaitForever);
}

int32_t system_dashboard_app(void* p) {
    UNUSED(p);
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(*app));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->rx_queue = furi_message_queue_alloc(256, sizeof(uint8_t));

    // Switch USB to dual CDC; interface 0 stays for qFlipper RPC,
    // interface 1 is ours.
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
        // Drain RX bytes (non-blocking)
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
                app->line_pos = 0; // overflow, drop line
            }
        }

        // Process input with timeout
        if(furi_message_queue_get(input_q, &e, 50) == FuriStatusOk) {
            if(e.type == InputTypeShort && e.key == InputKeyBack) {
                running = false;
            }
        }

        uint32_t now = furi_get_tick();
        if(now - last_redraw > 200) {
            view_port_update(vp);
            last_redraw = now;
        }
    }

    // Restore USB
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
