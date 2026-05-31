#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>

#include <bt/bt_service/bt.h>
#include <profiles/serial_profile.h>

typedef enum {
    ConnIdle,
    ConnAdvertising,
    ConnConnected,
} ConnState;

typedef struct {
    FuriMutex* mutex;
    Bt* bt;
    FuriHalBleProfileBase* profile;
    ConnState state;
    uint32_t send_count;
    char last_sent[16];
} App;

static uint16_t serial_rx_cb(SerialServiceEvent event, void* context) {
    // We don't act on data from PC for this button-style app;
    // only the FAP sends commands. But we accept and ignore.
    UNUSED(context);
    UNUSED(event);
    return 0;
}

static void bt_status_cb(BtStatus status, void* context) {
    App* app = context;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    switch(status) {
    case BtStatusAdvertising: app->state = ConnAdvertising; break;
    case BtStatusConnected:   app->state = ConnConnected; break;
    case BtStatusOff:
    case BtStatusUnavailable:
    default:                  app->state = ConnIdle; break;
    }
    furi_mutex_release(app->mutex);
}

static void send_cmd(App* app, const char* cmd) {
    if(!app->profile) return;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    ConnState s = app->state;
    furi_mutex_release(app->mutex);
    if(s != ConnConnected) return;

    size_t len = strlen(cmd);
    ble_profile_serial_tx(app->profile, (uint8_t*)cmd, (uint16_t)len);

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->send_count++;
    strncpy(app->last_sent, cmd, sizeof(app->last_sent) - 1);
    app->last_sent[sizeof(app->last_sent) - 1] = '\0';
    // strip trailing newline for display
    size_t l = strlen(app->last_sent);
    if(l > 0 && app->last_sent[l - 1] == '\n') app->last_sent[l - 1] = '\0';
    furi_mutex_release(app->mutex);
}

static const char* state_label(ConnState s) {
    switch(s) {
    case ConnConnected:   return "CONNECTED";
    case ConnAdvertising: return "Advertising";
    default:              return "Idle";
    }
}

static void draw_cb(Canvas* canvas, void* ctx) {
    App* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "BLE VPN Button");
    canvas_draw_line(canvas, 0, 13, 128, 13);

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 24, "BLE:");
    canvas_draw_str(canvas, 30, 24, state_label(app->state));

    // Big OK button hint
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 36, AlignCenter, AlignCenter,
                            "OK = TOGGLE");

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 48, AlignCenter, AlignCenter,
                            "L = DOWN   R = UP");

    char buf[40];
    if(app->send_count > 0) {
        snprintf(buf, sizeof(buf), "sent %lu: %s",
                 (unsigned long)app->send_count, app->last_sent);
        canvas_draw_str(canvas, 2, 60, buf);
    } else if(app->state != ConnConnected) {
        canvas_draw_str(canvas, 2, 60, "Pair Flipper, then connect");
    } else {
        canvas_draw_str(canvas, 2, 60, "Ready");
    }

    furi_mutex_release(app->mutex);
}

static void input_cb(InputEvent* e, void* ctx) {
    FuriMessageQueue* q = ctx;
    furi_message_queue_put(q, e, FuriWaitForever);
}

int32_t ble_vpn_button_app(void* p) {
    UNUSED(p);
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(*app));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->state = ConnIdle;

    // Bring up BLE Serial profile
    app->bt = furi_record_open(RECORD_BT);
    bt_disconnect(app->bt);
    furi_delay_ms(200);
    bt_set_status_changed_callback(app->bt, bt_status_cb, app);

    app->profile = bt_profile_start(app->bt, ble_profile_serial, NULL);
    if(app->profile) {
        // disable internal RPC so we own the channel
        ble_profile_serial_set_rpc_active(app->profile, false);
        ble_profile_serial_set_event_callback(app->profile, 128, serial_rx_cb, app);
    }

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
        if(s == FuriStatusOk && e.type == InputTypeShort) {
            switch(e.key) {
            case InputKeyOk:    send_cmd(app, "TOGGLE\n"); break;
            case InputKeyLeft:  send_cmd(app, "DOWN\n"); break;
            case InputKeyRight: send_cmd(app, "UP\n"); break;
            case InputKeyBack:  running = false; break;
            default: break;
            }
        }
        uint32_t now = furi_get_tick();
        if(now - last_redraw > 250) {
            view_port_update(vp);
            last_redraw = now;
        }
    }

    // Restore default BLE profile (back to qFlipper RPC)
    bt_set_status_changed_callback(app->bt, NULL, NULL);
    bt_disconnect(app->bt);
    furi_delay_ms(200);
    bt_profile_restore_default(app->bt);
    furi_record_close(RECORD_BT);

    view_port_enabled_set(vp, false);
    gui_remove_view_port(gui, vp);
    view_port_free(vp);
    furi_message_queue_free(q);
    furi_mutex_free(app->mutex);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
