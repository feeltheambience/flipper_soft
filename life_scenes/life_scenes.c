#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_speaker.h>
#include <gui/gui.h>
#include <input/input.h>
#include <storage/storage.h>
#include <infrared.h>
#include <infrared_transmit.h>

#define SCENES_DIR "/ext/life_scenes"
#define MAX_SCENES 24
#define MAX_NAME_LEN 40
#define MAX_STEPS 48
#define MAX_LINE 96

typedef enum {
    StepIR,
    StepDelay,
    StepVibro,
    StepBeep,
    StepSubghzSkip, // documented but not executed in v1
} StepType;

typedef struct {
    StepType type;
    InfraredProtocol ir_proto;
    uint32_t addr;
    uint32_t cmd;
    uint32_t arg; // delay ms / vibro ms / beep ms
    uint32_t arg2; // beep freq
    char label[24];
} Step;

typedef enum {
    ScreenList,
    ScreenRunning,
    ScreenDone,
} Screen;

typedef struct {
    FuriMutex* mutex;
    char files[MAX_SCENES][MAX_NAME_LEN];
    int file_count;
    int selected;
    int scroll;

    Screen screen;
    char scene_name[MAX_NAME_LEN];
    Step steps[MAX_STEPS];
    int step_count;
    int current_step;
    char current_label[32];

    FuriThread* worker;
    volatile bool cancel;
    bool speaker_acquired;
} App;

// ---------- file scan ----------

static void scan_scenes(App* app) {
    app->file_count = 0;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, SCENES_DIR);

    File* dir = storage_file_alloc(storage);
    if(storage_dir_open(dir, SCENES_DIR)) {
        FileInfo info;
        char name[128];
        while(storage_dir_read(dir, &info, name, sizeof(name)) &&
              app->file_count < MAX_SCENES) {
            if(!(info.flags & FSF_DIRECTORY)) {
                size_t len = strlen(name);
                if(len > 6 && strcmp(name + len - 6, ".scene") == 0) {
                    strncpy(app->files[app->file_count], name, MAX_NAME_LEN - 1);
                    app->files[app->file_count][MAX_NAME_LEN - 1] = '\0';
                    size_t l = strlen(app->files[app->file_count]);
                    if(l > 6) app->files[app->file_count][l - 6] = '\0'; // strip .scene
                    app->file_count++;
                }
            }
        }
        storage_dir_close(dir);
    }
    storage_file_free(dir);
    furi_record_close(RECORD_STORAGE);
}

// ---------- scene parsing ----------

static char* trim(char* s) {
    while(*s == ' ' || *s == '\t') s++;
    char* end = s + strlen(s) - 1;
    while(end > s && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
        *end-- = '\0';
    }
    return s;
}

static void parse_step_line(App* app, char* line) {
    line = trim(line);
    if(line[0] == '\0' || line[0] == '#') return;
    if(app->step_count >= MAX_STEPS) return;

    // tokenize by comma (in place)
    char* tok[6] = {0};
    int n = 0;
    char* p = line;
    tok[n++] = p;
    while(*p && n < 6) {
        if(*p == ',') {
            *p = '\0';
            tok[n++] = p + 1;
        }
        p++;
    }
    for(int i = 0; i < n; i++) tok[i] = trim(tok[i]);

    const char* action = tok[0];

    if(strcmp(action, "NAME") == 0 && n >= 2) {
        strncpy(app->scene_name, tok[1], MAX_NAME_LEN - 1);
        app->scene_name[MAX_NAME_LEN - 1] = '\0';
        return;
    }

    Step* st = &app->steps[app->step_count];
    memset(st, 0, sizeof(*st));

    if(strcmp(action, "IR") == 0 && n >= 4) {
        InfraredProtocol proto = infrared_get_protocol_by_name(tok[1]);
        if(proto == InfraredProtocolUnknown) return;
        st->type = StepIR;
        st->ir_proto = proto;
        st->addr = (uint32_t)strtol(tok[2], NULL, 0);
        st->cmd = (uint32_t)strtol(tok[3], NULL, 0);
        snprintf(st->label, sizeof(st->label), "IR %s", tok[1]);
        app->step_count++;
    } else if(strcmp(action, "DELAY") == 0 && n >= 2) {
        st->type = StepDelay;
        st->arg = (uint32_t)strtol(tok[1], NULL, 0);
        snprintf(st->label, sizeof(st->label), "Wait %lums", (unsigned long)st->arg);
        app->step_count++;
    } else if(strcmp(action, "VIBRO") == 0 && n >= 2) {
        st->type = StepVibro;
        st->arg = (uint32_t)strtol(tok[1], NULL, 0);
        snprintf(st->label, sizeof(st->label), "Vibro %lums", (unsigned long)st->arg);
        app->step_count++;
    } else if(strcmp(action, "BEEP") == 0 && n >= 3) {
        st->type = StepBeep;
        st->arg2 = (uint32_t)strtol(tok[1], NULL, 0); // freq
        st->arg = (uint32_t)strtol(tok[2], NULL, 0); // ms
        snprintf(st->label, sizeof(st->label), "Beep %luHz", (unsigned long)st->arg2);
        app->step_count++;
    } else if(strcmp(action, "SUBGHZ") == 0 && n >= 2) {
        st->type = StepSubghzSkip;
        snprintf(st->label, sizeof(st->label), "SubGHz (skip)");
        app->step_count++;
    }
}

static bool load_scene(App* app, const char* basename) {
    app->step_count = 0;
    app->scene_name[0] = '\0';
    strncpy(app->scene_name, basename, MAX_NAME_LEN - 1);

    char path[160];
    snprintf(path, sizeof(path), "%s/%s.scene", SCENES_DIR, basename);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* f = storage_file_alloc(storage);
    bool ok = false;
    if(storage_file_open(f, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        char line[MAX_LINE];
        int pos = 0;
        char ch;
        while(storage_file_read(f, &ch, 1) == 1) {
            if(ch == '\n') {
                line[pos] = '\0';
                parse_step_line(app, line);
                pos = 0;
            } else if(pos < MAX_LINE - 1) {
                line[pos++] = ch;
            }
        }
        if(pos > 0) {
            line[pos] = '\0';
            parse_step_line(app, line);
        }
        storage_file_close(f);
        ok = (app->step_count > 0);
    }
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

// ---------- execution ----------

static void exec_step(App* app, const Step* st) {
    switch(st->type) {
    case StepIR: {
        InfraredMessage msg = {
            .protocol = st->ir_proto,
            .address = st->addr,
            .command = st->cmd,
            .repeat = false,
        };
        infrared_send(&msg, 3);
        break;
    }
    case StepDelay:
        // chunked so cancel is responsive
        for(uint32_t e = 0; e < st->arg && !app->cancel; e += 50) {
            furi_delay_ms(50);
        }
        break;
    case StepVibro:
        furi_hal_vibro_on(true);
        for(uint32_t e = 0; e < st->arg && !app->cancel; e += 50) {
            furi_delay_ms(50);
        }
        furi_hal_vibro_on(false);
        break;
    case StepBeep:
        if(furi_hal_speaker_acquire(500)) {
            furi_hal_speaker_start((float)st->arg2, 1.0f);
            furi_delay_ms(st->arg);
            furi_hal_speaker_stop();
            furi_hal_speaker_release();
        }
        break;
    case StepSubghzSkip:
        // Not executed in v1
        furi_delay_ms(100);
        break;
    }
}

static int32_t worker_thread(void* ctx) {
    App* app = ctx;
    for(int i = 0; i < app->step_count && !app->cancel; i++) {
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->current_step = i;
        strncpy(app->current_label, app->steps[i].label, sizeof(app->current_label) - 1);
        app->current_label[sizeof(app->current_label) - 1] = '\0';
        furi_mutex_release(app->mutex);

        exec_step(app, &app->steps[i]);

        // small gap between steps
        if(!app->cancel) furi_delay_ms(120);
    }

    furi_hal_vibro_on(false);
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->screen = ScreenDone;
    furi_mutex_release(app->mutex);
    return 0;
}

static void start_scene(App* app) {
    app->cancel = false;
    app->current_step = 0;
    app->current_label[0] = '\0';
    app->screen = ScreenRunning;
    app->worker = furi_thread_alloc();
    furi_thread_set_name(app->worker, "SceneWorker");
    furi_thread_set_stack_size(app->worker, 2048);
    furi_thread_set_context(app->worker, app);
    furi_thread_set_callback(app->worker, worker_thread);
    furi_thread_start(app->worker);
}

static void stop_scene(App* app) {
    if(app->worker) {
        app->cancel = true;
        furi_thread_join(app->worker);
        furi_thread_free(app->worker);
        app->worker = NULL;
    }
}

// ---------- UI ----------

static void draw_cb(Canvas* canvas, void* ctx) {
    App* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Life Scenes");
    canvas_draw_line(canvas, 0, 13, 128, 13);
    canvas_set_font(canvas, FontSecondary);

    if(app->screen == ScreenList) {
        if(app->file_count == 0) {
            canvas_draw_str_aligned(canvas, 64, 28, AlignCenter, AlignCenter,
                                    "No scenes found.");
            canvas_draw_str_aligned(canvas, 64, 40, AlignCenter, AlignCenter,
                                    "Put .scene files into:");
            canvas_draw_str_aligned(canvas, 64, 50, AlignCenter, AlignCenter,
                                    "/ext/life_scenes/");
            canvas_draw_str_aligned(canvas, 64, 62, AlignCenter, AlignCenter,
                                    "OK = rescan");
        } else {
            const int visible = 5;
            const int line_h = 10;
            if(app->selected < app->scroll) app->scroll = app->selected;
            if(app->selected >= app->scroll + visible)
                app->scroll = app->selected - visible + 1;
            for(int i = 0; i < visible && (i + app->scroll) < app->file_count; i++) {
                int idx = i + app->scroll;
                int y = 24 + i * line_h;
                if(idx == app->selected) {
                    canvas_draw_box(canvas, 0, y - 8, 128, line_h);
                    canvas_set_color(canvas, ColorWhite);
                }
                canvas_draw_str(canvas, 4, y, app->files[idx]);
                if(idx == app->selected) canvas_set_color(canvas, ColorBlack);
            }
        }
    } else if(app->screen == ScreenRunning) {
        canvas_draw_str_aligned(canvas, 64, 24, AlignCenter, AlignCenter,
                                app->scene_name);
        char buf[40];
        snprintf(buf, sizeof(buf), "Step %d / %d",
                 app->current_step + 1, app->step_count);
        canvas_draw_str_aligned(canvas, 64, 38, AlignCenter, AlignCenter, buf);
        canvas_draw_str_aligned(canvas, 64, 50, AlignCenter, AlignCenter,
                                app->current_label);
        canvas_draw_str_aligned(canvas, 64, 62, AlignCenter, AlignCenter,
                                "Back = stop");
    } else { // ScreenDone
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 28, AlignCenter, AlignCenter, "Scene done");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 44, AlignCenter, AlignCenter,
                                app->scene_name);
        canvas_draw_str_aligned(canvas, 64, 60, AlignCenter, AlignCenter,
                                "Any key = back");
    }

    furi_mutex_release(app->mutex);
}

static void input_cb(InputEvent* e, void* ctx) {
    FuriMessageQueue* q = ctx;
    furi_message_queue_put(q, e, FuriWaitForever);
}

int32_t life_scenes_app(void* p) {
    UNUSED(p);
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(*app));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->screen = ScreenList;
    scan_scenes(app);

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
                    break;
                case InputKeyDown:
                    if(app->selected < app->file_count - 1) app->selected++;
                    break;
                case InputKeyOk:
                    if(app->file_count == 0) {
                        furi_mutex_release(app->mutex);
                        scan_scenes(app);
                        furi_mutex_acquire(app->mutex, FuriWaitForever);
                    } else {
                        char sel[MAX_NAME_LEN];
                        strncpy(sel, app->files[app->selected], MAX_NAME_LEN);
                        furi_mutex_release(app->mutex);
                        bool ok = load_scene(app, sel);
                        furi_mutex_acquire(app->mutex, FuriWaitForever);
                        if(ok) {
                            furi_mutex_release(app->mutex);
                            start_scene(app);
                            furi_mutex_acquire(app->mutex, FuriWaitForever);
                        }
                    }
                    break;
                case InputKeyBack:
                    running = false;
                    break;
                default:
                    break;
                }
                furi_mutex_release(app->mutex);
            } else if(scr == ScreenRunning) {
                if(e.key == InputKeyBack) {
                    stop_scene(app);
                    furi_mutex_acquire(app->mutex, FuriWaitForever);
                    app->screen = ScreenList;
                    furi_mutex_release(app->mutex);
                }
            } else { // ScreenDone
                stop_scene(app);
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

    stop_scene(app);
    furi_hal_vibro_on(false);

    view_port_enabled_set(vp, false);
    gui_remove_view_port(gui, vp);
    view_port_free(vp);
    furi_message_queue_free(q);
    furi_mutex_free(app->mutex);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
