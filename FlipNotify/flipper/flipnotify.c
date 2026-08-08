#include <furi.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <bt/bt_service/bt.h>
#include <ble_glue/profiles/serial_profile.h>
#include <string.h>

#define MAX_TEXT 220

typedef struct {
    FuriMessageQueue* input_queue;
    FuriMutex* lock;
    ViewPort* view_port;
    Gui* gui;
    NotificationApp* notifications;
    Bt* bt;
    FuriHalBleProfileBase* profile;
    char app[32];
    char title[64];
    char text[MAX_TEXT];
    bool connected;
} FlipNotify;

static void draw_wrapped(Canvas* canvas, const char* s, int x, int y, int max_chars, int max_lines) {
    char line[32];
    int line_no = 0;
    while(*s && line_no < max_lines) {
        int n = 0;
        while(*s && n < max_chars) {
            if(*s == '\n') { s++; break; }
            line[n++] = *s++;
        }
        line[n] = 0;
        canvas_draw_str(canvas, x, y + line_no * 11, line);
        line_no++;
    }
}

static void draw_cb(Canvas* canvas, void* ctx) {
    FlipNotify* app = ctx;
    furi_mutex_acquire(app->lock, FuriWaitForever);
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "FlipNotify");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 88, 10, app->connected ? "BLE" : "WAIT");
    canvas_draw_line(canvas, 0, 13, 127, 13);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 25, app->app[0] ? app->app : "Waiting for phone...");
    canvas_set_font(canvas, FontSecondary);
    draw_wrapped(canvas, app->title, 2, 37, 21, 1);
    draw_wrapped(canvas, app->text, 2, 49, 21, 2);
    furi_mutex_release(app->lock);
}

static void input_cb(InputEvent* event, void* ctx) {
    FlipNotify* app = ctx;
    furi_message_queue_put(app->input_queue, event, 0);
}

static void bt_status_cb(BtStatus status, void* ctx) {
    FlipNotify* app = ctx;
    furi_mutex_acquire(app->lock, FuriWaitForever);
    app->connected = (status == BtStatusConnected);
    furi_mutex_release(app->lock);
    view_port_update(app->view_port);
}

static void copy_field(char* dst, size_t cap, const char* start, size_t len) {
    if(len >= cap) len = cap - 1;
    memcpy(dst, start, len);
    dst[len] = 0;
}

static void parse_message(FlipNotify* app, const char* msg, size_t len) {
    // Wire format: app\ttitle\ttext\n
    const char* end = msg + len;
    const char* p1 = memchr(msg, '\t', len);
    if(!p1) return;
    const char* p2 = memchr(p1 + 1, '\t', end - (p1 + 1));
    if(!p2) return;
    const char* text_end = end;
    while(text_end > p2 + 1 && (text_end[-1] == '\n' || text_end[-1] == '\r' || text_end[-1] == 0)) text_end--;

    furi_mutex_acquire(app->lock, FuriWaitForever);
    copy_field(app->app, sizeof(app->app), msg, p1 - msg);
    copy_field(app->title, sizeof(app->title), p1 + 1, p2 - (p1 + 1));
    copy_field(app->text, sizeof(app->text), p2 + 1, text_end - (p2 + 1));
    furi_mutex_release(app->lock);

    notification_message(app->notifications, &sequence_single_vibro);
    view_port_update(app->view_port);
}

static uint16_t serial_cb(SerialServiceEvent event, void* ctx) {
    FlipNotify* app = ctx;
    if(event.event == SerialServiceEventTypeDataReceived && event.data.buffer && event.data.size) {
        parse_message(app, (const char*)event.data.buffer, event.data.size);
        ble_profile_serial_notify_buffer_is_empty(app->profile);
    }
    return 240;
}

int32_t flipnotify_app(void* p) {
    UNUSED(p);
    FlipNotify app = {0};
    app.lock = furi_mutex_alloc(FuriMutexTypeNormal);
    app.input_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app.gui = furi_record_open(RECORD_GUI);
    app.notifications = furi_record_open(RECORD_NOTIFICATION);
    app.bt = furi_record_open(RECORD_BT);

    app.view_port = view_port_alloc();
    view_port_draw_callback_set(app.view_port, draw_cb, &app);
    view_port_input_callback_set(app.view_port, input_cb, &app);
    gui_add_view_port(app.gui, app.view_port, GuiLayerFullscreen);

    bt_set_status_changed_callback(app.bt, bt_status_cb, &app);
    app.profile = bt_profile_start(app.bt, ble_profile_serial, NULL);
    if(app.profile) {
        ble_profile_serial_set_rpc_active(app.profile, false);
        ble_profile_serial_set_event_callback(app.profile, 240, serial_cb, &app);
    }
    view_port_update(app.view_port);

    InputEvent event;
    bool running = true;
    while(running) {
        if(furi_message_queue_get(app.input_queue, &event, 250) == FuriStatusOk) {
            if(event.type == InputTypeShort && event.key == InputKeyBack) running = false;
        }
    }

    if(app.profile) ble_profile_serial_set_event_callback(app.profile, 0, NULL, NULL);
    bt_set_status_changed_callback(app.bt, NULL, NULL);
    bt_profile_restore_default(app.bt);

    gui_remove_view_port(app.gui, app.view_port);
    view_port_free(app.view_port);
    furi_record_close(RECORD_BT);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);
    furi_message_queue_free(app.input_queue);
    furi_mutex_free(app.lock);
    return 0;
}
