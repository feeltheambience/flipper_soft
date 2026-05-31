#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_usb.h>
#include <furi_hal_usb_hid.h>
#include <gui/gui.h>
#include <input/input.h>

// HID typing speed (per keystroke press/release/inter-key)
#define INTERKEY_DELAY 8
// Wait after Win+X, A, ALT+Y to let elevated PowerShell appear
#define POST_ELEVATE_DELAY 2500
// USB enumeration wait
#define USB_WAIT_MAX 5000

typedef enum {
    ActionEnable = 0,
    ActionDisable,
    ActionAddUser,
    ActionStatus,
    ActionExit,
    ActionCount,
} Action;

static const char* action_titles[ActionCount] = {
    "Enable RDP",
    "Disable RDP",
    "Add RDP User",
    "Show Status",
    "Exit",
};

typedef enum {
    StateMenu,
    StateConfirm,
    StateRunning,
    StateDone,
    StateError,
} AppState;

typedef struct {
    FuriMutex* mutex;
    AppState state;
    Action selected;
    char status_msg[64];
    FuriThread* worker;
    FuriHalUsbInterface* prev_usb_iface;
    volatile bool cancel;
} App;

// ----- HID helpers -----

static void hid_type_char(char c) {
    if((uint8_t)c >= sizeof(hid_asciimap) / sizeof(hid_asciimap[0])) return;
    uint16_t key = hid_asciimap[(uint8_t)c];
    if(key == HID_KEYBOARD_NONE) return;
    furi_hal_hid_kb_press(key);
    furi_delay_ms(INTERKEY_DELAY);
    furi_hal_hid_kb_release_all();
    furi_delay_ms(INTERKEY_DELAY);
}

static void hid_type_str(const char* s) {
    while(*s) {
        hid_type_char(*s++);
    }
}

static void hid_press(uint16_t k) {
    furi_hal_hid_kb_press(k);
    furi_delay_ms(20);
    furi_hal_hid_kb_release_all();
    furi_delay_ms(20);
}

// ----- Open elevated PowerShell via Win+X, A, then UAC ALT+Y -----

static void open_admin_shell(void) {
    hid_press(KEY_MOD_LEFT_GUI | HID_KEYBOARD_X);
    furi_delay_ms(700);
    hid_press(HID_KEYBOARD_A);
    furi_delay_ms(1500);
    hid_press(KEY_MOD_LEFT_ALT | HID_KEYBOARD_Y);
    furi_delay_ms(POST_ELEVATE_DELAY);
}

// ----- PowerShell payloads -----

static const char* SCRIPT_ENABLE =
    "Set-ItemProperty -Path 'HKLM:\\System\\CurrentControlSet\\Control\\Terminal Server' "
    "-Name fDenyTSConnections -Value 0; "
    "Enable-NetFirewallRule -DisplayGroup 'Remote Desktop'; "
    "Set-Service -Name TermService -StartupType Automatic; "
    "Start-Service TermService; "
    "Write-Host ('USER: ' + $env:USERNAME); "
    "Write-Host ('HOST: ' + $env:COMPUTERNAME); "
    "(Get-NetIPAddress -AddressFamily IPv4 | "
    "Where-Object {$_.PrefixOrigin -in 'Dhcp','Manual'}).IPAddress";

static const char* SCRIPT_DISABLE =
    "Set-ItemProperty -Path 'HKLM:\\System\\CurrentControlSet\\Control\\Terminal Server' "
    "-Name fDenyTSConnections -Value 1; "
    "Disable-NetFirewallRule -DisplayGroup 'Remote Desktop'; "
    "Stop-Service TermService -Force; "
    "Set-Service -Name TermService -StartupType Manual; "
    "Write-Host 'RDP disabled.'";

static const char* SCRIPT_ADDUSER =
    "$pw = ConvertTo-SecureString 'RDP_Flipper_2024!' -AsPlainText -Force; "
    "if(-not (Get-LocalUser -Name 'flipperuser' -ErrorAction SilentlyContinue)){ "
    "New-LocalUser -Name 'flipperuser' -Password $pw -FullName 'Flipper RDP User' "
    "-Description 'Added by RDP Manager' -PasswordNeverExpires "
    "} else { Set-LocalUser -Name 'flipperuser' -Password $pw }; "
    "Add-LocalGroupMember -Group 'Remote Desktop Users' -Member 'flipperuser' "
    "-ErrorAction SilentlyContinue; "
    "Write-Host 'Login: flipperuser  Pass: RDP_Flipper_2024!'";

static const char* SCRIPT_STATUS =
    "Get-Service TermService | Format-Table Name, Status, StartType; "
    "Get-NetFirewallRule -DisplayGroup 'Remote Desktop' | "
    "Where-Object {$_.Enabled -eq 'True'} | "
    "Select-Object DisplayName, Direction, Action | Format-Table; "
    "Write-Host ('USER: ' + $env:USERNAME); "
    "Write-Host ('HOST: ' + $env:COMPUTERNAME); "
    "Get-NetIPAddress -AddressFamily IPv4 | Format-Table InterfaceAlias, IPAddress";

static const char* script_for(Action a) {
    switch(a) {
    case ActionEnable: return SCRIPT_ENABLE;
    case ActionDisable: return SCRIPT_DISABLE;
    case ActionAddUser: return SCRIPT_ADDUSER;
    case ActionStatus: return SCRIPT_STATUS;
    default: return NULL;
    }
}

// ----- Worker thread: switch USB to HID, run script, restore USB -----

static void set_status(App* app, const char* msg) {
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    snprintf(app->status_msg, sizeof(app->status_msg), "%s", msg);
    furi_mutex_release(app->mutex);
}

static void set_state(App* app, AppState s) {
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->state = s;
    furi_mutex_release(app->mutex);
}

static int32_t worker_thread(void* ctx) {
    App* app = ctx;

    app->prev_usb_iface = furi_hal_usb_get_config();
    furi_hal_usb_unlock();

    if(!furi_hal_usb_set_config(&usb_hid, NULL)) {
        set_status(app, "USB switch failed");
        set_state(app, StateError);
        return 0;
    }

    set_status(app, "Waiting for host...");

    int waited = 0;
    while(!furi_hal_hid_is_connected() && waited < USB_WAIT_MAX && !app->cancel) {
        furi_delay_ms(100);
        waited += 100;
    }

    if(app->cancel) {
        furi_hal_usb_set_config(app->prev_usb_iface, NULL);
        return 0;
    }

    if(!furi_hal_hid_is_connected()) {
        set_status(app, "Host did not respond");
        furi_hal_usb_set_config(app->prev_usb_iface, NULL);
        set_state(app, StateError);
        return 0;
    }

    furi_delay_ms(1000); // host settle

    set_status(app, "Opening admin shell");
    open_admin_shell();
    if(app->cancel) {
        furi_hal_usb_set_config(app->prev_usb_iface, NULL);
        return 0;
    }

    set_status(app, "Typing script...");
    const char* script = script_for(app->selected);
    if(script) {
        hid_type_str(script);
        furi_delay_ms(200);
        hid_press(HID_KEYBOARD_RETURN);
    }

    furi_delay_ms(500);
    furi_hal_usb_set_config(app->prev_usb_iface, NULL);

    set_status(app, "Done. Check PC screen.");
    set_state(app, StateDone);
    return 0;
}

// ----- UI -----

static void draw_cb(Canvas* canvas, void* ctx) {
    App* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "RDP Manager");
    canvas_draw_line(canvas, 0, 13, 128, 13);

    canvas_set_font(canvas, FontSecondary);

    if(app->state == StateMenu) {
        const int line_h = 10;
        for(int i = 0; i < ActionCount; i++) {
            int y = 23 + i * line_h;
            if(i == (int)app->selected) {
                canvas_draw_box(canvas, 0, y - 8, 128, line_h);
                canvas_set_color(canvas, ColorWhite);
            }
            canvas_draw_str(canvas, 4, y, action_titles[i]);
            if(i == (int)app->selected) canvas_set_color(canvas, ColorBlack);
        }
    } else if(app->state == StateConfirm) {
        canvas_draw_str_aligned(canvas, 64, 22, AlignCenter, AlignCenter,
                                "Plug Flipper into PC USB");
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 38, AlignCenter, AlignCenter,
                                action_titles[app->selected]);
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 56, AlignCenter, AlignCenter,
                                "OK = run   Back = cancel");
    } else if(app->state == StateRunning) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 26, AlignCenter, AlignCenter, "Running");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 42, AlignCenter, AlignCenter, app->status_msg);
        canvas_draw_str_aligned(canvas, 64, 58, AlignCenter, AlignCenter, "Back = cancel");
    } else if(app->state == StateDone) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 26, AlignCenter, AlignCenter, "Done");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 42, AlignCenter, AlignCenter, app->status_msg);
        canvas_draw_str_aligned(canvas, 64, 58, AlignCenter, AlignCenter, "Any key = menu");
    } else if(app->state == StateError) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 26, AlignCenter, AlignCenter, "Error");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 42, AlignCenter, AlignCenter, app->status_msg);
        canvas_draw_str_aligned(canvas, 64, 58, AlignCenter, AlignCenter, "Any key = menu");
    }

    furi_mutex_release(app->mutex);
}

static void input_cb(InputEvent* e, void* ctx) {
    FuriMessageQueue* q = ctx;
    furi_message_queue_put(q, e, FuriWaitForever);
}

static void start_worker(App* app) {
    app->cancel = false;
    snprintf(app->status_msg, sizeof(app->status_msg), "Connecting USB...");
    app->state = StateRunning;
    app->worker = furi_thread_alloc();
    furi_thread_set_name(app->worker, "RDPWorker");
    furi_thread_set_stack_size(app->worker, 2048);
    furi_thread_set_context(app->worker, app);
    furi_thread_set_callback(app->worker, worker_thread);
    furi_thread_start(app->worker);
}

static void stop_worker(App* app) {
    if(app->worker) {
        app->cancel = true;
        furi_thread_join(app->worker);
        furi_thread_free(app->worker);
        app->worker = NULL;
    }
}

int32_t rdp_manager_app(void* p) {
    UNUSED(p);
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(*app));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->state = StateMenu;
    app->selected = 0;

    FuriMessageQueue* q = furi_message_queue_alloc(8, sizeof(InputEvent));
    ViewPort* vp = view_port_alloc();
    view_port_draw_callback_set(vp, draw_cb, app);
    view_port_input_callback_set(vp, input_cb, q);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, vp, GuiLayerFullscreen);

    InputEvent e;
    bool running = true;
    while(running) {
        if(furi_message_queue_get(q, &e, FuriWaitForever) == FuriStatusOk) {
            if(e.type != InputTypeShort && e.type != InputTypeRepeat) continue;

            furi_mutex_acquire(app->mutex, FuriWaitForever);
            AppState s = app->state;
            furi_mutex_release(app->mutex);

            if(s == StateMenu) {
                switch(e.key) {
                case InputKeyUp:
                    if(app->selected > 0) app->selected--;
                    break;
                case InputKeyDown:
                    if(app->selected < ActionCount - 1) app->selected++;
                    break;
                case InputKeyOk:
                    if(app->selected == ActionExit) {
                        running = false;
                    } else {
                        set_state(app, StateConfirm);
                    }
                    break;
                case InputKeyBack:
                    running = false;
                    break;
                default:
                    break;
                }
            } else if(s == StateConfirm) {
                if(e.key == InputKeyOk) {
                    start_worker(app);
                } else if(e.key == InputKeyBack) {
                    set_state(app, StateMenu);
                }
            } else if(s == StateRunning) {
                if(e.key == InputKeyBack) {
                    app->cancel = true;
                    stop_worker(app);
                    set_state(app, StateMenu);
                }
            } else if(s == StateDone || s == StateError) {
                stop_worker(app);
                set_state(app, StateMenu);
            }

            view_port_update(vp);
        }
    }

    stop_worker(app);

    view_port_enabled_set(vp, false);
    gui_remove_view_port(gui, vp);
    view_port_free(vp);
    furi_message_queue_free(q);
    furi_mutex_free(app->mutex);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
