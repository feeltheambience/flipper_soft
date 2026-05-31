#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_usb.h>
#include <furi_hal_usb_cdc.h>
#include <gui/gui.h>
#include <input/input.h>

#define CDC_IF 1
#define LINE_BUF_SZ 128
#define MAX_NAME 24

typedef enum {
    VpnUnknown,
    VpnConnected,
    VpnDisconnected,
    VpnReconnecting,
    VpnError,
} VpnStatus;

typedef struct {
    FuriMutex* mutex;
    FuriMessageQueue* rx_queue;
    FuriHalUsbInterface* prev_usb;
    bool usb_switched;
    bool got_data;

    VpnStatus status;
    char profile_name[MAX_NAME];
    uint32_t uptime_sec;
    uint32_t reconnect_count;
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

static VpnStatus parse_status(const char* s) {
    if(strncmp(s, "OK", 2) == 0 || strncmp(s, "CONNECTED", 9) == 0) return VpnConnected;
    if(strncmp(s, "DOWN", 4) == 0 || strncmp(s, "DISCONNECTED", 12) == 0) return VpnDisconnected;
    if(strncmp(s, "RECON", 5) == 0) return VpnReconnecting;
    if(strncmp(s, "ERR", 3) == 0) return VpnError;
    return VpnUnknown;
}

static void parse_line(App* app, const char* line) {
    // Format: VPN,<STATUS>,<profile>,<uptime_sec>,<reconnect_count>
    if(strncmp(line, "VPN,", 4) != 0) return;
    char status[16] = {0};
    char prof[MAX_NAME] = {0};
    unsigned int uptime = 0, reconns = 0;
    if(sscanf(line + 4, "%15[^,],%23[^,],%u,%u",
              status, prof, &uptime, &reconns) >= 2) {
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->status = parse_status(status);
        strncpy(app->profile_name, prof, MAX_NAME - 1);
        app->profile_name[MAX_NAME - 1] = '\0';
        app->uptime_sec = uptime;
        app->reconnect_count = reconns;
        app->got_data = true;
        app->last_update_tick = furi_get_tick();
        furi_mutex_release(app->mutex);
    }
}

static const char* status_label(VpnStatus s) {
    switch(s) {
    case VpnConnected: return "CONNECTED";
    case VpnDisconnected: return "DISCONNECTED";
    case VpnReconnecting: return "RECONNECTING";
    case VpnError: return "ERROR";
    default: return "?";
    }
}

static void draw_cb(Canvas* canvas, void* ctx) {
    App* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "VPN Keeper");
    canvas_draw_line(canvas, 0, 13, 128, 13);

    canvas_set_font(canvas, FontSecondary);

    if(!app->got_data) {
        canvas_draw_str_aligned(canvas, 64, 26, AlignCenter, AlignCenter,
                                "Waiting for daemon");
        canvas_draw_str_aligned(canvas, 64, 40, AlignCenter, AlignCenter,
                                "On PC, run:");
        canvas_draw_str_aligned(canvas, 64, 52, AlignCenter, AlignCenter,
                                "python keeper.py \"VpnName\"");
    } else {
        char buf[48];

        // Visual status box
        const char* sl = status_label(app->status);
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 24, AlignCenter, AlignCenter, sl);

        // Solid/empty rectangle indicator
        int ind_x = 4, ind_y = 18, ind_w = 12, ind_h = 12;
        canvas_draw_frame(canvas, ind_x, ind_y, ind_w, ind_h);
        if(app->status == VpnConnected) {
            canvas_draw_box(canvas, ind_x + 2, ind_y + 2, ind_w - 4, ind_h - 4);
        } else if(app->status == VpnReconnecting) {
            uint32_t blink = (furi_get_tick() / 300) & 1;
            if(blink) {
                canvas_draw_box(canvas, ind_x + 2, ind_y + 2, ind_w - 4, ind_h - 4);
            }
        }

        canvas_set_font(canvas, FontSecondary);
        snprintf(buf, sizeof(buf), "Profile: %s", app->profile_name);
        canvas_draw_str(canvas, 2, 40, buf);

        uint32_t hh = app->uptime_sec / 3600;
        uint32_t mm = (app->uptime_sec % 3600) / 60;
        uint32_t ss = app->uptime_sec % 60;
        if(app->status == VpnConnected) {
            snprintf(buf, sizeof(buf), "Up: %luh %lum %lus",
                     (unsigned long)hh, (unsigned long)mm, (unsigned long)ss);
        } else {
            snprintf(buf, sizeof(buf), "Down");
        }
        canvas_draw_str(canvas, 2, 50, buf);

        snprintf(buf, sizeof(buf), "Reconnects: %lu",
                 (unsigned long)app->reconnect_count);
        canvas_draw_str(canvas, 2, 60, buf);

        uint32_t age = (furi_get_tick() - app->last_update_tick) / 1000;
        snprintf(buf, sizeof(buf), "%lus", (unsigned long)age);
        canvas_draw_str(canvas, 110, 60, buf);
    }

    furi_mutex_release(app->mutex);
}

static void input_cb(InputEvent* e, void* ctx) {
    FuriMessageQueue* q = ctx;
    furi_message_queue_put(q, e, FuriWaitForever);
}

int32_t vpn_keeper_app(void* p) {
    UNUSED(p);
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(*app));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->rx_queue = furi_message_queue_alloc(256, sizeof(uint8_t));
    strncpy(app->profile_name, "(no data)", MAX_NAME - 1);

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
