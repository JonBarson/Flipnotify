/*
 * WiFi Toolkit - Flipper Zero FAP.
 * Talks to the official WiFi Devboard (ESP32) over UART using a simple text protocol.
 * v0.1a: devboard detection, network scan, network detail, current connection.
 * Passive scope only - no offensive functionality.
 */
#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>
#include <furi_hal_serial.h>
#include <furi_hal_serial_control.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define UART_BAUD 115200u
#define MAX_NETS 32
#define SSID_LEN 33
#define LINE_MAX 160
#define RX_STREAM_SIZE 512
#define MENU_COUNT 5

typedef enum {
    StateDetect,
    StateMenu,
    StateScanning,
    StateScanList,
    StateDetail,
    StateCurrent,
    StateComingSoon,
} AppState;

typedef struct {
    char ssid[SSID_LEN];
    char bssid[18];
    int rssi;
    int channel;
    char sec[8];
} WifiNet;

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* input_queue;
    FuriMutex* mutex;
    FuriHalSerialHandle* serial;
    FuriStreamBuffer* rx_stream;
    FuriThread* worker;
    volatile bool running;

    AppState state;
    bool devboard_ok;

    WifiNet nets[MAX_NETS];
    int net_count;
    bool scanning;

    int menu_index;
    int list_index;

    bool cur_connected;
    char cur_state[16];
    char cur_ssid[SSID_LEN];
    char cur_ip[16];
    char cur_gw[16];
    char cur_dns[16];
    int cur_rssi;
    int cur_channel;
} App;

static const char* MENU_ITEMS[MENU_COUNT] = {
    "Scan Networks", "Saved Networks", "Current Connection", "Analyzer", "Settings"};

// ---------------- UART ----------------
static void uart_send_line(App* app, const char* s) {
    furi_hal_serial_tx(app->serial, (const uint8_t*)s, strlen(s));
    furi_hal_serial_tx(app->serial, (const uint8_t*)"\n", 1);
}

static void uart_rx_cb(FuriHalSerialHandle* handle, FuriHalSerialRxEvent event, void* context) {
    App* app = context;
    if(event & FuriHalSerialRxEventData) {
        while(furi_hal_serial_async_rx_available(handle)) {
            uint8_t b = furi_hal_serial_async_rx(handle);
            furi_stream_buffer_send(app->rx_stream, &b, 1, 0);
        }
    }
}

static void copy_str(char* dst, const char* src, size_t cap) {
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = 0;
}

static int split_pipe(char* s, char** out, int max) {
    int n = 0;
    out[n++] = s;
    while(*s && n < max) {
        if(*s == '|') { *s = 0; out[n++] = s + 1; }
        s++;
    }
    return n;
}

static void parse_line(App* app, char* line) {
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    if(strncmp(line, "PONG", 4) == 0) {
        app->devboard_ok = true;
        if(app->state == StateDetect) { app->state = StateMenu; app->menu_index = 0; }
    } else if(strncmp(line, "SCAN_BEGIN", 10) == 0) {
        app->net_count = 0;
        app->scanning = true;
    } else if(strncmp(line, "NET|", 4) == 0) {
        if(app->net_count < MAX_NETS) {
            char* f[6];
            int nf = split_pipe(line, f, 6);
            if(nf >= 6) {
                WifiNet* n = &app->nets[app->net_count];
                copy_str(n->ssid, f[1], SSID_LEN);
                copy_str(n->bssid, f[2], sizeof(n->bssid));
                n->rssi = atoi(f[3]);
                n->channel = atoi(f[4]);
                copy_str(n->sec, f[5], sizeof(n->sec));
                app->net_count++;
            }
        }
    } else if(strncmp(line, "SCAN_END", 8) == 0) {
        app->scanning = false;
        if(app->state == StateScanning) { app->state = StateScanList; app->list_index = 0; }
    } else if(strncmp(line, "STATUS|", 7) == 0) {
        char* f[8];
        int nf = split_pipe(line, f, 8);
        if(nf >= 2) {
            if(strcmp(f[1], "CONNECTED") == 0 && nf >= 6) {
                app->cur_connected = true;
                copy_str(app->cur_state, "CONNECTED", sizeof(app->cur_state));
                copy_str(app->cur_ssid, f[2], SSID_LEN);
                copy_str(app->cur_ip, f[3], sizeof(app->cur_ip));
                app->cur_rssi = atoi(f[4]);
                app->cur_channel = atoi(f[5]);
                if(nf >= 7) copy_str(app->cur_gw, f[6], sizeof(app->cur_gw));
                if(nf >= 8) copy_str(app->cur_dns, f[7], sizeof(app->cur_dns));
            } else if(strcmp(f[1], "DISCONNECTED") == 0) {
                app->cur_connected = false;
                copy_str(app->cur_state, "DISCONNECTED", sizeof(app->cur_state));
            } else if(strcmp(f[1], "CONNECTING") == 0) {
                copy_str(app->cur_state, "CONNECTING", sizeof(app->cur_state));
            } else if(strcmp(f[1], "ERROR") == 0) {
                copy_str(app->cur_state, "ERROR", sizeof(app->cur_state));
            }
        }
    }
    furi_mutex_release(app->mutex);
    view_port_update(app->view_port);
}

static int32_t worker_thread(void* ctx) {
    App* app = ctx;
    char line[LINE_MAX];
    size_t len = 0;
    uint8_t b;
    while(app->running) {
        size_t got = furi_stream_buffer_receive(app->rx_stream, &b, 1, 100);
        if(got == 0) continue;
        if(b == '\r') continue;
        if(b == '\n') {
            line[len] = 0;
            if(len > 0) parse_line(app, line);
            len = 0;
        } else {
            if(len < LINE_MAX - 1) line[len++] = b;
            else len = 0;
        }
    }
    return 0;
}

// ---------------- drawing ----------------
static void title(Canvas* c, const char* t) {
    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 2, 10, t);
    canvas_draw_line(c, 0, 13, 127, 13);
    canvas_set_font(c, FontSecondary);
}

static void draw_menu(Canvas* c, App* app) {
    title(c, "WiFi Toolkit");
    int rows = 4;
    int start = 0;
    if(app->menu_index >= rows) start = app->menu_index - rows + 1;
    for(int i = 0; i < rows && start + i < MENU_COUNT; i++) {
        int idx = start + i;
        int y = 24 + i * 11;
        if(idx == app->menu_index) {
            canvas_draw_box(c, 0, y - 9, 128, 11);
            canvas_set_color(c, ColorWhite);
        }
        canvas_draw_str(c, 4, y, MENU_ITEMS[idx]);
        if(idx == app->menu_index) canvas_set_color(c, ColorBlack);
    }
}

static void draw_scan_list(Canvas* c, App* app) {
    char hdr[24];
    snprintf(hdr, sizeof(hdr), "Networks (%d)", app->net_count);
    title(c, hdr);
    if(app->net_count == 0) { canvas_draw_str(c, 2, 32, "No networks found"); return; }
    int rows = 4;
    int start = 0;
    if(app->list_index >= rows) start = app->list_index - rows + 1;
    for(int i = 0; i < rows && start + i < app->net_count; i++) {
        int idx = start + i;
        int y = 24 + i * 11;
        WifiNet* n = &app->nets[idx];
        if(idx == app->list_index) {
            canvas_draw_box(c, 0, y - 9, 128, 11);
            canvas_set_color(c, ColorWhite);
        }
        char ssid[18];
        copy_str(ssid, n->ssid, sizeof(ssid));
        canvas_draw_str(c, 2, y, ssid);
        char r[8];
        snprintf(r, sizeof(r), "%d", n->rssi);
        canvas_draw_str_aligned(c, 126, y, AlignRight, AlignBottom, r);
        if(idx == app->list_index) canvas_set_color(c, ColorBlack);
    }
}

static void draw_detail(Canvas* c, App* app) {
    title(c, "Network Detail");
    if(app->net_count == 0) return;
    WifiNet* n = &app->nets[app->list_index];
    char l[48];
    canvas_draw_str(c, 2, 24, n->ssid);
    snprintf(l, sizeof(l), "BSSID %s", n->bssid);
    canvas_draw_str(c, 2, 34, l);
    snprintf(l, sizeof(l), "RSSI %d dBm  Ch %d", n->rssi, n->channel);
    canvas_draw_str(c, 2, 44, l);
    snprintf(l, sizeof(l), "Sec %s  2.4 GHz", n->sec);
    canvas_draw_str(c, 2, 54, l);
    canvas_draw_str(c, 2, 63, "OK: Connect   Back");
}

static void draw_current(Canvas* c, App* app) {
    title(c, "Current Connection");
    char l[48];
    if(app->cur_connected) {
        canvas_draw_str(c, 2, 24, app->cur_ssid);
        snprintf(l, sizeof(l), "IP %s", app->cur_ip);
        canvas_draw_str(c, 2, 34, l);
        snprintf(l, sizeof(l), "RSSI %d  Ch %d", app->cur_rssi, app->cur_channel);
        canvas_draw_str(c, 2, 44, l);
        snprintf(l, sizeof(l), "GW %s", app->cur_gw);
        canvas_draw_str(c, 2, 54, l);
    } else {
        const char* st = app->cur_state[0] ? app->cur_state : "Not connected";
        canvas_draw_str(c, 2, 30, st);
    }
    canvas_draw_str(c, 2, 63, "OK: refresh   Back");
}

static void draw_cb(Canvas* c, void* ctx) {
    App* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    canvas_clear(c);
    canvas_set_color(c, ColorBlack);
    switch(app->state) {
    case StateDetect:
        title(c, "WiFi Toolkit");
        canvas_draw_str(c, 2, 28, "Detecting devboard...");
        canvas_draw_str(c, 2, 40, "Attach WiFi Devboard.");
        canvas_draw_str(c, 2, 63, "OK: retry   Back: exit");
        break;
    case StateMenu:
        draw_menu(c, app);
        break;
    case StateScanning: {
        title(c, "Scanning...");
        char l[24];
        snprintf(l, sizeof(l), "%d found", app->net_count);
        canvas_draw_str(c, 2, 32, l);
        canvas_draw_str(c, 2, 63, "Back: cancel");
        break;
    }
    case StateScanList:
        draw_scan_list(c, app);
        break;
    case StateDetail:
        draw_detail(c, app);
        break;
    case StateCurrent:
        draw_current(c, app);
        break;
    case StateComingSoon:
        title(c, "Coming soon");
        canvas_draw_str(c, 2, 32, "Available in next build.");
        canvas_draw_str(c, 2, 63, "Back");
        break;
    }
    furi_mutex_release(app->mutex);
}

// ---------------- input ----------------
static void input_cb(InputEvent* e, void* ctx) {
    App* app = ctx;
    furi_message_queue_put(app->input_queue, e, 0);
}

static void menu_select(App* app) {
    switch(app->menu_index) {
    case 0:
        app->net_count = 0;
        app->state = StateScanning;
        uart_send_line(app, "SCAN");
        break;
    case 2:
        app->cur_state[0] = 0;
        app->cur_connected = false;
        app->state = StateCurrent;
        uart_send_line(app, "STATUS");
        break;
    default:
        app->state = StateComingSoon;
        break;
    }
}

static void handle_input(App* app, InputKey key) {
    switch(app->state) {
    case StateDetect:
        if(key == InputKeyOk) uart_send_line(app, "PING");
        else if(key == InputKeyBack) app->running = false;
        break;
    case StateMenu:
        if(key == InputKeyUp) app->menu_index = (app->menu_index + MENU_COUNT - 1) % MENU_COUNT;
        else if(key == InputKeyDown) app->menu_index = (app->menu_index + 1) % MENU_COUNT;
        else if(key == InputKeyOk) menu_select(app);
        else if(key == InputKeyBack) app->running = false;
        break;
    case StateScanning:
        if(key == InputKeyBack) app->state = StateMenu;
        break;
    case StateScanList:
        if(key == InputKeyUp) { if(app->list_index > 0) app->list_index--; }
        else if(key == InputKeyDown) { if(app->list_index < app->net_count - 1) app->list_index++; }
        else if(key == InputKeyOk) { if(app->net_count > 0) app->state = StateDetail; }
        else if(key == InputKeyRight) { app->net_count = 0; app->state = StateScanning; uart_send_line(app, "SCAN"); }
        else if(key == InputKeyBack) app->state = StateMenu;
        break;
    case StateDetail:
        if(key == InputKeyOk) app->state = StateComingSoon;
        else if(key == InputKeyBack) app->state = StateScanList;
        break;
    case StateCurrent:
        if(key == InputKeyOk) uart_send_line(app, "STATUS");
        else if(key == InputKeyBack) app->state = StateMenu;
        break;
    case StateComingSoon:
        if(key == InputKeyBack) app->state = StateMenu;
        break;
    }
}

// ---------------- app ----------------
int32_t wifi_toolkit_app(void* p) {
    UNUSED(p);
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->input_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->rx_stream = furi_stream_buffer_alloc(RX_STREAM_SIZE, 1);
    app->running = true;
    app->state = StateDetect;

    app->serial = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
    furi_check(app->serial);
    furi_hal_serial_init(app->serial, UART_BAUD);
    furi_hal_serial_async_rx_start(app->serial, uart_rx_cb, app, false);

    app->worker = furi_thread_alloc_ex("WifiTkWorker", 2048, worker_thread, app);
    furi_thread_start(app->worker);

    app->gui = furi_record_open(RECORD_GUI);
    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, draw_cb, app);
    view_port_input_callback_set(app->view_port, input_cb, app);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    uart_send_line(app, "PING");

    InputEvent event;
    while(app->running) {
        if(furi_message_queue_get(app->input_queue, &event, 100) == FuriStatusOk) {
            if(event.type == InputTypeShort || event.type == InputTypeRepeat) {
                furi_mutex_acquire(app->mutex, FuriWaitForever);
                handle_input(app, event.key);
                furi_mutex_release(app->mutex);
                view_port_update(app->view_port);
            }
        }
    }

    app->running = false;
    furi_thread_join(app->worker);
    furi_thread_free(app->worker);

    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_GUI);

    furi_hal_serial_async_rx_stop(app->serial);
    furi_hal_serial_deinit(app->serial);
    furi_hal_serial_control_release(app->serial);

    furi_stream_buffer_free(app->rx_stream);
    furi_message_queue_free(app->input_queue);
    furi_mutex_free(app->mutex);
    free(app);
    return 0;
}
