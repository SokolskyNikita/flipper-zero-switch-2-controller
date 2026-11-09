#include "fp_ble.h"
#include "pad_usb.h"

#include <furi.h>
#include <furi_hal.h>
#include <bt/bt_service/bt.h>
#include <furi_ble/profile_interface.h>
#include <gap.h>
#include <profiles/serial_profile.h>
#include <services/serial_service.h>
#include <services/dev_info_service.h>
#include <services/battery_service.h>
#include <extra_services/hid_service.h>
#include <furi_hal_usb_hid.h>
#include <usb_hid.h>
#include <ble/core/ble_defs.h>
#include <ble/core/ble_std.h>
#include <ble/core/auto/ble_types.h>
#include <furi_ble/event_dispatcher.h>
#include <cli/cli.h>
#include <toolbox/cli/cli_registry.h>
#include <toolbox/pipe.h>
#include <stdio.h>
#include <string.h>

#define TAG "FramePerfectBle"
#define RX_BUF 256
/* 15 ms in 1.25 ms units: Apple's floor for ordinary accessories */
#define CONN_INTERVAL_15MS 12

/* ------------------------------------------------------------------ */
/* Profile: the stock serial profile with a fixed 15 ms interval      */
/* ------------------------------------------------------------------ */

typedef struct {
    FuriHalBleProfileBase base; /* must be first */
    BleServiceDevInfo* dev_info;
    BleServiceBattery* battery;
    BleServiceSerial* serial;
    BleServiceHid* hid; /* only in the HID variant */
} FpProfile;

static const FuriHalBleProfileTemplate fp_template;

static FuriHalBleProfileBase* fp_profile_start(FuriHalBleProfileParams params) {
    UNUSED(params);
    FpProfile* p = malloc(sizeof(FpProfile));
    p->base.config = &fp_template;
    /* same services in the same order as the stock profile, so the Mac's
     * cached view of the device still matches */
    p->dev_info = ble_svc_dev_info_start();
    p->battery = ble_svc_battery_start(true);
    p->serial = ble_svc_serial_start();
    p->hid = NULL;
    return &p->base;
}

static void fp_profile_stop(FuriHalBleProfileBase* profile) {
    furi_check(profile && profile->config == &fp_template);
    FpProfile* p = (FpProfile*)profile;
    ble_svc_battery_stop(p->battery);
    ble_svc_dev_info_stop(p->dev_info);
    ble_svc_serial_stop(p->serial);
}

static void fp_profile_gap_config(GapConfig* config, FuriHalBleProfileParams params) {
    /* stock config: name, address, pairing; then a fixed 15 ms interval */
    ble_profile_serial->get_gap_config(config, params);
    config->conn_param.conn_int_min = CONN_INTERVAL_15MS;
    config->conn_param.conn_int_max = CONN_INTERVAL_15MS;
    /* Note (2026-11-09): this request is never sent. The firmware's GAP code
     * only renegotiates when the central's interval is *below* conn_int_min,
     * so macOS's 30 ms is kept. Apps can't send the L2CAP update themselves
     * (no aci_* exports). A gamepad appearance (0x03C4) didn't change
     * macOS's choice either ("LowLatency NO", 30 ms). */
}

static const FuriHalBleProfileTemplate fp_template = {
    .start = fp_profile_start,
    .stop = fp_profile_stop,
    .get_gap_config = fp_profile_gap_config,
};

/* ------------------------------------------------------------------ */
/* Profile variant: HID (mouse appearance) + our serial data channel  */
/* ------------------------------------------------------------------ */
/* macOS moves LE HID devices to 15 ms, and HIDs with a mouse or touchpad
 * appearance to 7.5 ms (flipper-zero/macos-ble-hid-latency.md, section 6).
 * The HID service and report map are the stock Flipper ones (keyboard +
 * mouse + consumer); nothing is ever sent on them. */

enum { FpReportIdKeyboard = 1, FpReportIdMouse = 2, FpReportIdConsumer = 3 };

static const uint8_t fp_hid_report_map[] = {
    HID_USAGE_PAGE(HID_PAGE_DESKTOP),
    HID_USAGE(HID_DESKTOP_KEYBOARD),
    HID_COLLECTION(HID_APPLICATION_COLLECTION),
    HID_REPORT_ID(FpReportIdKeyboard),
    HID_USAGE_PAGE(HID_DESKTOP_KEYPAD),
    HID_USAGE_MINIMUM(HID_KEYBOARD_L_CTRL),
    HID_USAGE_MAXIMUM(HID_KEYBOARD_R_GUI),
    HID_LOGICAL_MINIMUM(0),
    HID_LOGICAL_MAXIMUM(1),
    HID_REPORT_SIZE(1),
    HID_REPORT_COUNT(8),
    HID_INPUT(HID_IOF_DATA | HID_IOF_VARIABLE | HID_IOF_ABSOLUTE),
    HID_REPORT_COUNT(1),
    HID_REPORT_SIZE(8),
    HID_INPUT(HID_IOF_CONSTANT | HID_IOF_VARIABLE | HID_IOF_ABSOLUTE),
    HID_USAGE_PAGE(HID_PAGE_LED),
    HID_REPORT_COUNT(8),
    HID_REPORT_SIZE(1),
    HID_USAGE_MINIMUM(1),
    HID_USAGE_MAXIMUM(8),
    HID_OUTPUT(HID_IOF_DATA | HID_IOF_VARIABLE | HID_IOF_ABSOLUTE),
    HID_REPORT_COUNT(6),
    HID_REPORT_SIZE(8),
    HID_LOGICAL_MINIMUM(0),
    HID_LOGICAL_MAXIMUM(101),
    HID_USAGE_PAGE(HID_DESKTOP_KEYPAD),
    HID_USAGE_MINIMUM(0),
    HID_USAGE_MAXIMUM(101),
    HID_INPUT(HID_IOF_DATA | HID_IOF_ARRAY | HID_IOF_ABSOLUTE),
    HID_END_COLLECTION,
    HID_USAGE_PAGE(HID_PAGE_DESKTOP),
    HID_USAGE(HID_DESKTOP_MOUSE),
    HID_COLLECTION(HID_APPLICATION_COLLECTION),
    HID_USAGE(HID_DESKTOP_POINTER),
    HID_COLLECTION(HID_PHYSICAL_COLLECTION),
    HID_REPORT_ID(FpReportIdMouse),
    HID_USAGE_PAGE(HID_PAGE_BUTTON),
    HID_USAGE_MINIMUM(1),
    HID_USAGE_MAXIMUM(3),
    HID_LOGICAL_MINIMUM(0),
    HID_LOGICAL_MAXIMUM(1),
    HID_REPORT_COUNT(3),
    HID_REPORT_SIZE(1),
    HID_INPUT(HID_IOF_DATA | HID_IOF_VARIABLE | HID_IOF_ABSOLUTE),
    HID_REPORT_SIZE(1),
    HID_REPORT_COUNT(5),
    HID_INPUT(HID_IOF_CONSTANT | HID_IOF_VARIABLE | HID_IOF_ABSOLUTE),
    HID_USAGE_PAGE(HID_PAGE_DESKTOP),
    HID_USAGE(HID_DESKTOP_X),
    HID_USAGE(HID_DESKTOP_Y),
    HID_USAGE(HID_DESKTOP_WHEEL),
    HID_LOGICAL_MINIMUM(-127),
    HID_LOGICAL_MAXIMUM(127),
    HID_REPORT_SIZE(8),
    HID_REPORT_COUNT(3),
    HID_INPUT(HID_IOF_DATA | HID_IOF_VARIABLE | HID_IOF_RELATIVE),
    HID_END_COLLECTION,
    HID_END_COLLECTION,
    HID_USAGE_PAGE(HID_PAGE_CONSUMER),
    HID_USAGE(HID_CONSUMER_CONTROL),
    HID_COLLECTION(HID_APPLICATION_COLLECTION),
    HID_REPORT_ID(FpReportIdConsumer),
    HID_LOGICAL_MINIMUM(0),
    HID_RI_LOGICAL_MAXIMUM(16, 0x3FF),
    HID_USAGE_MINIMUM(0),
    HID_RI_USAGE_MAXIMUM(16, 0x3FF),
    HID_REPORT_COUNT(1),
    HID_REPORT_SIZE(16),
    HID_INPUT(HID_IOF_DATA | HID_IOF_ARRAY | HID_IOF_ABSOLUTE),
    HID_END_COLLECTION,
};

#define FP_GAP_APPEARANCE_MOUSE 0x03C2

static const FuriHalBleProfileTemplate fp_hid_template;

static FuriHalBleProfileBase* fp_hid_profile_start(FuriHalBleProfileParams params) {
    UNUSED(params);
    FpProfile* p = malloc(sizeof(FpProfile));
    p->base.config = &fp_hid_template;
    p->battery = ble_svc_battery_start(true);
    p->dev_info = ble_svc_dev_info_start();
    /* serial first: its TX/RX values are large, and with the HID service
     * added first they didn't fit in the radio's attribute memory (only the
     * flow-control and RPC-status characteristics appeared). */
    p->serial = ble_svc_serial_start();
    p->hid = ble_svc_hid_start();
    ble_svc_hid_update_report_map(p->hid, fp_hid_report_map, sizeof(fp_hid_report_map));
    uint8_t info[4] = {0x01, 0x01, 0x00, 0x03}; /* HID 1.01, no country, wake + connectable */
    ble_svc_hid_update_info(p->hid, info);
    return &p->base;
}

static void fp_hid_profile_stop(FuriHalBleProfileBase* profile) {
    furi_check(profile && profile->config == &fp_hid_template);
    FpProfile* p = (FpProfile*)profile;
    ble_svc_serial_stop(p->serial);
    ble_svc_hid_stop(p->hid);
    ble_svc_dev_info_stop(p->dev_info);
    ble_svc_battery_stop(p->battery);
}

static void fp_hid_profile_gap_config(GapConfig* config, FuriHalBleProfileParams params) {
    UNUSED(params);
    ble_profile_serial->get_gap_config(config, NULL);
    config->adv_service.UUID_Type = UUID_TYPE_16;
    config->adv_service.Service_UUID_16 = 0x1812; /* HID */
    config->appearance_char = FP_GAP_APPEARANCE_MOUSE;
    config->bonding_mode = true;
    config->pairing_method = GapPairingPinCodeVerifyYesNo;
    config->conn_param.conn_int_min = 0x06; /* 7.5 ms */
    config->conn_param.conn_int_max = 0x24;
    config->conn_param.slave_latency = 0;
    config->conn_param.supervisor_timeout = 0;
    /* own address and name, so the Mac sees a new device, separate from
     * the Flipper's normal pairing */
    config->mac_address[2] += 2;
    snprintf(
        config->adv_name,
        sizeof(config->adv_name),
        "%cFP %s",
        furi_hal_version_get_ble_local_device_name_ptr()[0],
        furi_hal_version_get_name_ptr());
}

static const FuriHalBleProfileTemplate fp_hid_template = {
    .start = fp_hid_profile_start,
    .stop = fp_hid_profile_stop,
    .get_gap_config = fp_hid_profile_gap_config,
};

#define FP_HID_KEYS_PATH APP_DATA_PATH(".bt_hid.keys")

/* ------------------------------------------------------------------ */
/* State                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t seq;
    uint64_t t_rx;
} Ping;

#define FP_OUTQ 64

static struct {
    Bt* bt;
    FpProfile* profile;
    FuriMessageQueue* pings;
    volatile bool connected;
    uint32_t answered;
    GapSvcEventHandler* rx_hook;
    uint32_t hooked;
    uint32_t keepalive_us; /* 0 = off */
    uint64_t last_tx;
    uint32_t keepalives;
    uint32_t q_full;
    uint32_t tx_fail;
    bool no_reply;       /* test: receive pings but don't answer */
    uint8_t want_phy;    /* 0 = leave alone, 1 = 1M, 2 = 2M */
    bool phy_sent;
    int phy_status;
    volatile uint16_t conn_handle;
    volatile bool have_handle;
    FuriMessageQueue* cmds; /* FpMsg from the Mac, for the app */
    uint32_t cmds_dropped;
    uint8_t outq[FP_OUTQ][FP_MSG_MAX + 1]; /* [0] = length */
    uint8_t out_head, out_tail;
    uint32_t out_dropped;
    bool no_latency;     /* send "peripheral latency off" after each connect */
    bool latency_sent;
    int latency_status;
} s;

/* The firmware's HID service claims every GATT write event (it returns
 * "handled" for any ACI_GATT_ATTRIBUTE_MODIFIED), and the dispatcher stops at
 * the first handler that claims an event. Handlers registered later are asked
 * first, so this hook, added after the profile starts, sees our pings before
 * the HID service can swallow them. */
/* HCI packet layout from the ST transport header (tl.h), not in the SDK */
typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t data[];
} FpHciUartPckt;
typedef struct __attribute__((packed)) {
    uint8_t evt;
    uint8_t plen;
    uint8_t data[];
} FpHciEventPckt;
typedef struct __attribute__((packed)) {
    uint16_t ecode;
    uint8_t data[];
} FpBlecoreEvt;
#define FP_ACI_GATT_ATTRIBUTE_MODIFIED 0x0C01

static BleEventAckStatus rx_hook_cb(void* event, void* ctx) {
    UNUSED(ctx);
    FpHciEventPckt* ev = (FpHciEventPckt*)(((FpHciUartPckt*)event)->data);
    if(ev->evt != HCI_VENDOR_SPECIFIC_DEBUG_EVT_CODE) return BleEventNotAck;
    FpBlecoreEvt* core = (FpBlecoreEvt*)ev->data;
    if(core->ecode != FP_ACI_GATT_ATTRIBUTE_MODIFIED) return BleEventNotAck;
    aci_gatt_attribute_modified_event_rp0* am = (aci_gatt_attribute_modified_event_rp0*)core->data;
    s.conn_handle = am->Connection_Handle;
    s.have_handle = true;
    if(am->Attr_Data_Length == 5 && am->Attr_Data[0] == 'S') {
        Ping p = {.t_rx = pad_now_us()};
        memcpy(&p.seq, &am->Attr_Data[1], 4);
        if(furi_message_queue_put(s.pings, &p, 0) != FuriStatusOk) s.q_full++;
        s.hooked++;
        return BleEventAckFlowEnable;
    }
    /* commands for the app: an upper-case letter, then a binary payload */
    if(am->Attr_Data_Length >= 1 && am->Attr_Data_Length <= FP_CMD_MAX && s.cmds &&
       am->Attr_Data[0] >= 'A' && am->Attr_Data[0] <= 'Z') {
        FpMsg m = {.t_rx = pad_now_us(), .len = (uint8_t)am->Attr_Data_Length};
        memcpy(m.d, am->Attr_Data, m.len);
        if(furi_message_queue_put(s.cmds, &m, 0) != FuriStatusOk) s.cmds_dropped++;
        return BleEventAckFlowEnable;
    }
    return BleEventNotAck;
}

bool fp_ble_take_cmd(FpMsg* m) {
    return s.cmds && furi_message_queue_get(s.cmds, m, 0) == FuriStatusOk;
}

bool fp_ble_send(const void* data, uint8_t len) {
    if(!s.bt || len == 0 || len > FP_MSG_MAX) return false;
    uint8_t next = (uint8_t)((s.out_head + 1) % FP_OUTQ);
    if(next == s.out_tail) {
        s.out_dropped++;
        return false;
    }
    s.outq[s.out_head][0] = len;
    memcpy(&s.outq[s.out_head][1], data, len);
    s.out_head = next;
    return true;
}

/* ------------------------------------------------------------------ */
/* Radio command access without a firmware change                      */
/* ------------------------------------------------------------------ */
/* Apps can't call the radio's ACI commands: the firmware doesn't export
 * them. But the exported furi_hal_bt_stop_tone_tx() is a tail jump to ST's
 * aci_hal_tone_stop(), which calls hci_send_req(). We follow those two jumps,
 * check the code looks exactly as expected (else do nothing), and send our
 * own command through hci_send_req. Checked on Momentum 93ac669b. */

typedef struct {
    uint16_t ogf;
    uint16_t ocf;
    int event;
    void* cparam;
    int clen;
    void* rparam;
    int rlen;
} FpHciRequest;
typedef int (*FpHciSendReq)(FpHciRequest* req, uint8_t async);

/* Target of a Thumb-2 B.W (T4) or BL at addr, or 0 if it isn't one */
static uint32_t t32_branch_target(uint32_t addr, bool want_bl) {
    const uint16_t* h = (const uint16_t*)addr;
    uint16_t h1 = h[0], h2 = h[1];
    if((h1 & 0xF800) != 0xF000) return 0;
    if(want_bl ? ((h2 & 0xD000) != 0xD000) : ((h2 & 0xD000) != 0x9000)) return 0;
    uint32_t s = (h1 >> 10) & 1, j1 = (h2 >> 13) & 1, j2 = (h2 >> 11) & 1;
    uint32_t i1 = !(j1 ^ s), i2 = !(j2 ^ s);
    uint32_t imm = (s << 24) | (i1 << 23) | (i2 << 22) | ((h1 & 0x3FF) << 12) | ((h2 & 0x7FF) << 1);
    if(s) imm |= 0xFE000000;
    return addr + 4 + imm;
}

static FpHciSendReq find_hci_send_req(void) {
    uint32_t f = (uint32_t)(uintptr_t)furi_hal_bt_stop_tone_tx & ~1u;
    if(f < 0x08000000 || f >= 0x08100000) return NULL;
    uint32_t tone_stop = t32_branch_target(f, false);
    if(tone_stop < 0x08000000 || tone_stop >= 0x08100000) return NULL;
    /* aci_hal_tone_stop: push {r4,lr}; movs r4,#0; sub sp,#0x20 ... bl hci_send_req
     * at +0x26, literal ogf 0x3f / ocf 0x16 at +0x38 */
    const uint16_t* c = (const uint16_t*)tone_stop;
    const uint32_t* lit = (const uint32_t*)(tone_stop + 0x38);
    if(c[0] != 0xB510 || c[1] != 0x2400 || c[2] != 0xB088 || *lit != 0x0016003F) return NULL;
    uint32_t send = t32_branch_target(tone_stop + 0x26, true);
    if(send < 0x08000000 || send >= 0x08100000) return NULL;
    return (FpHciSendReq)(uintptr_t)(send | 1);
}

/* ACI_HAL_SET_PERIPHERAL_LATENCY (0xFC20): enable=0 makes the Flipper listen on
 * every connection event whatever peripheral latency the Mac granted.
 * Returns the radio's status (0 = ok), or -1 if the sender wasn't found. */
static int fp_set_peripheral_latency(bool enable) {
    static FpHciSendReq send = NULL;
    if(!send) send = find_hci_send_req();
    if(!send) return -1;
    uint8_t param = enable ? 1 : 0;
    uint8_t status = 0xFF;
    FpHciRequest rq = {
        .ogf = 0x3F, .ocf = 0x020, .cparam = &param, .clen = 1, .rparam = &status, .rlen = 1};
    if(send(&rq, 0) < 0) return 0x100;
    return status;
}

/* HCI_LE_Set_PHY (OGF 0x08, OCF 0x032): ask for 1M or 2M both ways. The
 * controller answers with Command Status; the result comes later as an LE PHY
 * Update Complete event, which the firmware's GAP code logs. */
static int fp_set_phy(uint16_t handle, uint8_t phy) {
    static FpHciSendReq send = NULL;
    if(!send) send = find_hci_send_req();
    if(!send) return -1;
    uint8_t mask = phy == 1 ? 0x01 : 0x02;
    uint8_t cp[7] = {handle & 0xFF, handle >> 8, 0x00, mask, mask, 0x00, 0x00};
    uint8_t status = 0xFF;
    FpHciRequest rq = {
        .ogf = 0x08, .ocf = 0x032, .event = 0x0F, .cparam = cp, .clen = sizeof(cp), .rparam = &status, .rlen = 1};
    if(send(&rq, 0) < 0) return 0x100;
    return status;
}

static uint16_t serial_cb(SerialServiceEvent event, void* ctx) {
    UNUSED(ctx);
    if(event.event == SerialServiceEventTypeDataReceived) {
        uint64_t t = pad_now_us();
        const uint8_t* d = event.data.buffer;
        if(event.data.size >= 5 && d[0] == 'S') {
            Ping p = {.t_rx = t};
            memcpy(&p.seq, &d[1], 4);
            furi_message_queue_put(s.pings, &p, 0);
        }
    }
    return RX_BUF;
}

static void status_cb(BtStatus status, void* ctx) {
    UNUSED(ctx);
    s.connected = (status == BtStatusConnected);
    if(s.connected && s.profile) {
        ble_svc_serial_set_callbacks(s.profile->serial, RX_BUF, serial_cb, NULL);
        ble_svc_serial_set_rpc_active(s.profile->serial, false);
    }
}

bool fp_ble_start(bool hid) {
    if(s.bt) return true;
    s.pings = furi_message_queue_alloc(32, sizeof(Ping));
    s.cmds = furi_message_queue_alloc(16, sizeof(FpMsg));
    s.out_head = s.out_tail = 0;
    s.bt = furi_record_open(RECORD_BT);
    bt_disconnect(s.bt);
    furi_delay_ms(200);
    if(hid)
        bt_keys_storage_set_storage_path(s.bt, FP_HID_KEYS_PATH); /* own pairing */
    else
        bt_keys_storage_set_default_path(s.bt); /* keep the existing Mac pairing */
    s.profile = (FpProfile*)bt_profile_start(s.bt, hid ? &fp_hid_template : &fp_template, NULL);
    if(!s.profile) {
        FURI_LOG_E(TAG, "profile start failed");
        furi_record_close(RECORD_BT);
        s.bt = NULL;
        furi_message_queue_free(s.pings);
        furi_message_queue_free(s.cmds);
        s.cmds = NULL;
        return false;
    }
    ble_svc_serial_set_callbacks(s.profile->serial, RX_BUF, serial_cb, NULL);
    if(hid) s.rx_hook = ble_event_dispatcher_register_svc_handler(rx_hook_cb, &s);
    /* macOS gives HID links peripheral latency 22: with nothing to send, the
     * Flipper may sleep through 22 connection events (330 ms at 15 ms), and
     * the Mac's writes wait for it. Sending something every 10 ms keeps it
     * listening on every event. */
    /* off unless the app sets it: fp_ble_set_keepalive_ms() */
    bt_set_status_changed_callback(s.bt, status_cb, NULL);
    furi_hal_bt_start_advertising();
    return true;
}

void fp_ble_stop(void) {
    if(!s.bt) return;
    bt_set_status_changed_callback(s.bt, NULL, NULL);
    bt_disconnect(s.bt);
    furi_delay_ms(200);
    if(s.rx_hook) {
        ble_event_dispatcher_unregister_svc_handler(s.rx_hook);
        s.rx_hook = NULL;
    }
    FURI_LOG_I(TAG, "pings via hook %lu, answered %lu, keepalives %lu", s.hooked, s.answered, s.keepalives);
    bt_keys_storage_set_default_path(s.bt);
    bt_profile_restore_default(s.bt);
    furi_record_close(RECORD_BT);
    s.bt = NULL;
    s.profile = NULL;
    s.connected = false;
    furi_message_queue_free(s.pings);
    FuriMessageQueue* c = s.cmds;
    s.cmds = NULL;
    furi_message_queue_free(c);
}

bool fp_ble_connected(void) {
    return s.connected;
}

void fp_ble_stats(FuriString* out) {
    furi_string_printf(
        out, "rx via hook %lu, queue full %lu, answered %lu, reply send retries %lu, keepalives %lu (every %lu ms), latency-off status %d, phy %uM request status %d",
        s.hooked, s.q_full, s.answered, s.tx_fail, s.keepalives, s.keepalive_us / 1000, fp_ble_latency_status(), s.want_phy, s.phy_sent ? s.phy_status : -2);
}

uint32_t fp_ble_pings(void) {
    return s.answered;
}

void fp_ble_set_keepalive_ms(uint32_t ms) {
    s.keepalive_us = ms * 1000;
}

void fp_ble_set_phy(uint8_t phy) {
    s.want_phy = phy;
}

void fp_ble_set_no_reply(bool on) {
    s.no_reply = on;
}

void fp_ble_set_no_latency(bool on) {
    s.no_latency = on;
}

int fp_ble_latency_status(void) {
    return s.latency_sent ? s.latency_status : -2;
}

void fp_ble_poll(void) {
    if(!s.bt || !s.profile) return;
    if(!s.connected) {
        s.latency_sent = false;
        s.phy_sent = false;
        s.have_handle = false;
    }
    if(s.want_phy && s.connected && s.have_handle && !s.phy_sent) {
        s.phy_status = fp_set_phy(s.conn_handle, s.want_phy);
        s.phy_sent = true;
        FURI_LOG_I(TAG, "set PHY %uM on handle 0x%04x: status %d", s.want_phy, s.conn_handle, s.phy_status);
    }
    if(s.no_latency && s.connected && !s.latency_sent) {
        furi_delay_ms(300); /* let the connection settle first */
        s.latency_status = fp_set_peripheral_latency(false);
        s.latency_sent = true;
        FURI_LOG_I(TAG, "peripheral latency off: status %d", s.latency_status);
    }
    /* One reply at a time: if the radio's notification buffer is busy, keep
     * the reply and try again on the next poll instead of dropping it. t_tx is
     * stamped on each attempt, so the Mac still gets the real send time. */
    static bool pending = false;
    static Ping cur;
    if(s.no_reply) {
        while(furi_message_queue_get(s.pings, &cur, 0) == FuriStatusOk) {
        }
        pending = false;
    }
    for(int n = 0; n < 4; n++) {
        if(!pending) {
            if(furi_message_queue_get(s.pings, &cur, 0) != FuriStatusOk) break;
            pending = true;
        }
        uint8_t r[21];
        r[0] = 'R';
        memcpy(&r[1], &cur.seq, 4);
        memcpy(&r[5], &cur.t_rx, 8);
        uint64_t t_tx = pad_now_us();
        memcpy(&r[13], &t_tx, 8);
        if(!ble_svc_serial_update_tx(s.profile->serial, r, sizeof(r))) {
            s.tx_fail++;
            break; /* retry next poll */
        }
        s.answered++;
        pending = false;
        s.last_tx = t_tx;
    }
    /* queued messages for the Mac, oldest first, after ping replies */
    while(!pending && s.connected && s.out_tail != s.out_head) {
        uint8_t* m = s.outq[s.out_tail];
        if(!ble_svc_serial_update_tx(s.profile->serial, &m[1], m[0])) {
            s.tx_fail++;
            break; /* retry next poll */
        }
        s.out_tail = (uint8_t)((s.out_tail + 1) % FP_OUTQ);
        s.last_tx = pad_now_us();
    }
    if(s.keepalive_us && s.connected && !pending) {
        uint64_t now = pad_now_us();
        if(now - s.last_tx >= s.keepalive_us) {
            /* 'K' u64 t_tx: also a timed Flipper->Mac stream, so the Mac can
             * tell whether the link was moving while a ping was delayed */
            uint8_t k[9];
            k[0] = 'K';
            memcpy(&k[1], &now, 8);
            if(ble_svc_serial_update_tx(s.profile->serial, k, sizeof(k))) s.keepalives++;
            s.last_tx = now;
        }
    }
}

/* ------------------------------------------------------------------ */
/* USB reference clock: CLI command fp_clock                          */
/* ------------------------------------------------------------------ */

static void fp_clock_cmd(PipeSide* pipe, FuriString* args, void* ctx) {
    UNUSED(args);
    UNUSED(ctx);
    char line[32];
    pipe_send(pipe, "ready\n", 6);
    for(;;) {
        uint8_t c;
        if(pipe_receive(pipe, &c, 1) != 1) break;
        uint64_t t = pad_now_us();
        if(c == 'q') break;
        /* split: printf here has no 64-bit support */
        int n = snprintf(
            line,
            sizeof(line),
            "c %lu %06lu\n",
            (unsigned long)(t / 1000000ULL),
            (unsigned long)(t % 1000000ULL));
        pipe_send(pipe, line, n);
    }
}

/* fp_peek: no args prints a few exported function addresses; "fp_peek ADDR LEN"
 * prints LEN bytes of memory at hex ADDR. Used to find the radio's command
 * sender in this firmware build. */
static void fp_peek_cmd(PipeSide* pipe, FuriString* args, void* ctx) {
    UNUSED(ctx);
    char line[96];
    int n;
    unsigned long addr = 0, len = 0;
    if(sscanf(furi_string_get_cstr(args), "%lx %lu", &addr, &len) != 2) {
        n = snprintf(
            line, sizeof(line), "stop_tone_tx %08lx\nget_rssi %08lx\nstart_rx %08lx\n",
            (unsigned long)(uintptr_t)furi_hal_bt_stop_tone_tx,
            (unsigned long)(uintptr_t)furi_hal_bt_get_rssi,
            (unsigned long)(uintptr_t)furi_hal_bt_start_rx);
        pipe_send(pipe, line, n);
        return;
    }
    if(addr < 0x08000000UL || addr + len > 0x08100000UL) {
        pipe_send(pipe, "flash only\n", 11);
        return;
    }
    const uint8_t* m = (const uint8_t*)addr;
    for(unsigned long i = 0; i < len; i += 32) {
        n = 0;
        for(unsigned long j = i; j < len && j < i + 32; j++)
            n += snprintf(line + n, sizeof(line) - n, "%02x", m[j]);
        line[n++] = '\n';
        pipe_send(pipe, line, n);
    }
}

void fp_clock_cli_register(void) {
    CliRegistry* reg = furi_record_open(RECORD_CLI);
    cli_registry_add_command(
        reg, "fp_clock", CliCommandFlagParallelSafe | CliCommandFlagDontAttachStdio, fp_clock_cmd, NULL);
    cli_registry_add_command(
        reg, "fp_peek", CliCommandFlagParallelSafe | CliCommandFlagDontAttachStdio, fp_peek_cmd, NULL);
    furi_record_close(RECORD_CLI);
}

void fp_clock_cli_unregister(void) {
    CliRegistry* reg = furi_record_open(RECORD_CLI);
    cli_registry_delete_command(reg, "fp_clock");
    cli_registry_delete_command(reg, "fp_peek");
    furi_record_close(RECORD_CLI);
}
