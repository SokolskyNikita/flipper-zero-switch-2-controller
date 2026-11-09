/*
 * Frame Perfect: Flipper Zero as a wired Switch / Switch 2 controller with
 * frame-timed sequences and poll-rate testing.
 *
 * Home has two entries:
 *   Automatic test  tries every identity / poll interval, measures how often
 *                   the console really reads us, recommends the best setting
 *                   and saves it to settings.txt.
 *   Manual test     starts from the saved recommendation (or from the
 *                   defaults below), lets you change it, then connects for
 *                   live control or sequences.
 *
 * Derived from Controller Ducky (https://github.com/eric-m-holub/controller-ducky,
 * Apache-2.0, commit d63fbbe): the Pro Controller handshake, subcommand
 * replies and fake SPI flash below come from there. Everything else (UI,
 * sequences, measurements, HORI mode, headless test mode) is new.
 *
 * Headless use (from the Flipper CLI, for tests with a computer as host):
 *   loader open /ext/apps/USB/frame_perfect.fap "auto mode=pro int=1 seq=tap_test.txt runs=2 quit=20"
 *   loader open /ext/apps/USB/frame_perfect.fap "autotest quit"
 * Results go to /ext/apps_data/frame_perfect/log.txt.
 */
#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/elements.h>
#include <input/input.h>
#include <storage/storage.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pad_usb.h"
#include "seq.h"
#include "fp_ble.h"

#define TAG "FramePerfect"
#define LOG_PATH APP_DATA_PATH("log.txt")
#define SEQ_DIR APP_DATA_PATH("sequences")
#define SETTINGS_PATH APP_DATA_PATH("settings.txt")

/* Used when no automatic test has produced a setting yet: what a real Pro
 * Controller does over USB. */
#define DEFAULT_KIND PadPro
#define DEFAULT_INTERVAL 8
#define TAP_MS 100
#define PICKER_MAX 24
#define PICKER_NAME 40

/* ------------------------------------------------------------------ */
/* Pro Controller handshake (from Controller Ducky)                   */
/* ------------------------------------------------------------------ */

static const uint8_t SW_MAC[6] = {0x02, 0xF1, 0x95, 0x00, 0x00, 0x01};

static const uint8_t spi_imu_cal[24] = {
    0xD3, 0xFF, 0xD5, 0xFF, 0x55, 0x01, 0x00, 0x40, 0x00, 0x40, 0x00, 0x40,
    0x19, 0x00, 0xDD, 0xFF, 0xDC, 0xFF, 0x3B, 0x34, 0x3B, 0x34, 0x3B, 0x34,
};
static const uint8_t spi_stick_cal[18] = {
    0xDC, 0xC5, 0x5D, 0x00, 0x08, 0x80, 0xDC, 0xC5, 0x5D,
    0x00, 0x08, 0x80, 0xDC, 0xC5, 0x5D, 0xDC, 0xC5, 0x5D,
};
static const uint8_t spi_colors[12] = {
    0x32, 0x32, 0x32, 0xFF, 0xFF, 0xFF, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32,
};
static const uint8_t spi_sensor_off[6] = {0x50, 0xFD, 0x00, 0x00, 0xC6, 0x0F};
static const uint8_t spi_stick_params[18] = {
    0x0F, 0x30, 0x61, 0x96, 0x30, 0xF3, 0xD4, 0x14, 0x54,
    0x41, 0x15, 0x54, 0xC7, 0x79, 0x9C, 0x33, 0x36, 0x63,
};

typedef struct {
    uint32_t addr;
    const uint8_t* data;
    uint8_t len;
} SpiRegion;

static const SpiRegion spi_regions[] = {
    {0x6020, spi_imu_cal, sizeof(spi_imu_cal)},
    {0x603D, spi_stick_cal, sizeof(spi_stick_cal)},
    {0x6050, spi_colors, sizeof(spi_colors)},
    {0x6080, spi_sensor_off, sizeof(spi_sensor_off)},
    {0x6086, spi_stick_params, sizeof(spi_stick_params)},
    {0x6098, spi_stick_params, sizeof(spi_stick_params)},
};

static void spi_read(uint32_t addr, uint8_t len, uint8_t* out) {
    memset(out, 0xFF, len);
    for(size_t i = 0; i < COUNT_OF(spi_regions); i++) {
        const SpiRegion* r = &spi_regions[i];
        uint32_t start = addr > r->addr ? addr : r->addr;
        uint32_t a_end = addr + len;
        uint32_t r_end = r->addr + r->len;
        uint32_t end = a_end < r_end ? a_end : r_end;
        if(start < end) memcpy(&out[start - addr], &r->data[start - r->addr], end - start);
    }
}

/* ------------------------------------------------------------------ */
/* App                                                                */
/* ------------------------------------------------------------------ */

typedef enum {
    EvInput,
    EvUsbRx,
} EvType;

typedef struct {
    EvType type;
    union {
        InputEvent input;
        PadRx rx;
    };
} AppEvent;

typedef enum {
    ScrHome,
    ScrManual,
    ScrPad,
    ScrLive,
    ScrPicker,
    ScrRun,
    ScrAuto,
    ScrPrompt,
} Screen;

typedef enum {
    LayDpad,
    LayLStick,
    LayShoulder,
    LaySystem,
    LayCount,
} Layout;

static const char* const layout_names[LayCount] = {"D-pad", "L-stick", "Shoulder", "System"};
static const char* const layout_hints[LayCount] = {
    "Arrows D-pad  OK A  Back B",
    "Arrows L-stick OK A Back B",
    "U ZL D ZR L L R R OK X Bk Y",
    "L Minus R Plus OK Home",
};

static const uint8_t interval_choices[] = {8, 5, 4, 2, 1};

typedef struct {
    PadKind kind;
    uint8_t interval;
} SweepCfg;

static const SweepCfg sweep_cfgs[] = {
    {PadPro, 8},
    {PadPro, 4},
    {PadPro, 2},
    {PadPro, 1},
    {PadHori, 8},
    {PadHori, 5},
    {PadHori, 4},
    {PadHori, 1},
};
#define SWEEP_N COUNT_OF(sweep_cfgs)
#define SWEEP_WAIT_MS 10000
#define SWEEP_MEASURE_S 5

typedef struct {
    bool streamed;
    uint32_t to_stream_ms;
    uint32_t polls;
    uint32_t mean_us;
    uint32_t min_us;
    uint32_t max_us;
} SweepResult;

typedef struct {
    Gui* gui;
    ViewPort* vp;
    FuriMessageQueue* queue;
    bool running;
    Screen screen;
    uint8_t sel; /* menu cursor on the current screen */

    /* chosen identity */
    PadKind kind;
    uint8_t interval_idx;

    /* poll statistics, refreshed every second */
    uint32_t stats_at;
    PollStats last;
    bool have_last;

    /* handshake trace */
    uint8_t seen80; /* bit n = 0x80 0x0n seen */
    uint32_t seen_sub[8]; /* bitset of 0x01 subcommands */
    uint32_t rx_count;

    /* live control */
    Layout layout;
    bool key_down[InputKeyMAX];
    uint32_t tap_until[InputKeyMAX];

    /* sequences */
    char names[PICKER_MAX][PICKER_NAME];
    uint8_t n_names;
    char seq_name[PICKER_NAME];
    Sequence* seq;
    bool seq_loaded;
    char seq_err[32];
    uint16_t seq_err_line;
    bool was_playing;
    uint32_t run_started;

    /* sweep */
    bool sweeping;
    uint8_t sweep_i;
    uint8_t sweep_phase; /* 0 start, 1 wait for stream, 2 measure, 3 done */
    uint32_t sweep_t0;
    uint8_t sweep_secs;
    PollStats sweep_acc;
    SweepResult sweep_res[SWEEP_N];
    bool quit_after_sweep;

    /* recommended setting: from settings.txt / the last automatic test */
    bool rec_valid; /* false: defaults in use */
    PadKind rec_kind;
    uint8_t rec_interval;
    uint32_t rec_mean_us;
    char rec_when[24];
    bool auto_failed; /* the last automatic test found nothing that worked */
    bool rec_confirmed; /* you saw the Switch react to it */

    /* guided steps: what the screen asks, and which step answers go to */
    uint8_t flow;
    const char* p_title;
    const char* p_lines[4];
    const char* p_keys; /* bottom line, e.g. "OK: yes   Back: no" */
    bool p_wait; /* no keys: the Flipper is busy, the screen just reports */
    uint32_t flow_t0;
    int8_t rank[SWEEP_N]; /* sweep results, best first, -1 = end */
    uint8_t rank_pos;
    bool headless_sweep; /* started from CLI args: no questions */
    Sequence* probe; /* stick right, right, left, left */
    bool game_flipper_ok;
    char p_buf[32];
    char auto_answer; /* test only: 'y' / 'n' answers the questions */
    bool ble_active;
    uint32_t ble_quit_ms;
    uint32_t ble_t0;
    bool ble_seen_connected;

    /* remote mode: the Mac drives over Bluetooth */
    bool remote;
    bool have_grid;
    uint32_t grid_period_ns;
    uint64_t grid_anchor_us; /* a frame start, pad_now_us clock */
    uint32_t grid_updates;
    uint16_t go_id;
    bool remote_run; /* the current run was started by a go */
    char r_line[3][48];
    uint32_t hold_until; /* tick when an 'H' hold ends, 0 = none */

    /* log bookkeeping */
    PadUsbEvents ev_seen;
    bool streaming_seen;
    uint32_t stats_logged_at;
    uint32_t rx_logged; /* raw OUT packets logged on this connection */
    uint32_t rumble_only; /* 0x10 packets, counted not logged */
    uint32_t live_buttons_seen;
    int8_t live_sticks_seen;

    /* headless auto test */
    bool autotest;
    uint32_t auto_quit_ms;
    uint32_t auto_t0;
    uint8_t auto_runs;
    uint8_t auto_runs_done;
    bool auto_seq_ok;
} App;

static void app_redraw(App* app) {
    view_port_update(app->vp);
}

/* ------------------------------------------------------------------ */
/* Log file                                                           */
/* ------------------------------------------------------------------ */

static void log_line(const char* fmt, ...) {
    char buf[200];
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    int n = snprintf(
        buf,
        sizeof(buf),
        "%04u-%02u-%02u %02u:%02u:%02u ",
        dt.year,
        dt.month,
        dt.day,
        dt.hour,
        dt.minute,
        dt.second);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + n, sizeof(buf) - n - 2, fmt, ap);
    va_end(ap);
    strcat(buf, "\n");

    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, APP_DATA_PATH(""));
    File* f = storage_file_alloc(storage);
    if(storage_file_open(f, LOG_PATH, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        storage_file_write(f, buf, strlen(buf));
    }
    storage_file_close(f);
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
    FURI_LOG_I(TAG, "%s", buf);
}

static const char* kind_name(PadKind k) {
    return k == PadHori ? "HORI" : "Pro";
}

static void log_stats(const char* what, const PollStats* s) {
    uint32_t mean = s->polls ? s->sum_us / s->polls : 0;
    char hist[96];
    int n = 0;
    for(int i = 0; i < 17; i++) {
        if(!s->hist[i]) continue;
        n += snprintf(
            hist + n, sizeof(hist) - n, " %u.%u:%lu", i / 2, (i % 2) * 5, (unsigned long)s->hist[i]);
        if(n >= (int)sizeof(hist)) break;
    }
    hist[sizeof(hist) - 1] = '\0';
    log_line(
        "%s %s int=%u polls=%lu mean=%luus min=%luus max=%luus fail=%lu hist[ms:n]%s",
        what,
        kind_name(pad_usb_kind()),
        pad_usb_interval(),
        (unsigned long)s->polls,
        (unsigned long)mean,
        (unsigned long)(s->polls ? s->min_us : 0),
        (unsigned long)s->max_us,
        (unsigned long)s->write_fail,
        hist);
}

static void log_handshake(App* app) {
    char subs[96];
    int n = 0;
    for(int i = 0; i < 256; i++) {
        if(app->seen_sub[i / 32] & (1u << (i % 32))) {
            n += snprintf(subs + n, sizeof(subs) - n, " %02X", i);
            if(n >= (int)sizeof(subs)) break;
        }
    }
    subs[sizeof(subs) - 1] = '\0';
    log_line("handshake 80-cmds=0x%02X subcmds:%s", app->seen80, n ? subs : " none");
}

/* ------------------------------------------------------------------ */
/* USB RX (Pro handshake)                                             */
/* ------------------------------------------------------------------ */

static void on_rx(const PadRx* rx, void* ctx) {
    App* app = ctx;
    AppEvent ev = {.type = EvUsbRx};
    ev.rx = *rx;
    furi_message_queue_put(app->queue, &ev, 0);
}

static void send_subcmd_reply(uint8_t ack, uint8_t subcmd, const uint8_t* data, size_t len) {
    uint8_t r[64];
    PadState idle = {0};
    pad_pro_fill(r, 0x21, &idle);
    r[13] = ack;
    r[14] = subcmd;
    if(len > sizeof(r) - 15) len = sizeof(r) - 15;
    if(data && len) memcpy(&r[15], data, len);
    pad_usb_send_reply(r, 64);
}

static void handle_subcommand(const uint8_t* rx) {
    uint8_t sub = rx[10];
    const uint8_t* arg = &rx[11];
    switch(sub) {
    case 0x01: {
        uint8_t d[1] = {0x03};
        send_subcmd_reply(0x81, sub, d, 1);
        break;
    }
    case 0x02: {
        uint8_t d[12] = {0x03, 0x48, 0x03, 0x02};
        memcpy(&d[4], SW_MAC, 6);
        d[10] = 0x01;
        d[11] = 0x01;
        send_subcmd_reply(0x82, sub, d, sizeof(d));
        break;
    }
    case 0x04:
        send_subcmd_reply(0x83, sub, NULL, 0);
        break;
    case 0x10: {
        uint32_t addr = arg[0] | ((uint32_t)arg[1] << 8) | ((uint32_t)arg[2] << 16) |
                        ((uint32_t)arg[3] << 24);
        uint8_t len = arg[4];
        if(len > 0x1D) len = 0x1D;
        uint8_t d[5 + 0x1D];
        memcpy(d, arg, 4);
        d[4] = len;
        spi_read(addr, len, &d[5]);
        send_subcmd_reply(0x90, sub, d, 5 + len);
        break;
    }
    case 0x21: {
        static const uint8_t d[] = {0x01, 0x00, 0xFF, 0x00, 0x03, 0x00, 0x05, 0x01};
        send_subcmd_reply(0xA0, sub, d, sizeof(d));
        break;
    }
    default:
        send_subcmd_reply(0x80, sub, NULL, 0);
        break;
    }
}

static void log_rx(App* app, const PadRx* rx) {
    /* Pro: rumble-only packets (0x10) arrive constantly once streaming;
     * count them. Log everything else, up to 120 packets per connection. */
    if(pad_usb_kind() == PadPro && rx->len && rx->data[0] == 0x10) {
        app->rumble_only++;
        return;
    }
    if(app->rx_logged >= 120) return;
    app->rx_logged++;
    char hex[3 * 20 + 1];
    int n = 0;
    uint8_t show = rx->len < 20 ? rx->len : 20;
    for(uint8_t i = 0; i < show; i++) n += snprintf(hex + n, sizeof(hex) - n, "%02X ", rx->data[i]);
    hex[n ? n - 1 : 0] = '\0';
    log_line("rx #%lu len=%u: %s%s", (unsigned long)app->rx_logged, rx->len, hex, rx->len > 20 ? " ..." : "");
}

static void handle_rx(App* app, const PadRx* rx) {
    app->rx_count++;
    log_rx(app, rx);
    if(pad_usb_kind() != PadPro || rx->len < 2) return;
    switch(rx->data[0]) {
    case 0x80: {
        uint8_t c = rx->data[1];
        if(c < 8) app->seen80 |= (uint8_t)(1u << c);
        uint8_t r[64] = {0};
        switch(c) {
        case 0x01:
            r[0] = 0x81;
            r[1] = 0x01;
            r[3] = 0x03; /* Pro Controller */
            for(int i = 0; i < 6; i++) r[4 + i] = SW_MAC[5 - i];
            pad_usb_send_reply(r, 64);
            break;
        case 0x02:
        case 0x03:
            r[0] = 0x81;
            r[1] = c;
            pad_usb_send_reply(r, 64);
            break;
        case 0x04:
            pad_usb_set_streaming(true);
            break;
        case 0x05:
            pad_usb_set_streaming(false);
            break;
        default:
            break;
        }
        break;
    }
    case 0x01:
        if(rx->len >= 11) {
            uint8_t s = rx->data[10];
            app->seen_sub[s / 32] |= 1u << (s % 32);
            handle_subcommand(rx->data);
        }
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Connection                                                         */
/* ------------------------------------------------------------------ */

static void reset_trace(App* app) {
    app->rx_logged = 0;
    app->rumble_only = 0;
    app->streaming_seen = false;
    app->seen80 = 0;
    memset(app->seen_sub, 0, sizeof(app->seen_sub));
    app->rx_count = 0;
    app->have_last = false;
}

static bool connect_pad(App* app, PadKind kind, uint8_t interval) {
    seq_stop();
    PadState idle = {0};
    seq_set_live(&idle);
    reset_trace(app);
    bool ok = pad_usb_start(kind, interval);
    log_line("connect %s int=%u %s", kind_name(kind), interval, ok ? "ok" : "REFUSED");
    PollStats junk;
    pad_usb_take_stats(&junk);
    app->stats_at = furi_get_tick();
    return ok;
}

static const char* usb_state_text(void) {
    if(!pad_usb_active()) return "off";
    if(!pad_usb_configured()) return "waiting for host";
    if(pad_usb_suspended()) return "host asleep";
    if(!pad_usb_streaming()) return "handshake";
    return "streaming";
}

/* ------------------------------------------------------------------ */
/* Live control                                                       */
/* ------------------------------------------------------------------ */

static void live_update(App* app) {
    PadState st = {0};
    bool* k = app->key_down;
    switch(app->layout) {
    case LayDpad:
        if(k[InputKeyUp]) st.buttons |= BTN_UP;
        if(k[InputKeyDown]) st.buttons |= BTN_DOWN;
        if(k[InputKeyLeft]) st.buttons |= BTN_LEFT;
        if(k[InputKeyRight]) st.buttons |= BTN_RIGHT;
        if(k[InputKeyOk]) st.buttons |= BTN_A;
        if(k[InputKeyBack]) st.buttons |= BTN_B;
        break;
    case LayLStick:
        st.ly = (int8_t)((k[InputKeyUp] ? 1 : 0) - (k[InputKeyDown] ? 1 : 0));
        st.lx = (int8_t)((k[InputKeyRight] ? 1 : 0) - (k[InputKeyLeft] ? 1 : 0));
        if(k[InputKeyOk]) st.buttons |= BTN_A;
        if(k[InputKeyBack]) st.buttons |= BTN_B;
        break;
    case LayShoulder:
        if(k[InputKeyUp]) st.buttons |= BTN_ZL;
        if(k[InputKeyDown]) st.buttons |= BTN_ZR;
        if(k[InputKeyLeft]) st.buttons |= BTN_L;
        if(k[InputKeyRight]) st.buttons |= BTN_R;
        if(k[InputKeyOk]) st.buttons |= BTN_X;
        if(k[InputKeyBack]) st.buttons |= BTN_Y;
        break;
    case LaySystem:
        if(k[InputKeyLeft]) st.buttons |= BTN_MINUS;
        if(k[InputKeyRight]) st.buttons |= BTN_PLUS;
        if(k[InputKeyOk]) st.buttons |= BTN_HOME;
        break;
    default:
        break;
    }
    seq_set_live(&st);
    int8_t sticks = (int8_t)((st.lx & 3) | ((st.ly & 3) << 2));
    if(st.buttons != app->live_buttons_seen || sticks != app->live_sticks_seen) {
        app->live_buttons_seen = st.buttons;
        app->live_sticks_seen = sticks;
        char names[64];
        int n = 0;
        names[0] = '\0';
        for(size_t i = 0; i < seq_button_count; i++) {
            if(st.buttons & seq_button_names[i].mask)
                n += snprintf(names + n, sizeof(names) - n, "%s ", seq_button_names[i].name);
            if(n >= (int)sizeof(names)) break;
        }
        if(st.lx) n += snprintf(names + n, sizeof(names) - n, "LX%c ", st.lx > 0 ? '+' : '-');
        if(st.ly && n < (int)sizeof(names)) n += snprintf(names + n, sizeof(names) - n, "LY%c ", st.ly > 0 ? '+' : '-');
        names[sizeof(names) - 1] = '\0';
        log_line("live %s: %s", layout_names[app->layout], n ? names : "released");
    }
}

static void live_release_all(App* app) {
    memset(app->key_down, 0, sizeof(app->key_down));
    memset(app->tap_until, 0, sizeof(app->tap_until));
    live_update(app);
}

static void live_expire_taps(App* app) {
    uint32_t now = furi_get_tick();
    bool changed = false;
    for(int i = 0; i < InputKeyMAX; i++) {
        if(app->tap_until[i] && (int32_t)(now - app->tap_until[i]) >= 0) {
            app->tap_until[i] = 0;
            app->key_down[i] = false;
            changed = true;
        }
    }
    if(changed) {
        live_update(app);
        app_redraw(app);
    }
}

/* ------------------------------------------------------------------ */
/* Sequences                                                          */
/* ------------------------------------------------------------------ */

static void picker_scan(App* app) {
    app->n_names = 0;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, APP_DATA_PATH(""));
    storage_simply_mkdir(storage, SEQ_DIR);
    File* f = storage_file_alloc(storage);
    if(storage_dir_open(f, SEQ_DIR)) {
        FileInfo info;
        char name[PICKER_NAME];
        while(app->n_names < PICKER_MAX && storage_dir_read(f, &info, name, sizeof(name))) {
            if(file_info_is_dir(&info)) continue;
            size_t len = strlen(name);
            if(len > 4 && strcmp(name + len - 4, ".txt") == 0) {
                snprintf(app->names[app->n_names++], PICKER_NAME, "%s", name);
            }
        }
        storage_dir_close(f);
    }
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
    for(int i = 1; i < app->n_names; i++) {
        char tmp[PICKER_NAME];
        memcpy(tmp, app->names[i], PICKER_NAME);
        int j = i - 1;
        while(j >= 0 && strcmp(app->names[j], tmp) > 0) {
            memcpy(app->names[j + 1], app->names[j], PICKER_NAME);
            j--;
        }
        memcpy(app->names[j + 1], tmp, PICKER_NAME);
    }
}

static bool seq_open(App* app, const char* name) {
    char path[96];
    snprintf(path, sizeof(path), SEQ_DIR "/%s", name);
    snprintf(app->seq_name, sizeof(app->seq_name), "%s", name);
    app->seq_loaded =
        seq_load(app->seq, path, app->seq_err, sizeof(app->seq_err), &app->seq_err_line);
    if(!app->seq_loaded) {
        log_line("seq %s: error line %u: %s", name, app->seq_err_line, app->seq_err);
    }
    return app->seq_loaded;
}

static void log_run_end(App* app, const char* how) {
    char ps[120];
    int n = 0;
    ps[0] = '\0';
    for(int i = 0; i < app->seq->n_params; i++) {
        n += snprintf(
            ps + n, sizeof(ps) - n, " %s=%d", app->seq->params[i].name, app->seq->params[i].value);
        if(n >= (int)sizeof(ps)) break;
    }
    ps[sizeof(ps) - 1] = '\0';
    log_line(
        "run %s %s cycles=%lu %s int=%u%s",
        app->seq_name,
        how,
        (unsigned long)seq_cycles_done(),
        kind_name(pad_usb_kind()),
        pad_usb_interval(),
        ps);

    SeqEdge edges[SEQ_EDGE_LOG];
    uint8_t ne = seq_edges(edges, SEQ_EDGE_LOG);
    log_line(
        "edges: %u logged of %lu (us from start : buttons hex, stick bits 24-31)",
        ne,
        (unsigned long)seq_edge_total());
    char line[180];
    int m = 0;
    for(uint8_t i = 0; i < ne; i++) {
        m += snprintf(
            line + m,
            sizeof(line) - m,
            " %lu:%lX",
            (unsigned long)edges[i].t_us,
            (unsigned long)edges[i].buttons);
        if(m > (int)sizeof(line) - 24 || i == ne - 1) {
            log_line("edges%s", line);
            m = 0;
        }
    }
}

static void run_start(App* app) {
    if(!app->seq_loaded || seq_playing()) return;
    live_release_all(app);
    char ps[120];
    int n = 0;
    ps[0] = '\0';
    for(int i = 0; i < app->seq->n_params; i++) {
        n += snprintf(ps + n, sizeof(ps) - n, " %s=%d", app->seq->params[i].name, app->seq->params[i].value);
        if(n >= (int)sizeof(ps)) break;
    }
    ps[sizeof(ps) - 1] = '\0';
    log_line(
        "run %s start %s %ums, one cycle %lums, loop %s%s",
        app->seq_name,
        kind_name(pad_usb_kind()),
        pad_usb_interval(),
        (unsigned long)(seq_cycle_us(app->seq) / 1000),
        app->seq->has_loop ? (app->seq->loop_count ? "counted" : "forever") : "none",
        ps);
    seq_play(app->seq);
    app->was_playing = true;
    app->run_started = furi_get_tick();
}

static void run_stop(App* app) {
    if(seq_playing()) {
        seq_stop();
        log_run_end(app, "stopped");
    }
    app->was_playing = false;
}

/* ------------------------------------------------------------------ */
/* Sweep                                                              */
/* ------------------------------------------------------------------ */

static void sweep_begin(App* app) {
    app->sweeping = true;
    app->sweep_i = 0;
    app->sweep_phase = 0;
    memset(app->sweep_res, 0, sizeof(app->sweep_res));
    app->screen = ScrAuto;
    log_line("sweep start");
}

static uint8_t interval_index(uint8_t ms) {
    for(size_t i = 0; i < COUNT_OF(interval_choices); i++) {
        if(interval_choices[i] == ms) return (uint8_t)i;
    }
    return 0;
}

/* Manual test starts from the recommendation, or from the defaults */
static void apply_recommendation(App* app) {
    app->kind = app->rec_valid ? app->rec_kind : DEFAULT_KIND;
    app->interval_idx = interval_index(app->rec_valid ? app->rec_interval : DEFAULT_INTERVAL);
}

static void settings_load(App* app) {
    app->rec_valid = false;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* f = storage_file_alloc(storage);
    char buf[256];
    size_t n = 0;
    if(storage_file_open(f, SETTINGS_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        n = storage_file_read(f, buf, sizeof(buf) - 1);
    }
    storage_file_close(f);
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
    buf[n] = '\0';

    bool have_mode = false, have_int = false;
    char* line = buf;
    while(line && *line) {
        char* next = strchr(line, '\n');
        if(next) *next++ = '\0';
        char* cr = strchr(line, '\r');
        if(cr) *cr = '\0';
        if(strncmp(line, "mode=", 5) == 0) {
            if(strcmp(line + 5, "pro") == 0) {
                app->rec_kind = PadPro;
                have_mode = true;
            } else if(strcmp(line + 5, "hori") == 0) {
                app->rec_kind = PadHori;
                have_mode = true;
            }
        } else if(strncmp(line, "interval=", 9) == 0) {
            int v = atoi(line + 9);
            if(v >= 1 && v <= 8) {
                app->rec_interval = (uint8_t)v;
                have_int = true;
            }
        } else if(strncmp(line, "measured_us=", 12) == 0) {
            app->rec_mean_us = (uint32_t)atoi(line + 12);
        } else if(strncmp(line, "confirmed=", 10) == 0) {
            app->rec_confirmed = strcmp(line + 10, "yes") == 0;
        } else if(strncmp(line, "tested=", 7) == 0) {
            snprintf(app->rec_when, sizeof(app->rec_when), "%s", line + 7);
        }
        line = next;
    }
    app->rec_valid = have_mode && have_int;
}

static void settings_save(App* app) {
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    snprintf(
        app->rec_when,
        sizeof(app->rec_when),
        "%04u-%02u-%02u %02u:%02u",
        dt.year,
        dt.month,
        dt.day,
        dt.hour,
        dt.minute);
    char buf[200];
    snprintf(
        buf,
        sizeof(buf),
        "# Frame Perfect: recommended setting from the automatic test\n"
        "mode=%s\ninterval=%u\nmeasured_us=%lu\nconfirmed=%s\ntested=%s\n",
        app->rec_kind == PadHori ? "hori" : "pro",
        app->rec_interval,
        (unsigned long)app->rec_mean_us,
        app->rec_confirmed ? "yes" : "no",
        app->rec_when);
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, APP_DATA_PATH(""));
    File* f = storage_file_alloc(storage);
    if(storage_file_open(f, SETTINGS_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_write(f, buf, strlen(buf));
    }
    storage_file_close(f);
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
}

/* true if result i is a better recommendation than result j */
static bool better(const SweepResult* res, int i, int j) {
    const SweepResult* r = &res[i];
    const SweepResult* b = &res[j];
    bool faster = r->mean_us * 105 < b->mean_us * 100;
    bool slower = b->mean_us * 105 < r->mean_us * 100;
    if(faster) return true;
    if(slower) return false;
    bool r_steady = r->max_us <= 3 * r->mean_us;
    bool b_steady = b->max_us <= 3 * b->mean_us;
    if(r_steady != b_steady) return r_steady;
    if(sweep_cfgs[i].kind != sweep_cfgs[j].kind) return sweep_cfgs[i].kind == PadPro;
    return sweep_cfgs[i].interval > sweep_cfgs[j].interval;
}

/* Fills rank[] with the usable results, best first. Returns how many. */
static int rank_results(const SweepResult* res, int8_t* rank) {
    int n = 0;
    for(size_t i = 0; i < SWEEP_N; i++) {
        if(res[i].streamed && res[i].polls >= 10 && res[i].mean_us) rank[n++] = (int8_t)i;
    }
    for(int a = 1; a < n; a++) {
        int8_t v = rank[a];
        int b = a - 1;
        while(b >= 0 && better(res, v, rank[b])) {
            rank[b + 1] = rank[b];
            b--;
        }
        rank[b + 1] = v;
    }
    for(int i = n; i < (int)SWEEP_N; i++) rank[i] = -1;
    return n;
}

static void set_recommendation(App* app, int idx, bool confirmed) {
    app->rec_valid = true;
    app->auto_failed = false;
    app->rec_confirmed = confirmed;
    app->rec_kind = sweep_cfgs[idx].kind;
    app->rec_interval = sweep_cfgs[idx].interval;
    app->rec_mean_us = app->sweep_res[idx].mean_us;
    settings_save(app);
    log_line(
        "recommended %s int=%u (console reads every %luus, %s), saved to settings.txt",
        kind_name(app->rec_kind),
        app->rec_interval,
        (unsigned long)app->rec_mean_us,
        confirmed ? "input confirmed on the Switch" : "input not checked");
    apply_recommendation(app);
}

static void flow_after_sweep(App* app);

static void sweep_finish(App* app) {
    app->sweeping = false;
    app->sweep_phase = 3;
    pad_usb_stop();
    log_line("sweep done");
    for(size_t i = 0; i < SWEEP_N; i++) {
        SweepResult* r = &app->sweep_res[i];
        log_line(
            "sweep %s int=%u: %s to_stream=%lums polls=%lu mean=%luus min=%luus max=%luus",
            kind_name(sweep_cfgs[i].kind),
            sweep_cfgs[i].interval,
            r->streamed ? "streamed" : "NO STREAM",
            (unsigned long)r->to_stream_ms,
            (unsigned long)r->polls,
            (unsigned long)r->mean_us,
            (unsigned long)r->min_us,
            (unsigned long)r->max_us);
    }
    int n = rank_results(app->sweep_res, app->rank);
    if(app->headless_sweep) {
        if(n > 0)
            set_recommendation(app, app->rank[0], false);
        else
            log_line("recommended: none (nothing streamed); settings.txt unchanged");
        if(app->quit_after_sweep) app->running = false;
        return;
    }
    flow_after_sweep(app);
}

static void sweep_tick(App* app) {
    if(!app->sweeping) return;
    uint32_t now = furi_get_tick();
    const SweepCfg* c = &sweep_cfgs[app->sweep_i];
    SweepResult* r = &app->sweep_res[app->sweep_i];
    switch(app->sweep_phase) {
    case 0:
        connect_pad(app, c->kind, c->interval);
        app->sweep_t0 = now;
        app->sweep_phase = 1;
        break;
    case 1:
        if(pad_usb_streaming()) {
            r->streamed = true;
            r->to_stream_ms = now - app->sweep_t0;
            PollStats junk;
            pad_usb_take_stats(&junk);
            memset(&app->sweep_acc, 0, sizeof(app->sweep_acc));
            app->sweep_acc.min_us = UINT32_MAX;
            app->sweep_secs = 0;
            app->sweep_t0 = now;
            app->sweep_phase = 2;
        } else if(now - app->sweep_t0 > SWEEP_WAIT_MS) {
            if(c->kind == PadPro) log_handshake(app);
            log_line(
                "sweep %s int=%u: no stream after %us (configured=%d rx=%lu)",
                kind_name(c->kind),
                c->interval,
                SWEEP_WAIT_MS / 1000,
                pad_usb_configured(),
                (unsigned long)app->rx_count);
            app->sweep_phase = 4; /* next */
        }
        break;
    case 2:
        if(now - app->sweep_t0 >= 1000u * (app->sweep_secs + 1)) {
            PollStats s;
            pad_usb_take_stats(&s);
            PollStats* a = &app->sweep_acc;
            a->polls += s.polls;
            a->sum_us += s.sum_us;
            if(s.polls && s.min_us < a->min_us) a->min_us = s.min_us;
            if(s.max_us > a->max_us) a->max_us = s.max_us;
            for(int i = 0; i < 17; i++) a->hist[i] += s.hist[i];
            a->write_fail += s.write_fail;
            app->sweep_secs++;
            if(app->sweep_secs >= SWEEP_MEASURE_S) {
                r->polls = a->polls;
                r->mean_us = a->polls ? a->sum_us / a->polls : 0;
                r->min_us = a->polls ? a->min_us : 0;
                r->max_us = a->max_us;
                log_stats("sweep-measure", a);
                if(c->kind == PadPro) log_handshake(app);
                app->sweep_phase = 4;
            }
        }
        break;
    case 4:
        app->sweep_i++;
        if(app->sweep_i >= SWEEP_N) {
            sweep_finish(app);
        } else {
            app->sweep_phase = 0;
        }
        break;
    default:
        break;
    }
    app_redraw(app);
}

static void go_screen(App* app, Screen s);

/* ------------------------------------------------------------------ */
/* Guided steps (what to do on the Switch, and yes/no questions)      */
/* ------------------------------------------------------------------ */

enum {
    FlowNone,
    FlowAutoPrep, /* tell the user how to set up the Switch */
    FlowAutoNothing, /* nothing streamed */
    FlowCheckConnect, /* connecting the next candidate */
    FlowCheckMove, /* moving the Home menu cursor */
    FlowCheckAsk, /* did it move? */
    FlowCheckNone, /* no candidate moved the cursor */
    FlowGameReady,
    FlowGameMove,
    FlowGameAskMoved,
    FlowGameAskOwn,
    FlowGameResult,
    FlowBleTest,
    FlowRemote,
};

#define CHECK_MAX_TRIES 3
#define KEYS_YES_NO "OK: yes        Back: no"

static void prompt(
    App* app,
    uint8_t flow,
    const char* title,
    const char* l0,
    const char* l1,
    const char* l2,
    const char* l3,
    const char* keys) {
    app->flow = flow;
    app->p_title = title;
    app->p_lines[0] = l0;
    app->p_lines[1] = l1;
    app->p_lines[2] = l2;
    app->p_lines[3] = l3;
    app->p_keys = keys;
    app->p_wait = keys == NULL;
    app->flow_t0 = furi_get_tick();
    log_line(
        "screen [%s] %s / %s / %s / %s [%s]",
        title,
        l0 ? l0 : "",
        l1 ? l1 : "",
        l2 ? l2 : "",
        l3 ? l3 : "",
        keys ? keys : "working");
    app->screen = ScrPrompt;
    app_redraw(app);
}

/* stick right, right, left, left: moves a menu cursor and puts it back */
static void build_probe(Sequence* s) {
    static const struct {
        uint16_t frames;
        int8_t lx;
    } steps[] = {{30, 0}, {9, 1}, {25, 0}, {9, 1}, {40, 0}, {9, -1}, {25, 0}, {9, -1}, {20, 0}};
    memset(s, 0, sizeof(*s));
    s->fps = 60;
    for(size_t i = 0; i < COUNT_OF(steps); i++) {
        s->src[i].kind = DurFrames;
        s->src[i].value = steps[i].frames;
        s->src[i].st.lx = steps[i].lx;
    }
    s->n_src = COUNT_OF(steps);
    s->loop_at = s->n_src;
    seq_compile(s);
}

static void auto_start(App* app) {
    prompt(
        app,
        FlowAutoPrep,
        "Automatic test",
        "On the Switch 2, turn on",
        "Settings > Controllers >",
        "Pro Controller Wired Comm.",
        "Go to Home menu, plug me in",
        "OK: start   Back: cancel");
}

static void check_start(App* app) {
    int idx = app->rank[app->rank_pos];
    const SweepCfg* c = &sweep_cfgs[idx];
    snprintf(app->p_buf, sizeof(app->p_buf), "Trying %s %u ms", kind_name(c->kind), c->interval);
    connect_pad(app, c->kind, c->interval);
    prompt(
        app,
        FlowCheckConnect,
        "Input check",
        app->p_buf,
        "Watch the Home menu: the",
        "cursor should move right",
        "twice, then back.",
        NULL);
}

static void check_next(App* app, const char* why) {
    int idx = app->rank[app->rank_pos];
    log_line("input check %s int=%u: %s", kind_name(sweep_cfgs[idx].kind), sweep_cfgs[idx].interval, why);
    app->rank_pos++;
    if(app->rank_pos < CHECK_MAX_TRIES && app->rank_pos < SWEEP_N && app->rank[app->rank_pos] >= 0) {
        check_start(app);
        return;
    }
    pad_usb_stop();
    app->auto_failed = true;
    log_line("recommended: none (no setting moved the cursor); settings.txt unchanged");
    prompt(
        app,
        FlowCheckNone,
        "No input got through",
        "The Switch read me but",
        "ignored the buttons.",
        "Manual test will start",
        "from the defaults.",
        "OK: home");
}

static void flow_after_sweep(App* app) {
    if(app->rank[0] < 0) {
        app->auto_failed = true;
        log_line("recommended: none (nothing streamed); settings.txt unchanged");
        prompt(
            app,
            FlowAutoNothing,
            "Nothing connected",
            "The Switch never read me.",
            "Check the cable or port,",
            "and Pro Controller Wired",
            "Communication.",
            "OK: retry   Back: home");
        return;
    }
    app->rank_pos = 0;
    check_start(app);
}

static void game_start(App* app) {
    prompt(
        app,
        FlowGameReady,
        "Game check",
        "With your own controller:",
        "start Ocarina of Time and",
        "open the pause menu.",
        "Then leave it alone.",
        "OK: ready   Back: cancel");
}

static void flow_tick(App* app) {
    uint32_t now = furi_get_tick();
    switch(app->flow) {
    case FlowCheckConnect:
        if(pad_usb_streaming()) {
            if(now - app->flow_t0 > 1500) {
                live_release_all(app);
                seq_play(app->probe);
                app->flow = FlowCheckMove;
            }
        } else if(now - app->flow_t0 > 10000) {
            check_next(app, "did not stream");
        }
        break;
    case FlowCheckMove:
        if(!seq_playing()) {
            prompt(
                app,
                FlowCheckAsk,
                "Input check",
                "Did the Home menu cursor",
                "move right twice and",
                "then back?",
                app->p_buf,
                KEYS_YES_NO);
        }
        break;
    case FlowGameMove:
        if(!seq_playing()) {
            prompt(
                app,
                FlowGameAskMoved,
                "Game check",
                "Did the pause-menu cursor",
                "move right twice and",
                "then back?",
                "",
                KEYS_YES_NO);
        }
        break;
    default:
        break;
    }
}

static void flow_input(App* app, InputEvent* e) {
    if(e->type != InputTypeShort) return;
    bool ok = e->key == InputKeyOk;
    bool back = e->key == InputKeyBack;
    if(!ok && !back) return;
    {
        bool question = app->p_keys && strcmp(app->p_keys, KEYS_YES_NO) == 0;
        log_line(
            "answer [%s] %s",
            app->p_title ? app->p_title : "",
            question ? (ok ? "YES" : "NO") : (ok ? "OK" : "Back"));
    }
    switch(app->flow) {
    case FlowAutoPrep:
        if(ok) {
            app->flow = FlowNone;
            sweep_begin(app);
        } else {
            app->flow = FlowNone;
            go_screen(app, ScrHome);
        }
        break;
    case FlowAutoNothing:
        app->flow = FlowNone;
        if(ok)
            sweep_begin(app);
        else
            go_screen(app, ScrHome);
        break;
    case FlowCheckConnect:
    case FlowCheckMove:
        if(back) {
            seq_stop();
            pad_usb_stop();
            log_line("input check aborted");
            app->flow = FlowNone;
            go_screen(app, ScrHome);
        }
        break;
    case FlowCheckAsk:
        if(ok) {
            log_line(
                "input check %s int=%u: cursor moved",
                kind_name(sweep_cfgs[app->rank[app->rank_pos]].kind),
                sweep_cfgs[app->rank[app->rank_pos]].interval);
            set_recommendation(app, app->rank[app->rank_pos], true);
            pad_usb_stop();
            app->flow = FlowNone;
            app->screen = ScrAuto; /* results */
            app_redraw(app);
        } else {
            check_next(app, "cursor did not move");
        }
        break;
    case FlowCheckNone:
        app->flow = FlowNone;
        apply_recommendation(app);
        go_screen(app, ScrHome);
        break;
    case FlowGameReady:
        if(back) {
            app->flow = FlowNone;
            go_screen(app, ScrPad);
        } else if(!pad_usb_streaming()) {
            prompt(
                app,
                FlowGameResult,
                "Not connected",
                "The Switch isn't reading",
                "me right now. Disconnect",
                "and try another setting.",
                "",
                "OK: back");
        } else {
            live_release_all(app);
            seq_play(app->probe);
            prompt(app, FlowGameMove, "Game check", "Moving the cursor:", "right twice, then back.", "", "", NULL);
        }
        break;
    case FlowGameMove:
        if(back) {
            seq_stop();
            app->flow = FlowNone;
            go_screen(app, ScrPad);
        }
        break;
    case FlowGameAskMoved:
        app->game_flipper_ok = ok;
        prompt(
            app,
            FlowGameAskOwn,
            "Game check",
            "Now press something on",
            "your own controller.",
            "Does it still work?",
            "",
            KEYS_YES_NO);
        break;
    case FlowGameAskOwn: {
        bool own = ok;
        log_line(
            "game check %s int=%u: flipper input %s, own controller %s",
            kind_name(pad_usb_kind()),
            pad_usb_interval(),
            app->game_flipper_ok ? "yes" : "NO",
            own ? "yes" : "NO");
        prompt(
            app,
            FlowGameResult,
            "Game check saved",
            app->game_flipper_ok ? "Flipper input: works" : "Flipper input: ignored",
            own ? "Your controller: works" : "Your controller: stopped",
            app->game_flipper_ok ? "Ready for sequences." : "Try HORIPAD or another",
            app->game_flipper_ok ? "" : "port in Manual test.",
            "OK: back");
        break;
    }
    case FlowGameResult:
        app->flow = FlowNone;
        go_screen(app, ScrPad);
        break;
    case FlowBleTest:
    case FlowRemote:
        if(back) {
            app->ble_quit_ms = 1; /* main loop stops it */
        }
        break;
    default:
        app->flow = FlowNone;
        go_screen(app, ScrHome);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Drawing                                                            */
/* ------------------------------------------------------------------ */

static void draw_title(Canvas* c, const char* t) {
    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 2, 10, t);
    canvas_set_font(c, FontSecondary);
}

static void draw_menu(Canvas* c, const char* const* items, int n, int sel, int y0) {
    for(int i = 0; i < n; i++) {
        int y = y0 + i * 10;
        if(i == sel) {
            canvas_draw_box(c, 0, y - 8, 128, 10);
            canvas_set_color(c, ColorWhite);
        }
        canvas_draw_str(c, 4, y, items[i]);
        canvas_set_color(c, ColorBlack);
    }
}

static void stats_line(App* app, char* buf, size_t len) {
    if(!app->have_last || !app->last.polls) {
        snprintf(buf, len, "polls: -");
        return;
    }
    const PollStats* s = &app->last;
    snprintf(
        buf,
        len,
        "%luHz avg %lu min %lu max %lu",
        (unsigned long)s->polls,
        (unsigned long)(s->sum_us / s->polls),
        (unsigned long)s->min_us,
        (unsigned long)s->max_us);
}

static void draw_cb(Canvas* c, void* ctx) {
    App* app = ctx;
    char b[48];
    canvas_clear(c);
    switch(app->screen) {
    case ScrHome: {
        draw_title(c, "Frame Perfect");
        const char* items[] = {"Automatic test", "Manual test"};
        draw_menu(c, items, 2, app->sel, 26);
        if(app->rec_valid)
            snprintf(b, sizeof(b), "Saved: %s %u ms", kind_name(app->rec_kind), app->rec_interval);
        else
            snprintf(b, sizeof(b), "No saved setting yet");
        canvas_draw_str(c, 2, 52, b);
        canvas_draw_str(c, 2, 62, "Back: quit");
        break;
    }
    case ScrManual: {
        draw_title(c, "Manual test");
        if(app->rec_valid)
            snprintf(b, sizeof(b), "Auto test: %s %ums", kind_name(app->rec_kind), app->rec_interval);
        else if(app->auto_failed)
            snprintf(b, sizeof(b), "Auto test failed: defaults");
        else
            snprintf(b, sizeof(b), "No auto test yet: defaults");
        canvas_draw_str(c, 2, 20, b);
        char i0[32], i1[32];
        snprintf(i0, sizeof(i0), "Mode: %s", app->kind == PadHori ? "HORIPAD" : "Pro Controller");
        snprintf(i1, sizeof(i1), "Poll interval: %u ms", interval_choices[app->interval_idx]);
        const char* items[] = {i0, i1, "Connect", app->rec_valid ? "Reset to recommended" : "Reset to defaults"};
        draw_menu(c, items, 4, app->sel, 31);
        break;
    }
    case ScrPad: {
        snprintf(b, sizeof(b), "%s %ums %s", kind_name(pad_usb_kind()), pad_usb_interval(), usb_state_text());
        draw_title(c, b);
        stats_line(app, b, sizeof(b));
        canvas_draw_str(c, 2, 21, b);
        const char* items[] = {"Game check (2 questions)", "Live control", "Sequences", "Disconnect"};
        draw_menu(c, items, 4, app->sel, 33);
        break;
    }
    case ScrLive: {
        snprintf(b, sizeof(b), "Live: %s", layout_names[app->layout]);
        draw_title(c, b);
        canvas_draw_str(c, 2, 20, usb_state_text());
        stats_line(app, b, sizeof(b));
        canvas_draw_str(c, 2, 30, b);
        canvas_draw_str(c, 2, 42, layout_hints[app->layout]);
        canvas_draw_str(c, 2, 52, "Hold OK: next layout");
        canvas_draw_str(c, 2, 62, "Hold Back: leave");
        break;
    }
    case ScrPicker: {
        draw_title(c, "Sequences");
        if(app->n_names == 0) {
            canvas_draw_str(c, 2, 26, "No .txt files in");
            canvas_draw_str(c, 2, 36, "apps_data/frame_perfect/");
            canvas_draw_str(c, 2, 46, "sequences");
            break;
        }
        int first = app->sel > 4 ? app->sel - 4 : 0;
        const char* items[5];
        int n = 0;
        for(int i = first; i < app->n_names && n < 5; i++) items[n++] = app->names[i];
        draw_menu(c, items, n, app->sel - first, 22);
        break;
    }
    case ScrRun: {
        draw_title(c, app->seq_name);
        if(!app->seq_loaded) {
            snprintf(b, sizeof(b), "Line %u: %s", app->seq_err_line, app->seq_err);
            canvas_draw_str(c, 2, 24, b);
            canvas_draw_str(c, 2, 62, "Back: list");
            break;
        }
        if(seq_playing()) {
            snprintf(
                b,
                sizeof(b),
                "PLAYING line %u cycle %lu",
                seq_current_line(),
                (unsigned long)seq_cycles_done() + 1);
        } else {
            snprintf(b, sizeof(b), "%s  OK: run", pad_usb_streaming() ? "ready" : usb_state_text());
        }
        canvas_draw_str(c, 2, 20, b);
        int np = app->seq->n_params;
        int first = app->sel > 3 ? app->sel - 3 : 0;
        for(int i = first; i < np && i < first + 4; i++) {
            int y = 31 + (i - first) * 10;
            snprintf(b, sizeof(b), "%s = %d", app->seq->params[i].name, app->seq->params[i].value);
            if(i == app->sel) {
                canvas_draw_box(c, 0, y - 8, 128, 10);
                canvas_set_color(c, ColorWhite);
            }
            canvas_draw_str(c, 4, y, b);
            canvas_set_color(c, ColorBlack);
        }
        if(np == 0) canvas_draw_str(c, 2, 31, "(no params)");
        break;
    }
    case ScrPrompt: {
        draw_title(c, app->p_title ? app->p_title : "");
        for(int i = 0; i < 4; i++) {
            if(app->p_lines[i]) canvas_draw_str(c, 2, 21 + i * 10, app->p_lines[i]);
        }
        if(app->p_wait)
            canvas_draw_str(c, 2, 63, "Working...   Back: stop");
        else if(app->p_keys)
            canvas_draw_str(c, 2, 63, app->p_keys);
        break;
    }
    case ScrAuto: {
        if(app->sweep_phase == 3) {
            draw_title(c, "Automatic test done");
            if(app->auto_failed) {
                canvas_draw_str(c, 2, 20, "Nothing worked: defaults");
            } else {
                snprintf(
                    b,
                    sizeof(b),
                    "Best: %s %u ms %luHz %s",
                    kind_name(app->rec_kind),
                    app->rec_interval,
                    (unsigned long)(app->rec_mean_us ? 1000000 / app->rec_mean_us : 0),
                    app->rec_confirmed ? "ok" : "");
                canvas_draw_str(c, 2, 20, b);
            }
            for(size_t i = 0; i < SWEEP_N; i++) {
                SweepResult* r = &app->sweep_res[i];
                int x = i < 4 ? 0 : 64;
                int y = 29 + (int)(i % 4) * 8;
                char k = sweep_cfgs[i].kind == PadPro ? 'P' : 'H';
                if(r->streamed && r->mean_us)
                    snprintf(b, sizeof(b), "%c%u: %luHz", k, sweep_cfgs[i].interval, (unsigned long)(1000000 / r->mean_us));
                else
                    snprintf(b, sizeof(b), "%c%u: none", k, sweep_cfgs[i].interval);
                canvas_draw_str(c, x + 2, y, b);
            }
            canvas_draw_str(c, 2, 63, "OK: manual test  Back: home");
        } else {
            draw_title(c, "Automatic test");
            const SweepCfg* cf = &sweep_cfgs[app->sweep_i];
            snprintf(b, sizeof(b), "Step %u/%u: %s %u ms", app->sweep_i + 1, (unsigned)SWEEP_N, kind_name(cf->kind), cf->interval);
            canvas_draw_str(c, 2, 22, b);
            canvas_draw_str(c, 2, 32, usb_state_text());
            if(app->sweep_phase == 2) {
                snprintf(b, sizeof(b), "measuring %u/%u s", app->sweep_secs, SWEEP_MEASURE_S);
                canvas_draw_str(c, 2, 42, b);
            }
            canvas_draw_str(c, 2, 63, "About 2 min.  Back: abort");
        }
        break;
    }
    }
}

/* ------------------------------------------------------------------ */
/* Input                                                              */
/* ------------------------------------------------------------------ */

static void input_cb(InputEvent* ie, void* ctx) {
    App* app = ctx;
    AppEvent ev = {.type = EvInput, .input = *ie};
    furi_message_queue_put(app->queue, &ev, FuriWaitForever);
}

static const char* const screen_names[] = {
    "home", "manual", "connected", "live", "sequences", "run", "auto results", "prompt"};

static void go_screen(App* app, Screen s) {
    if(app->screen != s) log_line("screen %s", screen_names[s]);
    app->screen = s;
    app->sel = 0;
    app_redraw(app);
}

static void home_input(App* app, InputEvent* e) {
    if(e->type != InputTypeShort) return;
    switch(e->key) {
    case InputKeyUp:
    case InputKeyDown:
        app->sel = app->sel ? 0 : 1;
        break;
    case InputKeyOk:
        if(app->sel == 0) {
            auto_start(app);
        } else {
            apply_recommendation(app);
            go_screen(app, ScrManual);
        }
        break;
    case InputKeyBack:
        app->running = false;
        break;
    default:
        break;
    }
}

static void manual_input(App* app, InputEvent* e) {
    if(e->type != InputTypeShort && e->type != InputTypeRepeat) return;
    int n = (int)COUNT_OF(interval_choices);
    switch(e->key) {
    case InputKeyUp:
        app->sel = (app->sel + 3) % 4;
        break;
    case InputKeyDown:
        app->sel = (app->sel + 1) % 4;
        break;
    case InputKeyLeft:
    case InputKeyRight:
        /* the interval list runs 8 -> 1 ms, so Right = faster */
        if(app->sel == 0) app->kind = app->kind == PadPro ? PadHori : PadPro;
        if(app->sel == 1) {
            int d = e->key == InputKeyRight ? 1 : -1;
            app->interval_idx = (uint8_t)((app->interval_idx + n + d) % n);
        }
        break;
    case InputKeyOk:
        if(e->type != InputTypeShort) break;
        if(app->sel == 0) app->kind = app->kind == PadPro ? PadHori : PadPro;
        if(app->sel == 1) app->interval_idx = (uint8_t)((app->interval_idx + 1) % n);
        if(app->sel == 2) {
            if(connect_pad(app, app->kind, interval_choices[app->interval_idx]))
                go_screen(app, ScrPad);
        }
        if(app->sel == 3) apply_recommendation(app);
        break;
    case InputKeyBack:
        if(e->type == InputTypeShort) go_screen(app, ScrHome);
        break;
    default:
        break;
    }
}

static void pad_input(App* app, InputEvent* e) {
    if(e->type != InputTypeShort) return;
    switch(e->key) {
    case InputKeyUp:
        app->sel = (app->sel + 3) % 4;
        break;
    case InputKeyDown:
        app->sel = (app->sel + 1) % 4;
        break;
    case InputKeyOk:
        if(app->sel == 0) {
            game_start(app);
        } else if(app->sel == 1) {
            live_release_all(app);
            go_screen(app, ScrLive);
        } else if(app->sel == 2) {
            picker_scan(app);
            go_screen(app, ScrPicker);
        } else {
            seq_stop();
            pad_usb_stop();
            log_line("disconnect");
            go_screen(app, ScrManual);
        }
        break;
    case InputKeyBack:
        seq_stop();
        pad_usb_stop();
        log_line("disconnect");
        go_screen(app, ScrManual);
        break;
    default:
        break;
    }
}

static void live_input(App* app, InputEvent* e) {
    InputKey k = e->key;
    if(k == InputKeyOk || k == InputKeyBack) {
        /* tap-only, so the long press stays free for the menu actions */
        if(e->type == InputTypeShort) {
            app->key_down[k] = true;
            app->tap_until[k] = furi_get_tick() + TAP_MS;
            live_update(app);
        } else if(e->type == InputTypeLong) {
            if(k == InputKeyOk) {
                app->layout = (Layout)((app->layout + 1) % LayCount);
                live_release_all(app);
            } else {
                live_release_all(app);
                go_screen(app, ScrPad);
            }
        }
    } else {
        if(e->type == InputTypePress) {
            app->key_down[k] = true;
            live_update(app);
        } else if(e->type == InputTypeRelease) {
            app->key_down[k] = false;
            live_update(app);
        }
    }
    app_redraw(app);
}

static void picker_input(App* app, InputEvent* e) {
    if(e->type != InputTypeShort && e->type != InputTypeRepeat) return;
    if(e->key == InputKeyBack) {
        go_screen(app, ScrPad);
        return;
    }
    if(app->n_names == 0) return;
    if(e->key == InputKeyUp) app->sel = (app->sel + app->n_names - 1) % app->n_names;
    if(e->key == InputKeyDown) app->sel = (app->sel + 1) % app->n_names;
    if(e->key == InputKeyOk) {
        seq_open(app, app->names[app->sel]);
        go_screen(app, ScrRun);
    }
}

static void run_input(App* app, InputEvent* e) {
    if(e->type != InputTypeShort && e->type != InputTypeRepeat) return;
    int np = app->seq_loaded ? app->seq->n_params : 0;
    switch(e->key) {
    case InputKeyBack:
        run_stop(app);
        picker_scan(app);
        go_screen(app, ScrPicker);
        return;
    case InputKeyOk:
        if(e->type != InputTypeShort) break;
        if(seq_playing())
            run_stop(app);
        else
            run_start(app);
        break;
    case InputKeyUp:
        if(np) app->sel = (uint8_t)((app->sel + np - 1) % np);
        break;
    case InputKeyDown:
        if(np) app->sel = (uint8_t)((app->sel + 1) % np);
        break;
    case InputKeyLeft:
    case InputKeyRight:
        if(np && !seq_playing()) {
            SeqParam* p = &app->seq->params[app->sel];
            int v = p->value + (e->key == InputKeyRight ? 1 : -1);
            if(v < p->min) v = p->min;
            if(v > p->max) v = p->max;
            p->value = (int16_t)v;
            seq_compile(app->seq);
        }
        break;
    default:
        break;
    }
    app_redraw(app);
}

static void sweep_input(App* app, InputEvent* e) {
    if(e->type != InputTypeShort) return;
    if(e->key == InputKeyOk && !app->sweeping && app->sweep_phase == 3) {
        apply_recommendation(app);
        go_screen(app, ScrManual);
        return;
    }
    if(e->key != InputKeyBack) return;
    if(app->sweeping) {
        log_line("sweep aborted at step %u", app->sweep_i + 1);
        app->sweeping = false;
        pad_usb_stop();
    }
    go_screen(app, ScrHome);
}

static void handle_input(App* app, InputEvent* e) {
    switch(app->screen) {
    case ScrHome:
        home_input(app, e);
        break;
    case ScrManual:
        manual_input(app, e);
        break;
    case ScrPrompt:
        flow_input(app, e);
        break;
    case ScrPad:
        pad_input(app, e);
        break;
    case ScrLive:
        live_input(app, e);
        break;
    case ScrPicker:
        picker_input(app, e);
        break;
    case ScrRun:
        run_input(app, e);
        break;
    case ScrAuto:
        sweep_input(app, e);
        break;
    }
    app_redraw(app);
}

/* ------------------------------------------------------------------ */
/* Headless args                                                      */
/* ------------------------------------------------------------------ */

static const char* arg_val(const char* args, const char* key, char* out, size_t len) {
    size_t kl = strlen(key);
    const char* p = args;
    while(p && *p) {
        while(*p == ' ') p++;
        if(strncmp(p, key, kl) == 0 && p[kl] == '=') {
            p += kl + 1;
            size_t i = 0;
            while(*p && *p != ' ' && i + 1 < len) out[i++] = *p++;
            out[i] = '\0';
            return out;
        }
        while(*p && *p != ' ') p++;
    }
    return NULL;
}

static void parse_args(App* app, const char* args) {
    if(!args || !*args) return;
    char v[PICKER_NAME];
    if(strncmp(args, "blesync", 7) == 0 || strncmp(args, "blehid", 6) == 0) {
        /* Bluetooth sync test: own profile + fp_clock CLI reference.
         * blehid adds an HID service (mouse appearance) for a faster link. */
        bool hid = strncmp(args, "blehid", 6) == 0;
        app->ble_quit_ms = arg_val(args, "quit", v, sizeof(v)) ? (uint32_t)atoi(v) * 1000u : 0;
        fp_clock_cli_register();
        app->ble_active = fp_ble_start(hid);
        uint32_t ka = arg_val(args, "ka", v, sizeof(v)) ? (uint32_t)atoi(v) : 0;
        fp_ble_set_keepalive_ms(ka);
        /* chosen mode (2026-11-09): HID link + peripheral latency off, no
         * keepalive, PHY left to the Mac. nolat=0 turns latency-off off. */
        bool nolat = arg_val(args, "nolat", v, sizeof(v)) ? atoi(v) != 0 : hid;
        fp_ble_set_no_latency(nolat);
        bool noreply = arg_val(args, "noreply", v, sizeof(v)) && atoi(v) != 0;
        fp_ble_set_no_reply(noreply);
        uint8_t phy = arg_val(args, "phy", v, sizeof(v)) ? (uint8_t)atoi(v) : 0;
        fp_ble_set_phy(phy);
        if(phy) log_line("bluetooth test: asking for %uM PHY", phy);
        if(noreply) log_line("bluetooth test: pings not answered");
        log_line("bluetooth mode: %s, keepalive %lu ms, latency off %s", hid ? "HID + data (mouse appearance)" : "data only", (unsigned long)ka, nolat ? "yes" : "no");
        app->ble_t0 = furi_get_tick();
        log_line("blesync start: profile %s, quit after %lus", app->ble_active ? "ok" : "FAILED", (unsigned long)(app->ble_quit_ms / 1000));
        prompt(
            app,
            FlowBleTest,
            "Bluetooth sync test",
            "Own 15 ms profile on.",
            "Mac: tools/ble-app/run.sh",
            "'tools/ble_sync.py'",
            "",
            NULL);
        return;
    }
    if(strncmp(args, "remote", 6) == 0) {
        /* the Mac drives: Bluetooth HID link with latency off, USB stays the
         * CLI (for clock calibration) until the Mac sends 'U' */
        app->remote = true;
        app->ble_quit_ms = arg_val(args, "quit", v, sizeof(v)) ? (uint32_t)atoi(v) * 1000u : 0;
        fp_clock_cli_register();
        app->ble_active = fp_ble_start(true);
        fp_ble_set_no_latency(true);
        app->ble_t0 = furi_get_tick();
        log_line("remote start: bluetooth %s", app->ble_active ? "ok" : "FAILED");
        prompt(app, FlowRemote, "Remote mode", "Mac: tools/fp_remote.py", "", "", "", NULL);
        app->p_keys = "Back: quit";
        return;
    }
    if(strncmp(args, "autotest", 8) == 0) {
        /* headless: no questions, the recommendation is saved unconfirmed */
        app->headless_sweep = true;
        app->quit_after_sweep = strstr(args, "quit") != NULL;
        if(arg_val(args, "answer", v, sizeof(v))) {
            /* run the guided flow and answer its questions automatically */
            app->headless_sweep = false;
            app->auto_answer = v[0];
            log_line("autotest with answers=%c", app->auto_answer);
            auto_start(app);
            return;
        }
        sweep_begin(app);
        return;
    }
    if(strncmp(args, "auto", 4) != 0) return;
    app->autotest = true;
    PadKind kind = PadPro;
    uint8_t interval = 8;
    if(arg_val(args, "mode", v, sizeof(v)) && strcmp(v, "hori") == 0) kind = PadHori;
    if(arg_val(args, "int", v, sizeof(v))) interval = (uint8_t)atoi(v);
    app->auto_quit_ms = arg_val(args, "quit", v, sizeof(v)) ? (uint32_t)atoi(v) * 1000u : 30000u;
    app->auto_runs = arg_val(args, "runs", v, sizeof(v)) ? (uint8_t)atoi(v) : 1;
    app->auto_seq_ok = false;
    if(arg_val(args, "seq", v, sizeof(v))) app->auto_seq_ok = seq_open(app, v);
    log_line("auto start args=[%s]", args);
    app->auto_t0 = furi_get_tick();
    connect_pad(app, kind, interval);
    app->screen = app->auto_seq_ok ? ScrRun : ScrPad;
}

static void auto_tick(App* app) {
    if(!app->autotest) return;
    uint32_t now = furi_get_tick();
    if(app->auto_seq_ok && pad_usb_streaming() && !seq_playing() &&
       app->auto_runs_done < app->auto_runs && now - app->stats_at < 900) {
        /* start right after a stats window so runs don't straddle setup */
        furi_delay_ms(200);
        run_start(app);
        app->auto_runs_done++;
    }
    if(now - app->auto_t0 > app->auto_quit_ms) {
        log_line("auto quit");
        app->running = false;
    }
}


/* ------------------------------------------------------------------ */
/* Remote mode                                                        */
/* ------------------------------------------------------------------ */
/* Protocol (little-endian). Mac -> Flipper:
 *   'F' u32 period_ns u64 anchor_us     frame grid: a frame start on our clock
 *   'L' name                            load sequences/<name>
 *   'P' u8 idx i16 value                set a sequence parameter
 *   'G' u16 id u64 after_us i32 offset [u16 safe_us]  play from the first
 *                                       frame start at or after `after`, shifted
 *                                       by offset us; with safe_us: frame mode
 *                                       (see seq_set_frame_mode), offset ignored
 *   'X'                                 stop playback
 *   'U' u8 kind u8 interval             USB pad on (kind 0 Pro, 1 HORI,
 *                                       0xFF = saved setting); 'C' = USB off
 *   'H' u32 buttons u16 ms [i8 lx ly rx ry]  hold buttons (pad_usb.h BTN_*) and
 *                                       stick directions (-1/0/1) for ms, then
 *                                       release; ms 0 = until the next 'H'
 *   'Y'                                 list sequence files ('y' i name, then
 *                                       'y' 0xFF count); 'D' name deletes one
 *   'O' u16 bytes                       tail of the app log ('o' chunks, then a
 *                                       bare 'o')
 *   'Z'                                 quit the app (stock Bluetooth returns,
 *                                       so a new build can be uploaded)
 *   'W' u8 flags ...                    write a sequence file in pieces: flags 1 =
 *                                       start (payload = file name), 0 = data,
 *                                       2 = end (reply 'w' u8 ok u32 bytes)
 *   'Q'                                 status; 'E' edge log of the last run
 * Flipper -> Mac: 'l' 'p' 'g' 'd' 'u' 'q' 'e' replies, '!' text.
 */

static void r_say(const char* fmt, ...) {
    char b[FP_MSG_MAX + 1];
    b[0] = '!';
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b + 1, sizeof(b) - 1, fmt, ap);
    va_end(ap);
    fp_ble_send(b, (uint8_t)strlen(b));
}

static uint8_t usb_state_code(void) {
    if(!pad_usb_active()) return 0;
    if(!pad_usb_configured()) return 1;
    if(pad_usb_suspended()) return 2;
    if(!pad_usb_streaming()) return 3;
    return 4;
}

static void remote_screen(App* app) {
    snprintf(app->r_line[0], sizeof(app->r_line[0]), "Mac %s, grid %s",
             fp_ble_connected() ? "connected" : "not connected", app->have_grid ? "ok" : "none");
    snprintf(app->r_line[1], sizeof(app->r_line[1]), "USB: %s", usb_state_text());
    snprintf(app->r_line[2], sizeof(app->r_line[2]), "%.24s%s",
             app->seq_loaded ? app->seq_name : "no sequence",
             seq_playing() ? " (playing)" : "");
    app->p_lines[1] = app->r_line[0];
    app->p_lines[2] = app->r_line[1];
    app->p_lines[3] = app->r_line[2];
    app_redraw(app);
}

static void send_param(App* app, uint8_t i) {
    const SeqParam* p = &app->seq->params[i];
    uint8_t b[FP_MSG_MAX];
    b[0] = 'p';
    b[1] = i;
    memcpy(&b[2], &p->value, 2);
    memcpy(&b[4], &p->min, 2);
    memcpy(&b[6], &p->max, 2);
    size_t n = strlen(p->name);
    if(n > FP_MSG_MAX - 8) n = FP_MSG_MAX - 8;
    memcpy(&b[8], p->name, n);
    fp_ble_send(b, (uint8_t)(8 + n));
}

static void remote_cmd(App* app, const FpMsg* m) {
    const uint8_t* d = m->d;
    switch(d[0]) {
    case 'F':
        if(m->len >= 13) {
            memcpy(&app->grid_period_ns, &d[1], 4);
            memcpy(&app->grid_anchor_us, &d[5], 8);
            if(!app->have_grid)
                log_line("remote: first frame grid, period %lu ns", (unsigned long)app->grid_period_ns);
            app->have_grid = app->grid_period_ns > 10000000 && app->grid_period_ns < 50000000;
            app->grid_updates++;
        }
        break;
    case 'L': {
        char name[PICKER_NAME];
        uint8_t n = m->len - 1;
        if(n >= sizeof(name)) n = sizeof(name) - 1;
        memcpy(name, &d[1], n);
        name[n] = '\0';
        run_stop(app);
        bool ok = seq_open(app, name);
        log_line("remote: load %s: %s", name, ok ? "ok" : app->seq_err);
        uint8_t b[7] = {'l', ok, ok ? app->seq->n_params : 0};
        uint32_t cyc = ok ? seq_cycle_us(app->seq) : 0;
        memcpy(&b[3], &cyc, 4);
        fp_ble_send(b, sizeof(b));
        if(ok)
            for(uint8_t i = 0; i < app->seq->n_params; i++) send_param(app, i);
        else
            r_say("load: line %u %s", app->seq_err_line, app->seq_err);
        break;
    }
    case 'P':
        if(m->len >= 4 && app->seq_loaded && d[1] < app->seq->n_params && !seq_playing()) {
            SeqParam* p = &app->seq->params[d[1]];
            int16_t v;
            memcpy(&v, &d[2], 2);
            if(v < p->min) v = p->min;
            if(v > p->max) v = p->max;
            p->value = v;
            seq_compile(app->seq);
            log_line("remote: param %s = %d", p->name, v);
            send_param(app, d[1]);
        } else {
            r_say("param refused");
        }
        break;
    case 'G': {
        if(m->len < 15) break;
        uint16_t id;
        uint64_t after;
        int32_t offset;
        memcpy(&id, &d[1], 2);
        memcpy(&after, &d[3], 8);
        memcpy(&offset, &d[11], 4);
        uint16_t safe_us = 0;
        if(m->len >= 17) memcpy(&safe_us, &d[15], 2);
        if(safe_us) offset = 0;
        uint8_t status = 0;
        uint64_t start = 0;
        if(!app->have_grid)
            status = 1;
        else if(!app->seq_loaded)
            status = 2;
        else if(seq_playing())
            status = 3;
        else {
            /* first frame start at or after `after` */
            int64_t diff_ns = ((int64_t)after - (int64_t)app->grid_anchor_us) * 1000;
            int64_t n = diff_ns / (int64_t)app->grid_period_ns;
            if(n * (int64_t)app->grid_period_ns < diff_ns) n++;
            start = (uint64_t)((int64_t)app->grid_anchor_us +
                               (n * (int64_t)app->grid_period_ns + 500) / 1000 + offset);
            uint64_t now = pad_now_us();
            if(start < now + 20000) status = 4; /* too late: the console reads ahead */
        }
        if(status == 0) {
            seq_compile_period(app->seq, app->grid_period_ns);
            uint32_t lead = pad_usb_poll_period_us();
            if(lead == 0) lead = pad_usb_interval() * 1000u;
            live_release_all(app);
            if(safe_us)
                seq_set_frame_mode(app->grid_anchor_us, app->grid_period_ns, safe_us);
            else
                seq_set_frame_mode(0, 0, 0);
            seq_play_at(app->seq, start, lead);
            app->go_id = id;
            app->remote_run = true;
            app->was_playing = true;
            app->run_started = furi_get_tick();
            log_line(
                "remote: go %u at %lu.%06lu (in %ld ms) offset %ld us, period %lu ns, lead %lu us, frame mode %u us, usb %s",
                id,
                (unsigned long)(start / 1000000),
                (unsigned long)(start % 1000000),
                (long)((int64_t)(start - pad_now_us()) / 1000),
                (long)offset,
                (unsigned long)app->grid_period_ns,
                (unsigned long)lead,
                safe_us,
                usb_state_text());
        } else {
            log_line("remote: go %u refused, status %u", id, status);
        }
        uint8_t b[12] = {'g'};
        memcpy(&b[1], &id, 2);
        b[3] = status;
        memcpy(&b[4], &start, 8);
        fp_ble_send(b, sizeof(b));
        break;
    }
    case 'X':
        run_stop(app);
        r_say("stopped");
        break;
    case 'W': {
        static File* wf = NULL;
        static Storage* ws = NULL;
        static uint32_t wbytes = 0;
        static bool wok = false;
        uint8_t flags = m->len >= 2 ? d[1] : 0xFF;
        if(flags == 1) {
            char name[PICKER_NAME];
            uint8_t n = m->len - 2;
            if(n >= sizeof(name)) n = sizeof(name) - 1;
            memcpy(name, &d[2], n);
            name[n] = '\0';
            wok = n > 0 && !strchr(name, '/') && strstr(name, "..") == NULL;
            if(wf) {
                storage_file_close(wf);
                storage_file_free(wf);
                wf = NULL;
            }
            if(wok) {
                char path[96];
                snprintf(path, sizeof(path), SEQ_DIR "/%s", name);
                ws = furi_record_open(RECORD_STORAGE);
                storage_simply_mkdir(ws, SEQ_DIR);
                wf = storage_file_alloc(ws);
                wok = storage_file_open(wf, path, FSAM_WRITE, FSOM_CREATE_ALWAYS);
                log_line("remote: writing %s: %s", path, wok ? "open" : "FAILED");
            }
            wbytes = 0;
        } else if(flags == 0 && wf && wok) {
            uint16_t n = m->len - 2;
            if(storage_file_write(wf, &d[2], n) != n) wok = false;
            wbytes += n;
        } else if(flags == 2) {
            if(wf) {
                storage_file_close(wf);
                storage_file_free(wf);
                wf = NULL;
                furi_record_close(RECORD_STORAGE);
            }
            log_line("remote: write done, %lu bytes, %s", (unsigned long)wbytes, wok ? "ok" : "FAILED");
            uint8_t b[6] = {'w', wok};
            memcpy(&b[2], &wbytes, 4);
            fp_ble_send(b, sizeof(b));
        }
        break;
    }
    case 'H': {
        if(m->len < 7 || seq_playing()) {
            r_say("hold refused");
            break;
        }
        uint32_t buttons;
        uint16_t ms;
        memcpy(&buttons, &d[1], 4);
        memcpy(&ms, &d[5], 2);
        PadState st = {0};
        st.buttons = buttons;
        if(m->len >= 11) {
            st.lx = (int8_t)d[7];
            st.ly = (int8_t)d[8];
            st.rx = (int8_t)d[9];
            st.ry = (int8_t)d[10];
        }
        seq_set_live(&st);
        app->hold_until = ms ? furi_get_tick() + furi_ms_to_ticks(ms) : 0;
        log_line(
            "remote: hold buttons %06lX sticks %d,%d %d,%d for %u ms%s",
            (unsigned long)buttons, st.lx, st.ly, st.rx, st.ry, ms, ms ? "" : " (until changed)");
        r_say("hold %06lX %u ms", (unsigned long)buttons, ms);
        break;
    }
    case 'U': {
        PadKind kind = app->kind;
        uint8_t interval = interval_choices[app->interval_idx];
        if(m->len >= 3 && d[1] != 0xFF) {
            kind = d[1] ? PadHori : PadPro;
            interval = d[2];
        }
        bool ok = connect_pad(app, kind, interval);
        uint8_t b[4] = {'u', ok, kind == PadHori, interval};
        fp_ble_send(b, sizeof(b));
        break;
    }
    case 'C':
        run_stop(app);
        pad_usb_stop();
        log_line("remote: USB pad off");
        r_say("usb off");
        break;
    case 'Q': {
        uint8_t b[24] = {'q', usb_state_code(), seq_playing()};
        uint32_t pp = pad_usb_poll_period_us();
        uint64_t lp = pad_usb_last_poll_us();
        uint32_t cyc = seq_cycles_done();
        memcpy(&b[3], &pp, 4);
        memcpy(&b[7], &lp, 8);
        memcpy(&b[15], &cyc, 4);
        b[19] = app->have_grid;
        b[20] = app->seq_loaded;
        fp_ble_send(b, 21);
        break;
    }
    case 'E': {
        SeqEdge edges[SEQ_EDGE_LOG];
        uint8_t ne = seq_edges(edges, SEQ_EDGE_LOG);
        uint64_t st = seq_start_time();
        uint8_t h[10] = {'e', 0xFF, ne};
        memcpy(&h[3], &st, 7); /* low 56 bits are plenty */
        fp_ble_send(h, sizeof(h));
        for(uint8_t i = 0; i < ne; i++) {
            uint8_t b[10] = {'e', i};
            memcpy(&b[2], &edges[i].t_us, 4);
            memcpy(&b[6], &edges[i].buttons, 4);
            if(!fp_ble_send(b, sizeof(b))) break; /* queue full: Mac asks again */
        }
        break;
    }
    case 'Y': {
        picker_scan(app);
        for(uint8_t i = 0; i < app->n_names; i++) {
            uint8_t b[FP_MSG_MAX];
            b[0] = 'y';
            b[1] = i;
            size_t n = strlen(app->names[i]);
            if(n > FP_MSG_MAX - 2) n = FP_MSG_MAX - 2;
            memcpy(&b[2], app->names[i], n);
            fp_ble_send(b, (uint8_t)(2 + n));
        }
        uint8_t e[3] = {'y', 0xFF, app->n_names};
        fp_ble_send(e, sizeof(e));
        break;
    }
    case 'D': {
        char name[PICKER_NAME];
        uint8_t n = m->len - 1;
        if(n >= sizeof(name)) n = sizeof(name) - 1;
        memcpy(name, &d[1], n);
        name[n] = '\0';
        bool ok = n > 0 && !strchr(name, '/') && strstr(name, "..") == NULL;
        if(ok) {
            char path[96];
            snprintf(path, sizeof(path), SEQ_DIR "/%s", name);
            Storage* st = furi_record_open(RECORD_STORAGE);
            ok = storage_simply_remove(st, path);
            furi_record_close(RECORD_STORAGE);
        }
        log_line("remote: delete %s: %s", name, ok ? "ok" : "FAILED");
        r_say("delete %s", ok ? "ok" : "failed");
        break;
    }
    case 'O': {
        uint16_t want = 600;
        if(m->len >= 3) memcpy(&want, &d[1], 2);
        if(want > 1700) want = 1700; /* fits the send queue */
        Storage* st = furi_record_open(RECORD_STORAGE);
        File* f = storage_file_alloc(st);
        if(storage_file_open(f, LOG_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
            uint64_t size = storage_file_size(f);
            storage_file_seek(f, size > want ? (uint32_t)(size - want) : 0, true);
            uint8_t b[FP_MSG_MAX];
            b[0] = 'o';
            uint16_t got;
            while((got = storage_file_read(f, &b[1], FP_MSG_MAX - 1)) > 0) {
                if(!fp_ble_send(b, (uint8_t)(1 + got))) break;
            }
        }
        storage_file_close(f);
        storage_file_free(f);
        furi_record_close(RECORD_STORAGE);
        uint8_t e = 'o';
        fp_ble_send(&e, 1);
        break;
    }
    case 'Z':
        log_line("remote: quit requested over Bluetooth");
        r_say("quitting");
        run_stop(app);
        app->ble_quit_ms = 1; /* the main loop stops Bluetooth and exits */
        break;
    default:
        r_say("unknown cmd %c", d[0]);
        break;
    }
    remote_screen(app);
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */

int32_t frame_perfect_main(void* p) {
    const char* args = p;
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));
    app->seq = malloc(sizeof(Sequence));
    memset(app->seq, 0, sizeof(Sequence));
    app->probe = malloc(sizeof(Sequence));
    build_probe(app->probe);
    app->running = true;
    settings_load(app);
    apply_recommendation(app);
    app->queue = furi_message_queue_alloc(32, sizeof(AppEvent));

    pad_usb_alloc(on_rx, app);
    pad_usb_set_state_fn(seq_state_at);

    app->vp = view_port_alloc();
    view_port_draw_callback_set(app->vp, draw_cb, app);
    view_port_input_callback_set(app->vp, input_cb, app);
    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->vp, GuiLayerFullscreen);

    log_line(
        "start Frame Perfect 0.2, settings: %s %s %u ms measured=%luus confirmed=%s tested=%s",
        app->rec_valid ? "saved" : "none (defaults)",
        kind_name(app->kind),
        interval_choices[app->interval_idx],
        (unsigned long)app->rec_mean_us,
        app->rec_confirmed ? "yes" : "no",
        app->rec_when[0] ? app->rec_when : "-");
    parse_args(app, args);

    AppEvent ev;
    while(app->running) {
        if(furi_message_queue_get(app->queue, &ev, app->ble_active ? 1 : furi_ms_to_ticks(5)) == FuriStatusOk) {
            if(ev.type == EvInput)
                handle_input(app, &ev.input);
            else
                handle_rx(app, &ev.rx);
        }
        uint32_t now = furi_get_tick();
        /* the sweep takes its own statistics */
        if(pad_usb_active() && !app->sweeping && now - app->stats_at >= 1000) {
            app->stats_at = now;
            pad_usb_take_stats(&app->last);
            app->have_last = true;
            if(pad_usb_streaming() &&
               (app->autotest || seq_playing() || now - app->stats_logged_at >= 10000)) {
                app->stats_logged_at = now;
                log_stats("stats", &app->last);
            }
            if(app->remote && app->screen == ScrPrompt) remote_screen(app);
            app_redraw(app);
        }
        /* connection events, for the log */
        if(pad_usb_active()) {
            PadUsbEvents ev;
            pad_usb_events(&ev);
            if(memcmp(&ev, &app->ev_seen, sizeof(ev) - sizeof(uint32_t)) != 0) {
                log_line(
                    "usb %s %ums: configured=%lu deconfigured=%lu suspended=%lu resumed=%lu (state %s)",
                    kind_name(pad_usb_kind()),
                    pad_usb_interval(),
                    (unsigned long)ev.configured,
                    (unsigned long)ev.deconfigured,
                    (unsigned long)ev.suspended,
                    (unsigned long)ev.resumed,
                    usb_state_text());
            }
            app->ev_seen = ev;
            bool st = pad_usb_streaming();
            if(st != app->streaming_seen) {
                app->streaming_seen = st;
                log_line(
                    "%s %s %ums after %lums since configure; rx so far %lu, rumble-only %lu",
                    st ? "streaming started" : "streaming stopped",
                    kind_name(pad_usb_kind()),
                    pad_usb_interval(),
                    (unsigned long)(now - pad_usb_configured_at_ms()),
                    (unsigned long)app->rx_count,
                    (unsigned long)app->rumble_only);
                if(pad_usb_kind() == PadPro) log_handshake(app);
            }
        } else {
            memset(&app->ev_seen, 0, sizeof(app->ev_seen));
        }
        if(app->was_playing && !seq_playing()) {
            app->was_playing = false;
            log_run_end(app, "done");
            if(app->remote_run) {
                app->remote_run = false;
                uint8_t b[11] = {'d'};
                uint32_t cyc = seq_cycles_done(), ed = seq_edge_total();
                memcpy(&b[1], &app->go_id, 2);
                memcpy(&b[3], &cyc, 4);
                memcpy(&b[7], &ed, 4);
                fp_ble_send(b, sizeof(b));
            }
            app_redraw(app);
        }
        if(pad_usb_active()) pad_usb_kick();
        live_expire_taps(app);
        if(app->ble_active) {
            fp_ble_poll();
            if(app->remote) {
                FpMsg m;
                while(fp_ble_take_cmd(&m)) remote_cmd(app, &m);
                if(app->hold_until && (int32_t)(furi_get_tick() - app->hold_until) >= 0) {
                    app->hold_until = 0;
                    PadState idle = {0};
                    seq_set_live(&idle);
                    log_line("remote: hold released");
                    r_say("released");
                }
            }
            bool c = fp_ble_connected();
            if(c != app->ble_seen_connected) {
                app->ble_seen_connected = c;
                log_line("blesync: Mac %s (pings answered %lu)", c ? "connected" : "disconnected", (unsigned long)fp_ble_pings());
                if(app->remote) {
                    remote_screen(app);
                } else {
                    snprintf(app->p_buf, sizeof(app->p_buf), "Mac %s", c ? "connected" : "not connected");
                    app->p_lines[3] = app->p_buf;
                    app_redraw(app);
                }
            }
            if(app->ble_quit_ms && now - app->ble_t0 > app->ble_quit_ms) {
                {
                    FuriString* st = furi_string_alloc();
                    fp_ble_stats(st);
                    log_line("blesync stop: %s", furi_string_get_cstr(st));
                    furi_string_free(st);
                }
                fp_ble_stop();
                fp_clock_cli_unregister();
                app->ble_active = false;
                app->running = false;
            }
        }
        sweep_tick(app);
        flow_tick(app);
        if(app->auto_answer) {
            /* test only: answer whatever the screen asks after 1 s */
            if(app->screen == ScrPrompt && !app->p_wait && now - app->flow_t0 > 1000) {
                bool ask = app->flow == FlowCheckAsk || app->flow == FlowGameAskMoved ||
                           app->flow == FlowGameAskOwn;
                bool yes = !ask || app->auto_answer == 'y';
                if(app->flow == FlowAutoNothing) yes = false; /* don't retry forever */
                InputEvent ie = {.key = yes ? InputKeyOk : InputKeyBack, .type = InputTypeShort};
                log_line("auto answer flow=%u: %s", app->flow, yes ? "OK" : "Back");
                handle_input(app, &ie);
            } else if(app->screen == ScrAuto && !app->sweeping && app->sweep_phase == 3) {
                app->running = !app->quit_after_sweep;
            } else if(app->screen == ScrHome && app->quit_after_sweep) {
                app->running = false;
            }
        }
        auto_tick(app);
        if(seq_playing() && app->screen == ScrRun) app_redraw(app);
    }

    run_stop(app);
    seq_stop();
    if(app->ble_active) {
        fp_ble_stop();
        fp_clock_cli_unregister();
    }
    pad_usb_free();
    log_line("exit");

    gui_remove_view_port(app->gui, app->vp);
    view_port_free(app->vp);
    furi_record_close(RECORD_GUI);
    furi_message_queue_free(app->queue);
    free(app->seq);
    free(app->probe);
    free(app);
    return 0;
}
