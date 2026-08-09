/*
 * WiFi Toolkit - Flipper Zero FAP (v0.1).
 * Talks to the official WiFi Devboard (ESP32) over UART with a simple text protocol.
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
#define MAX_SAVED 8
#define SSID_LEN 33
#define LINE_MAX 160
#define RX_STREAM_SIZE 512
#define MENU_COUNT 5
#define RSSI_HIST 40
#define KB_ROWS 4
#define KB_COLS 10
#define KB_ACTIONS 5

typedef enum {
    StateDetect,
    StateMenu,
    StateScanning,
    StateScanList,
    StateDetail,
    StateKeyboard,
    StateConnecting,
    StateCurrent,
    StateSaved,
    StateSavedAction,
    StateAnalyzerMenu,
    StateSignalMonitor,
    StateChannelView,
    StateSettings,
} AppState;

typedef struct {
    char ssid[SSID_LEN];
    char bssid[18];
    int rssi;
    int channel;
    char sec[8];
} WifiNet;

typedef struct {
    char ssid[SSID_LEN];
    int autoc;
} SavedNet;

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* input_queue;
    FuriMutex* mutex;
    FuriHalSerialHandle* serial;
    FuriStreamBuffer* rx_stream;
    FuriThread* worker;
    FuriTimer* rssi_timer;
    volatile bool running;

    AppState state;

    WifiNet nets[MAX_NETS];
    int net_count;

    SavedNet saved[MAX_SAVED];
    int saved_count;
    int saved_index;
    int saved_action;

    int menu_index;
    int list_index;
    int detail_action;
    int analyzer_index;
    int settings_index;

    // connect target / keyboard
    char target_ssid[SSID_LEN];
    char password[64];
    int pw_len;
    int kb_row;
    int kb_col;
    bool kb_shift;
    bool kb_show;

    // current connection
    bool cur_connected;
    char cur_state[16];
    char cur_ssid[SSID_LEN];
    char cur_ip[16];
    char cur_gw[16];
    char cur_dns[16];
    int cur_rssi;
    int cur_channel;

    // signal monitor
    char mon_ssid[SSID_LEN];
    char mon_bssid[18];
    int mon_channel;
    int rssi_hist[RSSI_HIST];
    int rssi_hist_len;
    bool mon_have;

    // settings / version
    bool auto_reconnect;
    char esp_fw[16];
    char esp_proto[8];
} App;

static const char* MENU_ITEMS[MENU_COUNT] = {
    "Scan Networks", "Saved Networks", "Current Connection", "Analyzer", "Settings"};

static const char* KB_LOWER[KB_ROWS] = {"1234567890", "qwertyuiop", "asdfghjkl;", "zxcvbnm,._"};
static const char* KB_UPPER[KB_ROWS] = {"!@#$%^&*()", "QWERTYUIOP", "ASDFGHJKL:", "ZXCVBNM<>?"};
static const char* KB_ACT[KB_ACTIONS] = {"Aa", "SPC", "DEL", "SHW", "OK"};

// ---------------- helpers ----------------
static void uart_send_line(App* app, const char* s) {
    furi_hal_serial_tx(app->serial, (const uint8_t*)s, strlen(s));
    furi_hal_serial_tx(app->serial, (const uint8_t*)"\n", 1);
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

static int kb_cols(int row) { return row < KB_ROWS ? KB_COLS : KB_ACTIONS; }

static void rssi_push(App* app, int v) {
    if(app->rssi_hist_len < RSSI_HIST) {
        app->rssi_hist[app->rssi_hist_len++] = v;
    } else {
        for(int i = 0; i < RSSI_HIST - 1; i++) app->rssi_hist[i] = app->rssi_hist[i + 1];
        app->rssi_hist[RSSI_HIST - 1] = v;
    }
}

// ---------------- UART RX + parser ----------------
static void uart_rx_cb(FuriHalSerialHandle* handle, FuriHalSerialRxEvent event, void* context) {
    App* app = context;
    if(event & FuriHalSerialRxEventData) {
        while(furi_hal_serial_async_rx_available(handle)) {
            uint8_t b = furi_hal_serial_async_rx(handle);
            furi_stream_buffer_send(app->rx_stream, &b, 1, 0);
        }
    }
}

static void parse_line(App* app, char* line) {
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    if(strncmp(line, "PONG", 4) == 0) {
        char* f[3];
        int nf = split_pipe(line, f, 3);
        if(nf >= 3) { copy_str(app->esp_proto, f[1], sizeof(app->esp_proto)); copy_str(app->esp_fw, f[2], sizeof(app->esp_fw)); }
        if(app->state == StateDetect) { app->state = StateMenu; app->menu_index = 0; }
    } else if(strncmp(line, "VERSION|", 8) == 0) {
        char* f[3];
        int nf = split_pipe(line, f, 3);
        if(nf >= 3) { copy_str(app->esp_proto, f[1], sizeof(app->esp_proto)); copy_str(app->esp_fw, f[2], sizeof(app->esp_fw)); }
    } else if(strncmp(line, "SCAN_BEGIN", 10) == 0) {
        app->net_count = 0;
    } else if(strncmp(line, "NET|", 4) == 0) {
        if(app->net_count < MAX_NETS) {
            char* f[6];
            if(split_pipe(line, f, 6) >= 6) {
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
        if(app->state == StateScanning) { app->state = StateScanList; app->list_index = 0; }
    } else if(strncmp(line, "SAVED_BEGIN", 11) == 0) {
        app->saved_count = 0;
    } else if(strncmp(line, "SAVED|", 6) == 0) {
        if(app->saved_count < MAX_SAVED) {
            char* f[3];
            if(split_pipe(line, f, 3) >= 3) {
                copy_str(app->saved[app->saved_count].ssid, f[1], SSID_LEN);
                app->saved[app->saved_count].autoc = atoi(f[2]);
                app->saved_count++;
            }
        }
    } else if(strncmp(line, "SAVED_CLEARED", 13) == 0) {
        app->saved_count = 0;
    } else if(strncmp(line, "AUTOCONNECT|", 12) == 0) {
        app->auto_reconnect = (line[12] == '1');
    } else if(strncmp(line, "RSSI|", 5) == 0) {
        char* f[3];
        int nf = split_pipe(line, f, 3);
        if(nf >= 3 && app->state == StateSignalMonitor && strcmp(f[1], app->mon_bssid) == 0) {
            if(strcmp(f[2], "NA") != 0) { rssi_push(app, atoi(f[2])); app->mon_have = true; }
        }
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
                if(app->state == StateConnecting) app->state = StateCurrent;
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
        if(furi_stream_buffer_receive(app->rx_stream, &b, 1, 100) == 0) continue;
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

static void rssi_timer_cb(void* ctx) {
    App* app = ctx;
    if(app->state == StateSignalMonitor && app->mon_bssid[0]) {
        char cmd[40];
        snprintf(cmd, sizeof(cmd), "RSSI|%s", app->mon_bssid);
        uart_send_line(app, cmd);
    }
}

// ---------------- drawing ----------------
static void title(Canvas* c, const char* t) {
    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 2, 10, t);
    canvas_draw_line(c, 0, 13, 127, 13);
    canvas_set_font(c, FontSecondary);
}

static void draw_list(Canvas* c, const char** items, int count, int sel, int y0) {
    int rows = 4;
    int start = 0;
    if(sel >= rows) start = sel - rows + 1;
    for(int i = 0; i < rows && start + i < count; i++) {
        int idx = start + i;
        int y = y0 + i * 11;
        if(idx == sel) { canvas_draw_box(c, 0, y - 9, 128, 11); canvas_set_color(c, ColorWhite); }
        canvas_draw_str(c, 4, y, items[idx]);
        if(idx == sel) canvas_set_color(c, ColorBlack);
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
        if(idx == app->list_index) { canvas_draw_box(c, 0, y - 9, 128, 11); canvas_set_color(c, ColorWhite); }
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
    canvas_draw_str(c, 2, 23, n->ssid);
    snprintf(l, sizeof(l), "RSSI %d dBm  Ch %d", n->rssi, n->channel);
    canvas_draw_str(c, 2, 33, l);
    snprintf(l, sizeof(l), "Sec %s  2.4 GHz", n->sec);
    canvas_draw_str(c, 2, 43, l);
    const char* acts[2] = {"Connect", "Analyze signal"};
    for(int i = 0; i < 2; i++) {
        int x = 2 + i * 64;
        if(i == app->detail_action) { canvas_draw_box(c, x - 1, 54, 62, 11); canvas_set_color(c, ColorWhite); }
        canvas_draw_str(c, x + 2, 63, acts[i]);
        if(i == app->detail_action) canvas_set_color(c, ColorBlack);
    }
}

static void draw_keyboard(Canvas* c, App* app) {
    canvas_set_font(c, FontSecondary);
    canvas_draw_str(c, 2, 8, "Password:");
    char shown[22];
    if(app->kb_show) {
        copy_str(shown, app->password, sizeof(shown));
    } else {
        int m = app->pw_len < 21 ? app->pw_len : 21;
        for(int i = 0; i < m; i++) shown[i] = '*';
        shown[m] = 0;
    }
    canvas_draw_str(c, 2, 17, shown);
    canvas_draw_line(c, 0, 19, 127, 19);
    int ys[KB_ROWS] = {28, 36, 44, 52};
    for(int r = 0; r < KB_ROWS; r++) {
        const char* row = app->kb_shift ? KB_UPPER[r] : KB_LOWER[r];
        for(int col = 0; col < KB_COLS; col++) {
            int x = 5 + col * 12;
            char ch[2] = {row[col], 0};
            if(app->kb_row == r && app->kb_col == col) canvas_draw_frame(c, x - 2, ys[r] - 8, 11, 10);
            canvas_draw_str(c, x, ys[r], ch);
        }
    }
    for(int a = 0; a < KB_ACTIONS; a++) {
        int x = 2 + a * 25;
        if(app->kb_row == KB_ROWS && app->kb_col == a) { canvas_draw_box(c, x - 1, 55, 24, 10); canvas_set_color(c, ColorWhite); }
        canvas_draw_str(c, x + 1, 63, KB_ACT[a]);
        if(app->kb_row == KB_ROWS && app->kb_col == a) canvas_set_color(c, ColorBlack);
    }
}

static void draw_connecting(Canvas* c, App* app) {
    title(c, "Connecting");
    char l[48];
    snprintf(l, sizeof(l), "SSID %s", app->target_ssid);
    canvas_draw_str(c, 2, 26, l);
    if(app->cur_connected) {
        canvas_draw_str(c, 2, 40, "Connected!");
    } else if(strcmp(app->cur_state, "ERROR") == 0) {
        canvas_draw_str(c, 2, 40, "Failed (auth?)");
    } else {
        canvas_draw_str(c, 2, 40, "Connecting...");
    }
    canvas_draw_str(c, 2, 63, "Back");
}

static void draw_current(Canvas* c, App* app) {
    title(c, "Current Connection");
    char l[48];
    if(app->cur_connected) {
        canvas_draw_str(c, 2, 23, app->cur_ssid);
        snprintf(l, sizeof(l), "IP %s", app->cur_ip);
        canvas_draw_str(c, 2, 33, l);
        snprintf(l, sizeof(l), "RSSI %d  Ch %d", app->cur_rssi, app->cur_channel);
        canvas_draw_str(c, 2, 43, l);
        snprintf(l, sizeof(l), "GW %s", app->cur_gw);
        canvas_draw_str(c, 2, 53, l);
        canvas_draw_str(c, 2, 63, "OK:refresh  <:disconnect");
    } else {
        const char* st = app->cur_state[0] ? app->cur_state : "Not connected";
        canvas_draw_str(c, 2, 30, st);
        canvas_draw_str(c, 2, 63, "OK:refresh   Back");
    }
}

static void draw_saved(Canvas* c, App* app) {
    char hdr[24];
    snprintf(hdr, sizeof(hdr), "Saved (%d)", app->saved_count);
    title(c, hdr);
    if(app->saved_count == 0) { canvas_draw_str(c, 2, 32, "No saved networks"); return; }
    int rows = 4;
    int start = 0;
    if(app->saved_index >= rows) start = app->saved_index - rows + 1;
    for(int i = 0; i < rows && start + i < app->saved_count; i++) {
        int idx = start + i;
        int y = 24 + i * 11;
        if(idx == app->saved_index) { canvas_draw_box(c, 0, y - 9, 128, 11); canvas_set_color(c, ColorWhite); }
        canvas_draw_str(c, 2, y, app->saved[idx].ssid);
        if(app->saved[idx].autoc) canvas_draw_str_aligned(c, 126, y, AlignRight, AlignBottom, "auto");
        if(idx == app->saved_index) canvas_set_color(c, ColorBlack);
    }
}

static void draw_saved_action(Canvas* c, App* app) {
    title(c, "Saved Network");
    if(app->saved_count == 0) return;
    canvas_draw_str(c, 2, 23, app->saved[app->saved_index].ssid);
    const char* acts[4] = {"Connect", "Toggle auto-connect", "Forget", "Back"};
    draw_list(c, acts, 4, app->saved_action, 34);
}

static void draw_analyzer_menu(Canvas* c, App* app) {
    title(c, "Analyzer");
    const char* items[3] = {"Signal Monitor", "Channel View", "Back"};
    draw_list(c, items, 3, app->analyzer_index, 26);
    if(app->net_count == 0) canvas_draw_str(c, 2, 63, "Scan networks first");
}

static void draw_signal_monitor(Canvas* c, App* app) {
    char l[40];
    canvas_set_font(c, FontSecondary);
    snprintf(l, sizeof(l), "%s  Ch %d", app->mon_ssid, app->mon_channel);
    canvas_draw_str(c, 2, 8, l);
    canvas_draw_line(c, 0, 10, 127, 10);
    if(!app->mon_have) { canvas_draw_str(c, 2, 30, "Sampling..."); return; }
    int last = app->rssi_hist[app->rssi_hist_len - 1];
    snprintf(l, sizeof(l), "RSSI %d dBm", last);
    canvas_draw_str(c, 2, 20, l);
    int y_top = 24, y_bot = 62;
    for(int i = 1; i < app->rssi_hist_len; i++) {
        int r0 = app->rssi_hist[i - 1];
        int r1 = app->rssi_hist[i];
        if(r0 < -90) r0 = -90; if(r0 > -30) r0 = -30;
        if(r1 < -90) r1 = -90; if(r1 > -30) r1 = -30;
        int y0 = y_bot - ((r0 + 90) * (y_bot - y_top)) / 60;
        int y1 = y_bot - ((r1 + 90) * (y_bot - y_top)) / 60;
        int x0 = 4 + (i - 1) * 3;
        int x1 = 4 + i * 3;
        canvas_draw_line(c, x0, y0, x1, y1);
    }
}

static void draw_channel_view(Canvas* c, App* app) {
    title(c, "Channel View 2.4G");
    int counts[15];
    for(int i = 0; i < 15; i++) counts[i] = 0;
    for(int i = 0; i < app->net_count; i++) {
        int ch = app->nets[i].channel;
        if(ch >= 1 && ch <= 14) counts[ch]++;
    }
    int mx = 1;
    for(int ch = 1; ch <= 13; ch++) if(counts[ch] > mx) mx = counts[ch];
    for(int ch = 1; ch <= 13; ch++) {
        int x = 4 + (ch - 1) * 9;
        int h = (counts[ch] * 34) / mx;
        if(h > 0) canvas_draw_box(c, x, 58 - h, 7, h);
        else canvas_draw_line(c, x, 58, x + 6, 58);
    }
    canvas_draw_str(c, 2, 63, "CH 1 . . . . . . . . . 13");
}

static void draw_settings(Canvas* c, App* app) {
    title(c, "Settings");
    char l0[28];
    snprintf(l0, sizeof(l0), "Auto reconnect: %s", app->auto_reconnect ? "ON" : "OFF");
    const char* items[4] = {l0, "Reset saved WiFi", "ESP32 version", "Back"};
    draw_list(c, items, 4, app->settings_index, 24);
    if(app->esp_fw[0]) {
        char v[28];
        snprintf(v, sizeof(v), "FW %s  proto %s", app->esp_fw, app->esp_proto);
        canvas_draw_str(c, 2, 63, v);
    }
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
        title(c, "WiFi Toolkit");
        draw_list(c, MENU_ITEMS, MENU_COUNT, app->menu_index, 24);
        break;
    case StateScanning: {
        title(c, "Scanning...");
        char l[24];
        snprintf(l, sizeof(l), "%d found", app->net_count);
        canvas_draw_str(c, 2, 32, l);
        canvas_draw_str(c, 2, 63, "Back: cancel");
        break;
    }
    case StateScanList: draw_scan_list(c, app); break;
    case StateDetail: draw_detail(c, app); break;
    case StateKeyboard: draw_keyboard(c, app); break;
    case StateConnecting: draw_connecting(c, app); break;
    case StateCurrent: draw_current(c, app); break;
    case StateSaved: draw_saved(c, app); break;
    case StateSavedAction: draw_saved_action(c, app); break;
    case StateAnalyzerMenu: draw_analyzer_menu(c, app); break;
    case StateSignalMonitor: draw_signal_monitor(c, app); break;
    case StateChannelView: draw_channel_view(c, app); break;
    case StateSettings: draw_settings(c, app); break;
    }
    furi_mutex_release(app->mutex);
}

// ---------------- input ----------------
static void input_cb(InputEvent* e, void* ctx) {
    App* app = ctx;
    furi_message_queue_put(app->input_queue, e, 0);
}

static void start_connect(App* app, const char* ssid, bool open) {
    copy_str(app->target_ssid, ssid, SSID_LEN);
    app->cur_connected = false;
    copy_str(app->cur_state, "CONNECTING", sizeof(app->cur_state));
    if(open) {
        char cmd[80];
        snprintf(cmd, sizeof(cmd), "CONNECT|%s|", ssid);
        uart_send_line(app, cmd);
        app->state = StateConnecting;
    } else {
        app->pw_len = 0;
        app->password[0] = 0;
        app->kb_row = 0;
        app->kb_col = 0;
        app->kb_shift = false;
        app->kb_show = false;
        app->state = StateKeyboard;
    }
}

static void start_monitor(App* app, WifiNet* n) {
    copy_str(app->mon_ssid, n->ssid, SSID_LEN);
    copy_str(app->mon_bssid, n->bssid, sizeof(app->mon_bssid));
    app->mon_channel = n->channel;
    app->rssi_hist_len = 0;
    app->mon_have = false;
    app->state = StateSignalMonitor;
    furi_timer_start(app->rssi_timer, 1500);
}

static void kb_submit_char(App* app) {
    if(app->kb_row < KB_ROWS) {
        const char* row = app->kb_shift ? KB_UPPER[app->kb_row] : KB_LOWER[app->kb_row];
        char ch = row[app->kb_col];
        if(app->pw_len < 63) { app->password[app->pw_len++] = ch; app->password[app->pw_len] = 0; }
    } else {
        switch(app->kb_col) {
        case 0: app->kb_shift = !app->kb_shift; break;
        case 1: if(app->pw_len < 63) { app->password[app->pw_len++] = ' '; app->password[app->pw_len] = 0; } break;
        case 2: if(app->pw_len > 0) { app->pw_len--; app->password[app->pw_len] = 0; } break;
        case 3: app->kb_show = !app->kb_show; break;
        case 4: {
            char cmd[128];
            snprintf(cmd, sizeof(cmd), "CONNECT|%s|%s", app->target_ssid, app->password);
            uart_send_line(app, cmd);
            app->cur_connected = false;
            copy_str(app->cur_state, "CONNECTING", sizeof(app->cur_state));
            app->state = StateConnecting;
            break;
        }
        }
    }
}

static void menu_select(App* app) {
    switch(app->menu_index) {
    case 0: app->net_count = 0; app->state = StateScanning; uart_send_line(app, "SCAN"); break;
    case 1: app->saved_count = 0; app->saved_index = 0; app->state = StateSaved; uart_send_line(app, "SAVED_LIST"); break;
    case 2: app->cur_state[0] = 0; app->state = StateCurrent; uart_send_line(app, "STATUS"); break;
    case 3: app->analyzer_index = 0; app->state = StateAnalyzerMenu; break;
    case 4: app->settings_index = 0; app->state = StateSettings; uart_send_line(app, "VERSION"); break;
    }
}

static void saved_action_select(App* app) {
    SavedNet* s = &app->saved[app->saved_index];
    char cmd[80];
    switch(app->saved_action) {
    case 0:
        copy_str(app->target_ssid, s->ssid, SSID_LEN);
        app->cur_connected = false;
        copy_str(app->cur_state, "CONNECTING", sizeof(app->cur_state));
        snprintf(cmd, sizeof(cmd), "SAVED_CONNECT|%s", s->ssid);
        uart_send_line(app, cmd);
        app->state = StateConnecting;
        break;
    case 1:
        snprintf(cmd, sizeof(cmd), "SAVED_AUTO|%s|%d", s->ssid, s->autoc ? 0 : 1);
        uart_send_line(app, cmd);
        app->state = StateSaved;
        break;
    case 2:
        snprintf(cmd, sizeof(cmd), "SAVED_DELETE|%s", s->ssid);
        uart_send_line(app, cmd);
        app->state = StateSaved;
        break;
    default:
        app->state = StateSaved;
        break;
    }
}

static void settings_select(App* app) {
    switch(app->settings_index) {
    case 0:
        app->auto_reconnect = !app->auto_reconnect;
        uart_send_line(app, app->auto_reconnect ? "AUTOCONNECT|1" : "AUTOCONNECT|0");
        break;
    case 1: uart_send_line(app, "SAVED_CLEAR"); break;
    case 2: uart_send_line(app, "VERSION"); break;
    default: app->state = StateMenu; break;
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
        else if(key == InputKeyOk) { if(app->net_count > 0) { app->detail_action = 0; app->state = StateDetail; } }
        else if(key == InputKeyRight) { app->net_count = 0; app->state = StateScanning; uart_send_line(app, "SCAN"); }
        else if(key == InputKeyBack) app->state = StateMenu;
        break;
    case StateDetail:
        if(key == InputKeyLeft || key == InputKeyUp) app->detail_action = 0;
        else if(key == InputKeyRight || key == InputKeyDown) app->detail_action = 1;
        else if(key == InputKeyOk) {
            WifiNet* n = &app->nets[app->list_index];
            if(app->detail_action == 0) start_connect(app, n->ssid, strcmp(n->sec, "OPEN") == 0);
            else start_monitor(app, n);
        } else if(key == InputKeyBack) app->state = StateScanList;
        break;
    case StateKeyboard:
        if(key == InputKeyUp) { app->kb_row = (app->kb_row + KB_ROWS) % (KB_ROWS + 1); if(app->kb_col >= kb_cols(app->kb_row)) app->kb_col = kb_cols(app->kb_row) - 1; }
        else if(key == InputKeyDown) { app->kb_row = (app->kb_row + 1) % (KB_ROWS + 1); if(app->kb_col >= kb_cols(app->kb_row)) app->kb_col = kb_cols(app->kb_row) - 1; }
        else if(key == InputKeyLeft) { app->kb_col = (app->kb_col + kb_cols(app->kb_row) - 1) % kb_cols(app->kb_row); }
        else if(key == InputKeyRight) { app->kb_col = (app->kb_col + 1) % kb_cols(app->kb_row); }
        else if(key == InputKeyOk) kb_submit_char(app);
        else if(key == InputKeyBack) app->state = StateDetail;
        break;
    case StateConnecting:
        if(key == InputKeyBack) app->state = StateMenu;
        break;
    case StateCurrent:
        if(key == InputKeyOk) uart_send_line(app, "STATUS");
        else if(key == InputKeyLeft) uart_send_line(app, "DISCONNECT");
        else if(key == InputKeyBack) app->state = StateMenu;
        break;
    case StateSaved:
        if(key == InputKeyUp) { if(app->saved_index > 0) app->saved_index--; }
        else if(key == InputKeyDown) { if(app->saved_index < app->saved_count - 1) app->saved_index++; }
        else if(key == InputKeyOk) { if(app->saved_count > 0) { app->saved_action = 0; app->state = StateSavedAction; } }
        else if(key == InputKeyBack) app->state = StateMenu;
        break;
    case StateSavedAction:
        if(key == InputKeyUp) app->saved_action = (app->saved_action + 3) % 4;
        else if(key == InputKeyDown) app->saved_action = (app->saved_action + 1) % 4;
        else if(key == InputKeyOk) saved_action_select(app);
        else if(key == InputKeyBack) app->state = StateSaved;
        break;
    case StateAnalyzerMenu:
        if(key == InputKeyUp) app->analyzer_index = (app->analyzer_index + 2) % 3;
        else if(key == InputKeyDown) app->analyzer_index = (app->analyzer_index + 1) % 3;
        else if(key == InputKeyOk) {
            if(app->analyzer_index == 0) { if(app->net_count > 0) start_monitor(app, &app->nets[app->list_index]); }
            else if(app->analyzer_index == 1) { if(app->net_count > 0) app->state = StateChannelView; }
            else app->state = StateMenu;
        } else if(key == InputKeyBack) app->state = StateMenu;
        break;
    case StateSignalMonitor:
        if(key == InputKeyBack) { furi_timer_stop(app->rssi_timer); app->state = StateAnalyzerMenu; }
        break;
    case StateChannelView:
        if(key == InputKeyBack) app->state = StateAnalyzerMenu;
        break;
    case StateSettings:
        if(key == InputKeyUp) app->settings_index = (app->settings_index + 3) % 4;
        else if(key == InputKeyDown) app->settings_index = (app->settings_index + 1) % 4;
        else if(key == InputKeyOk) settings_select(app);
        else if(key == InputKeyBack) app->state = StateMenu;
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
    app->rssi_timer = furi_timer_alloc(rssi_timer_cb, FuriTimerTypePeriodic, app);
    app->running = true;
    app->state = StateDetect;
    app->auto_reconnect = true;

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
    furi_timer_stop(app->rssi_timer);
    furi_thread_join(app->worker);
    furi_thread_free(app->worker);

    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_GUI);

    furi_hal_serial_async_rx_stop(app->serial);
    furi_hal_serial_deinit(app->serial);
    furi_hal_serial_control_release(app->serial);

    furi_timer_free(app->rssi_timer);
    furi_stream_buffer_free(app->rx_stream);
    furi_message_queue_free(app->input_queue);
    furi_mutex_free(app->mutex);
    free(app);
    return 0;
}
