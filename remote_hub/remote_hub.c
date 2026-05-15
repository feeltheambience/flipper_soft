#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <storage/storage.h>
#include <loader/loader.h>

#define REMOTE_HUB_DIR "/ext/subghz/remote_hub"
#define MAX_FILES 32
#define MAX_NAME_LEN 48

typedef struct {
    FuriMutex* mutex;
    char files[MAX_FILES][MAX_NAME_LEN];
    int file_count;
    int selected;
    int scroll;
} RemoteHubApp;

static void sort_files(RemoteHubApp* app) {
    char tmp[MAX_NAME_LEN];
    for(int i = 1; i < app->file_count; i++) {
        strncpy(tmp, app->files[i], MAX_NAME_LEN);
        int j = i - 1;
        while(j >= 0 && strcmp(app->files[j], tmp) > 0) {
            strncpy(app->files[j + 1], app->files[j], MAX_NAME_LEN);
            j--;
        }
        strncpy(app->files[j + 1], tmp, MAX_NAME_LEN);
    }
}

static void scan_files(RemoteHubApp* app) {
    app->file_count = 0;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, REMOTE_HUB_DIR);

    File* dir = storage_file_alloc(storage);
    if(storage_dir_open(dir, REMOTE_HUB_DIR)) {
        FileInfo info;
        char name[256];
        while(storage_dir_read(dir, &info, name, sizeof(name)) &&
              app->file_count < MAX_FILES) {
            if(!(info.flags & FSF_DIRECTORY)) {
                size_t len = strlen(name);
                if(len > 4 && strcmp(name + len - 4, ".sub") == 0) {
                    strncpy(app->files[app->file_count], name, MAX_NAME_LEN - 1);
                    app->files[app->file_count][MAX_NAME_LEN - 1] = '\0';
                    size_t l = strlen(app->files[app->file_count]);
                    if(l > 4) app->files[app->file_count][l - 4] = '\0';
                    app->file_count++;
                }
            }
        }
        storage_dir_close(dir);
    }
    storage_file_free(dir);
    furi_record_close(RECORD_STORAGE);

    if(app->file_count > 1) sort_files(app);
}

static void draw_cb(Canvas* canvas, void* ctx) {
    RemoteHubApp* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Remote Hub");
    canvas_draw_line(canvas, 0, 13, 128, 13);

    canvas_set_font(canvas, FontSecondary);

    if(app->file_count == 0) {
        canvas_draw_str_aligned(canvas, 64, 26, AlignCenter, AlignCenter, "No .sub files.");
        canvas_draw_str_aligned(canvas, 64, 38, AlignCenter, AlignCenter, "Put files into:");
        canvas_draw_str_aligned(canvas, 64, 50, AlignCenter, AlignCenter, "/subghz/remote_hub/");
        canvas_draw_str_aligned(canvas, 64, 62, AlignCenter, AlignCenter, "OK = rescan");
    } else {
        const int visible = 5;
        const int line_h = 10;
        if(app->selected < app->scroll) app->scroll = app->selected;
        if(app->selected >= app->scroll + visible) app->scroll = app->selected - visible + 1;

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

    furi_mutex_release(app->mutex);
}

static void input_cb(InputEvent* e, void* ctx) {
    FuriMessageQueue* q = ctx;
    furi_message_queue_put(q, e, FuriWaitForever);
}

int32_t remote_hub_app(void* p) {
    UNUSED(p);
    RemoteHubApp* app = malloc(sizeof(RemoteHubApp));
    memset(app, 0, sizeof(*app));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    scan_files(app);

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
            if(e.type == InputTypeShort || e.type == InputTypeRepeat) {
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
                        scan_files(app);
                        furi_mutex_acquire(app->mutex, FuriWaitForever);
                    } else {
                        char path[256];
                        snprintf(path, sizeof(path), "%s/%s.sub",
                                 REMOTE_HUB_DIR, app->files[app->selected]);
                        furi_mutex_release(app->mutex);
                        Loader* loader = furi_record_open(RECORD_LOADER);
                        loader_start_with_gui_error(loader, "Sub-GHz", path);
                        furi_record_close(RECORD_LOADER);
                        furi_mutex_acquire(app->mutex, FuriWaitForever);
                    }
                    break;
                case InputKeyBack:
                    running = false;
                    break;
                default:
                    break;
                }
                furi_mutex_release(app->mutex);
                view_port_update(vp);
            }
        }
    }

    view_port_enabled_set(vp, false);
    gui_remove_view_port(gui, vp);
    view_port_free(vp);
    furi_message_queue_free(q);
    furi_mutex_free(app->mutex);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
