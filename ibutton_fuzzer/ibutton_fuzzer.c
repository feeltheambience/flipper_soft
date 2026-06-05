// iButton Sequential Fuzzer — authorized pentest tool.
//
// Loads a known-valid .ibtn key as the BASE, then emulates a small
// neighboring range of IDs (base+offset) to demonstrate the
// "sequential ID issuance" weakness of access-control systems.
// Every emulated UID is written to an audit log on SD.
//
// This is NOT a from-scratch brute force (the iButton keyspace is 2^48,
// infeasible to enumerate). It is a bounded neighbor-fuzz around a key
// you already legitimately possess.

#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <storage/storage.h>
#include <ibutton/ibutton_protocols.h>
#include <ibutton/ibutton_key.h>
#include <ibutton/ibutton_worker.h>

#define KEYS_DIR "/ext/ibutton"
#define LOG_DIR "/ext/apps_data/ibutton_fuzzer"
#define LOG_PATH LOG_DIR "/fuzz_log.txt"
#define MAX_FILES 24
#define MAX_NAME_LEN 40

// Range presets (how many neighbors to try, both directions handled in run)
static const int RANGE_OPTS[] = {10, 50, 100, 500, 1000};
#define NUM_RANGE (sizeof(RANGE_OPTS) / sizeof(RANGE_OPTS[0]))
// Hold time per ID (ms) — reader needs time to sample
static const int HOLD_OPTS[] = {150, 250, 400, 600, 1000};
#define NUM_HOLD (sizeof(HOLD_OPTS) / sizeof(HOLD_OPTS[0]))

typedef enum {
    ScreenList,
    ScreenConfig,
    ScreenRunning,
    ScreenDone,
} Screen;

typedef enum {
    FieldRange,
    FieldHold,
    FieldDir,
    FieldCount,
} ConfigField;

typedef struct {
    FuriMutex* mutex;
    Storage* storage;
    iButtonProtocols* protocols;
    iButtonWorker* worker;
    iButtonKey* key;

    char files[MAX_FILES][MAX_NAME_LEN];
    int file_count;
    int selected;
    int scroll;

    Screen screen;
    ConfigField field;
    int range_idx;
    int hold_idx;
    int dir; // 0 = up only (+), 1 = both (±)

    char base_name[MAX_NAME_LEN];
    uint8_t base_data[16];
    size_t data_size;
    int serial_lo;  // index of first serial byte (skip family)
    int serial_hi;  // index of last serial byte (before CRC)

    int total;
    int done;
    char current_uid[40];

    FuriThread* run_thread;
    volatile bool cancel;
} App;

// ---------- scan saved keys ----------

static void scan_keys(App* app) {
    app->file_count = 0;
    storage_simply_mkdir(app->storage, KEYS_DIR);
    File* dir = storage_file_alloc(app->storage);
    if(storage_dir_open(dir, KEYS_DIR)) {
        FileInfo info;
        char name[128];
        while(storage_dir_read(dir, &info, name, sizeof(name)) &&
              app->file_count < MAX_FILES) {
            if(!(info.flags & FSF_DIRECTORY)) {
                size_t len = strlen(name);
                if(len > 5 && strcmp(name + len - 5, ".ibtn") == 0) {
                    strncpy(app->files[app->file_count], name, MAX_NAME_LEN - 1);
                    app->files[app->file_count][MAX_NAME_LEN - 1] = '\0';
                    size_t l = strlen(app->files[app->file_count]);
                    if(l > 5) app->files[app->file_count][l - 5] = '\0';
                    app->file_count++;
                }
            }
        }
        storage_dir_close(dir);
    }
    storage_file_free(dir);
}

// ---------- load base key ----------

static bool load_base(App* app, const char* basename) {
    char path[160];
    snprintf(path, sizeof(path), "%s/%s.ibtn", KEYS_DIR, basename);
    if(!ibutton_protocols_load(app->protocols, app->key, path)) return false;

    iButtonEditableData ed;
    ibutton_protocols_get_editable_data(app->protocols, app->key, &ed);
    if(ed.size == 0 || ed.size > sizeof(app->base_data)) return false;

    memcpy(app->base_data, ed.ptr, ed.size);
    app->data_size = ed.size;
    // DS1990 layout: [family][serial...][crc]. Fuzz serial bytes only.
    // Generic: skip first byte (family) and last byte (crc).
    app->serial_lo = (ed.size >= 3) ? 1 : 0;
    app->serial_hi = (ed.size >= 3) ? (int)ed.size - 2 : (int)ed.size - 1;
    strncpy(app->base_name, basename, MAX_NAME_LEN - 1);
    app->base_name[MAX_NAME_LEN - 1] = '\0';
    return true;
}

// read/write the serial portion as a 64-bit integer (big-endian over bytes)
static uint64_t serial_get(App* app) {
    uint64_t v = 0;
    for(int i = app->serial_lo; i <= app->serial_hi; i++) {
        v = (v << 8) | app->base_data[i];
    }
    return v;
}

static void serial_set(App* app, uint8_t* out, uint64_t v) {
    for(int i = app->serial_hi; i >= app->serial_lo; i--) {
        out[i] = v & 0xFF;
        v >>= 8;
    }
}

static void uid_to_str(const uint8_t* data, size_t size, char* buf, size_t buflen) {
    int pos = 0;
    for(size_t i = 0; i < size && pos < (int)buflen - 3; i++) {
        pos += snprintf(buf + pos, buflen - pos, "%02X", data[i]);
    }
}

// ---------- audit log ----------

static void log_open_header(App* app) {
    storage_simply_mkdir(app->storage, "/ext/apps_data");
    storage_simply_mkdir(app->storage, LOG_DIR);
    File* f = storage_file_alloc(app->storage);
    if(storage_file_open(f, LOG_PATH, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        char hdr[128];
        int n = snprintf(hdr, sizeof(hdr),
                         "\n=== Session base=%s range=%d hold=%dms ===\n",
                         app->base_name, RANGE_OPTS[app->range_idx],
                         HOLD_OPTS[app->hold_idx]);
        storage_file_write(f, hdr, n);
        storage_file_close(f);
    }
    storage_file_free(f);
}

static void log_uid(App* app, const char* uid, int idx) {
    File* f = storage_file_alloc(app->storage);
    if(storage_file_open(f, LOG_PATH, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        char line[80];
        int n = snprintf(line, sizeof(line), "%5d  %s  t=%lu\n",
                         idx, uid, (unsigned long)furi_get_tick());
        storage_file_write(f, line, n);
        storage_file_close(f);
    }
    storage_file_free(f);
}

// ---------- fuzz thread ----------

static int32_t run_thread(void* ctx) {
    App* app = ctx;
    uint64_t base_serial = serial_get(app);
    int range = RANGE_OPTS[app->range_idx];
    int hold = HOLD_OPTS[app->hold_idx];
    bool both = (app->dir == 1);

    int total = both ? (range * 2 + 1) : (range + 1);
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->total = total;
    app->done = 0;
    furi_mutex_release(app->mutex);

    log_open_header(app);

    uint8_t work[16];
    int idx = 0;
    int lo = both ? -range : 0;
    int hi = range;

    for(int off = lo; off <= hi && !app->cancel; off++) {
        memcpy(work, app->base_data, app->data_size);
        serial_set(app, work, base_serial + off);

        // write into the key's editable buffer, recompute CRC
        iButtonEditableData ed;
        ibutton_protocols_get_editable_data(app->protocols, app->key, &ed);
        if(ed.size == app->data_size) {
            memcpy(ed.ptr, work, app->data_size);
            ibutton_protocols_apply_edits(app->protocols, app->key);
        }

        // read back the final bytes (post-CRC) for logging
        char uid[40];
        ibutton_protocols_get_editable_data(app->protocols, app->key, &ed);
        uid_to_str(ed.ptr, ed.size, uid, sizeof(uid));

        furi_mutex_acquire(app->mutex, FuriWaitForever);
        strncpy(app->current_uid, uid, sizeof(app->current_uid) - 1);
        app->current_uid[sizeof(app->current_uid) - 1] = '\0';
        app->done = idx + 1;
        furi_mutex_release(app->mutex);

        ibutton_worker_emulate_start(app->worker, app->key);
        for(int e = 0; e < hold && !app->cancel; e += 50) {
            furi_delay_ms(50);
        }
        ibutton_worker_stop(app->worker);

        log_uid(app, uid, idx);
        idx++;
    }

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->screen = ScreenDone;
    furi_mutex_release(app->mutex);
    return 0;
}

static void start_run(App* app) {
    app->cancel = false;
    app->screen = ScreenRunning;
    app->run_thread = furi_thread_alloc();
    furi_thread_set_name(app->run_thread, "FuzzRun");
    furi_thread_set_stack_size(app->run_thread, 2048);
    furi_thread_set_context(app->run_thread, app);
    furi_thread_set_callback(app->run_thread, run_thread);
    furi_thread_start(app->run_thread);
}

static void stop_run(App* app) {
    if(app->run_thread) {
        app->cancel = true;
        furi_thread_join(app->run_thread);
        furi_thread_free(app->run_thread);
        app->run_thread = NULL;
        ibutton_worker_stop(app->worker);
    }
}

// ---------- UI ----------

static void draw_cb(Canvas* canvas, void* ctx) {
    App* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "iButton Fuzzer");
    canvas_draw_line(canvas, 0, 13, 128, 13);
    canvas_set_font(canvas, FontSecondary);
    char buf[48];

    if(app->screen == ScreenList) {
        if(app->file_count == 0) {
            canvas_draw_str_aligned(canvas, 64, 28, AlignCenter, AlignCenter,
                                    "No saved keys.");
            canvas_draw_str_aligned(canvas, 64, 40, AlignCenter, AlignCenter,
                                    "Read a valid key first:");
            canvas_draw_str_aligned(canvas, 64, 50, AlignCenter, AlignCenter,
                                    "iButton > Read > Save");
            canvas_draw_str_aligned(canvas, 64, 62, AlignCenter, AlignCenter,
                                    "OK = rescan");
        } else {
            canvas_draw_str(canvas, 2, 22, "Pick base key:");
            const int visible = 4;
            for(int i = 0; i < visible && (i + app->scroll) < app->file_count; i++) {
                int idx = i + app->scroll;
                int y = 33 + i * 9;
                if(idx == app->selected) {
                    canvas_draw_box(canvas, 0, y - 7, 128, 9);
                    canvas_set_color(canvas, ColorWhite);
                }
                canvas_draw_str(canvas, 4, y, app->files[idx]);
                if(idx == app->selected) canvas_set_color(canvas, ColorBlack);
            }
        }
    } else if(app->screen == ScreenConfig) {
        snprintf(buf, sizeof(buf), "Base: %s", app->base_name);
        canvas_draw_str(canvas, 2, 23, buf);

        snprintf(buf, sizeof(buf), "%sRange: +/-%d",
                 app->field == FieldRange ? ">" : " ", RANGE_OPTS[app->range_idx]);
        canvas_draw_str(canvas, 2, 34, buf);
        snprintf(buf, sizeof(buf), "%sHold: %dms",
                 app->field == FieldHold ? ">" : " ", HOLD_OPTS[app->hold_idx]);
        canvas_draw_str(canvas, 2, 44, buf);
        snprintf(buf, sizeof(buf), "%sDir: %s",
                 app->field == FieldDir ? ">" : " ",
                 app->dir == 1 ? "both +/-" : "up only +");
        canvas_draw_str(canvas, 2, 54, buf);
        canvas_draw_str(canvas, 2, 63, "OK=run  U/D=val  L/R=field");
    } else if(app->screen == ScreenRunning) {
        snprintf(buf, sizeof(buf), "Emulating %d/%d", app->done, app->total);
        canvas_draw_str_aligned(canvas, 64, 26, AlignCenter, AlignCenter, buf);
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 40, AlignCenter, AlignCenter,
                                app->current_uid);
        canvas_set_font(canvas, FontSecondary);
        // progress bar
        int w = app->total > 0 ? (app->done * 124 / app->total) : 0;
        canvas_draw_frame(canvas, 2, 46, 124, 6);
        canvas_draw_box(canvas, 3, 47, w, 4);
        canvas_draw_str_aligned(canvas, 64, 62, AlignCenter, AlignCenter,
                                "Back = stop");
    } else { // Done
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 26, AlignCenter, AlignCenter, "Done");
        canvas_set_font(canvas, FontSecondary);
        snprintf(buf, sizeof(buf), "%d IDs emulated", app->done);
        canvas_draw_str_aligned(canvas, 64, 40, AlignCenter, AlignCenter, buf);
        canvas_draw_str_aligned(canvas, 64, 52, AlignCenter, AlignCenter,
                                "Log: ibutton_fuzzer/");
        canvas_draw_str_aligned(canvas, 64, 62, AlignCenter, AlignCenter,
                                "Any key = back");
    }

    furi_mutex_release(app->mutex);
}

static void input_cb(InputEvent* e, void* ctx) {
    FuriMessageQueue* q = ctx;
    furi_message_queue_put(q, e, FuriWaitForever);
}

int32_t ibutton_fuzzer_app(void* p) {
    UNUSED(p);
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(*app));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->storage = furi_record_open(RECORD_STORAGE);
    app->protocols = ibutton_protocols_alloc();
    app->key = ibutton_key_alloc(ibutton_protocols_get_max_data_size(app->protocols));
    app->worker = ibutton_worker_alloc(app->protocols);
    ibutton_worker_start_thread(app->worker);
    app->screen = ScreenList;
    app->range_idx = 1;
    app->hold_idx = 1;
    app->dir = 0;
    scan_keys(app);

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

            if(scr == ScreenList) {
                furi_mutex_acquire(app->mutex, FuriWaitForever);
                switch(e.key) {
                case InputKeyUp:
                    if(app->selected > 0) app->selected--;
                    if(app->selected < app->scroll) app->scroll = app->selected;
                    break;
                case InputKeyDown:
                    if(app->selected < app->file_count - 1) app->selected++;
                    if(app->selected >= app->scroll + 4) app->scroll = app->selected - 3;
                    break;
                case InputKeyOk:
                    if(app->file_count == 0) {
                        furi_mutex_release(app->mutex);
                        scan_keys(app);
                        furi_mutex_acquire(app->mutex, FuriWaitForever);
                    } else {
                        char sel[MAX_NAME_LEN];
                        strncpy(sel, app->files[app->selected], MAX_NAME_LEN);
                        furi_mutex_release(app->mutex);
                        bool ok = load_base(app, sel);
                        furi_mutex_acquire(app->mutex, FuriWaitForever);
                        if(ok) app->screen = ScreenConfig;
                    }
                    break;
                case InputKeyBack:
                    running = false;
                    break;
                default:
                    break;
                }
                furi_mutex_release(app->mutex);
            } else if(scr == ScreenConfig) {
                furi_mutex_acquire(app->mutex, FuriWaitForever);
                switch(e.key) {
                case InputKeyLeft:
                    if(app->field > 0) app->field--;
                    break;
                case InputKeyRight:
                    if(app->field < FieldCount - 1) app->field++;
                    break;
                case InputKeyUp:
                    if(app->field == FieldRange && app->range_idx < (int)NUM_RANGE - 1)
                        app->range_idx++;
                    else if(app->field == FieldHold && app->hold_idx < (int)NUM_HOLD - 1)
                        app->hold_idx++;
                    else if(app->field == FieldDir)
                        app->dir = 1;
                    break;
                case InputKeyDown:
                    if(app->field == FieldRange && app->range_idx > 0)
                        app->range_idx--;
                    else if(app->field == FieldHold && app->hold_idx > 0)
                        app->hold_idx--;
                    else if(app->field == FieldDir)
                        app->dir = 0;
                    break;
                case InputKeyOk:
                    furi_mutex_release(app->mutex);
                    start_run(app);
                    furi_mutex_acquire(app->mutex, FuriWaitForever);
                    break;
                case InputKeyBack:
                    app->screen = ScreenList;
                    break;
                default:
                    break;
                }
                furi_mutex_release(app->mutex);
            } else if(scr == ScreenRunning) {
                if(e.key == InputKeyBack) {
                    stop_run(app);
                    furi_mutex_acquire(app->mutex, FuriWaitForever);
                    app->screen = ScreenConfig;
                    furi_mutex_release(app->mutex);
                }
            } else { // Done
                stop_run(app);
                furi_mutex_acquire(app->mutex, FuriWaitForever);
                app->screen = ScreenList;
                furi_mutex_release(app->mutex);
            }
        }

        uint32_t now = furi_get_tick();
        if(now - last_redraw > 150) {
            view_port_update(vp);
            last_redraw = now;
        }
    }

    stop_run(app);
    ibutton_worker_stop_thread(app->worker);
    ibutton_worker_free(app->worker);
    ibutton_key_free(app->key);
    ibutton_protocols_free(app->protocols);

    view_port_enabled_set(vp, false);
    gui_remove_view_port(gui, vp);
    view_port_free(vp);
    furi_message_queue_free(q);
    furi_mutex_free(app->mutex);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
