/*
 * USB side of Frame Perfect. See pad_usb.h for what changed against
 * Controller Ducky (Apache-2.0), which this file is derived from.
 *
 * Report timing. The console reads the interrupt IN endpoint every
 * bInterval. A read completes the transfer we queued (usbd_evt_eptx). In that
 * interrupt we immediately queue the next report, built from the state at
 * that moment. So the console always reads the state as it was one poll
 * earlier: a constant offset, which keeps durations intact. Every completed
 * read is timestamped for the poll statistics.
 */
#include "pad_usb.h"

#include <furi_hal.h>
#include <furi_hal_usb.h>
#include <usb.h>
#include <string.h>

#define TAG "FramePerfectUsb"

#define PRO_VID 0x057E
#define PRO_PID 0x2009
#define HORI_VID 0x0F0D
#define HORI_PID 0x00C1

#define EP_IN 0x81
#define PRO_EP_OUT 0x01
#define HORI_EP_OUT 0x02
#define EP_SIZE 64

#ifndef USB_EP0_SIZE
#define USB_EP0_SIZE 8
#endif

#define HID_REQ_GET_REPORT 0x01
#define HID_REQ_SET_REPORT 0x09
#define HID_REQ_SET_IDLE 0x0A
#define HID_REQ_SET_PROTOCOL 0x0B
#define HID_DTYPE_HID 0x21
#define HID_DTYPE_REPORT 0x22
#ifndef USB_CLASS_HID
#define USB_CLASS_HID 0x03
#endif

#define STICK_CENTER 2048
#define STICK_RANGE 1500 /* must match spi_stick_cal */
#define STICK_DIAG 1060

/* ------------------------------------------------------------------ */
/* Clock                                                              */
/* ------------------------------------------------------------------ */

#define DWT_CYCCNT (*(volatile uint32_t*)0xE0001004)

static uint32_t clk_last;
static uint64_t clk_high;
static uint32_t clk_div = 64;

uint64_t pad_now_us(void) {
    FURI_CRITICAL_ENTER();
    uint32_t c = DWT_CYCCNT;
    if(c < clk_last) clk_high += (1ULL << 32);
    clk_last = c;
    uint64_t cyc = clk_high | c;
    FURI_CRITICAL_EXIT();
    return cyc / clk_div;
}

/* ------------------------------------------------------------------ */
/* Descriptors                                                        */
/* ------------------------------------------------------------------ */

/* Pro Controller HID report descriptor (from a real controller, via Controller Ducky) */
static const uint8_t pro_report_desc[] = {
    0x05, 0x01, 0x15, 0x00, 0x09, 0x04, 0xA1, 0x01, 0x85, 0x30, 0x05, 0x01, 0x05, 0x09, 0x19,
    0x01, 0x29, 0x0A, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0A, 0x55, 0x00, 0x65, 0x00,
    0x81, 0x02, 0x05, 0x09, 0x19, 0x0B, 0x29, 0x0E, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95,
    0x04, 0x81, 0x02, 0x75, 0x01, 0x95, 0x02, 0x81, 0x03, 0x0B, 0x01, 0x00, 0x01, 0x00, 0xA1,
    0x00, 0x0B, 0x30, 0x00, 0x01, 0x00, 0x0B, 0x31, 0x00, 0x01, 0x00, 0x0B, 0x32, 0x00, 0x01,
    0x00, 0x0B, 0x35, 0x00, 0x01, 0x00, 0x15, 0x00, 0x27, 0xFF, 0xFF, 0x00, 0x00, 0x75, 0x10,
    0x95, 0x04, 0x81, 0x02, 0xC0, 0x0B, 0x39, 0x00, 0x01, 0x00, 0x15, 0x00, 0x25, 0x07, 0x35,
    0x00, 0x46, 0x3B, 0x01, 0x65, 0x14, 0x75, 0x04, 0x95, 0x01, 0x81, 0x02, 0x05, 0x09, 0x19,
    0x0F, 0x29, 0x12, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x04, 0x81, 0x02, 0x75, 0x08,
    0x95, 0x34, 0x81, 0x03, 0x06, 0x00, 0xFF, 0x85, 0x21, 0x09, 0x01, 0x75, 0x08, 0x95, 0x3F,
    0x81, 0x03, 0x85, 0x81, 0x09, 0x02, 0x75, 0x08, 0x95, 0x3F, 0x81, 0x03, 0x85, 0x01, 0x09,
    0x03, 0x75, 0x08, 0x95, 0x3F, 0x91, 0x83, 0x85, 0x10, 0x09, 0x04, 0x75, 0x08, 0x95, 0x3F,
    0x91, 0x83, 0x85, 0x80, 0x09, 0x05, 0x75, 0x08, 0x95, 0x3F, 0x91, 0x83, 0x85, 0x82, 0x09,
    0x06, 0x75, 0x08, 0x95, 0x3F, 0x91, 0x83, 0xC0,
};

/* HORIPAD S report descriptor (LUFA Switch-Fightstick / wchill SwitchInputEmulator):
 * 14 buttons + 2 pad bits, hat nibble + pad nibble, 4 axes, 1 vendor byte = 8 bytes */
static const uint8_t hori_report_desc[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, /* Generic Desktop, Joystick, Application */
    0x15, 0x00, 0x25, 0x01, 0x35, 0x00, 0x45, 0x01, 0x75, 0x01, 0x95, 0x0E,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x0E, 0x81, 0x02, /* 14 buttons */
    0x95, 0x02, 0x81, 0x01, /* 2 pad bits */
    0x05, 0x01, 0x25, 0x07, 0x46, 0x3B, 0x01, 0x75, 0x04, 0x95, 0x01, 0x65, 0x14,
    0x09, 0x39, 0x81, 0x42, /* hat */
    0x65, 0x00, 0x95, 0x01, 0x81, 0x01, /* pad nibble */
    0x26, 0xFF, 0x00, 0x46, 0xFF, 0x00, 0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35,
    0x75, 0x08, 0x95, 0x04, 0x81, 0x02, /* 4 axes */
    0x75, 0x08, 0x95, 0x01, 0x81, 0x01, /* vendor byte */
    0xC0,
};

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdHID;
    uint8_t bCountryCode;
    uint8_t bNumDescriptors;
    uint8_t bDescriptorType0;
    uint16_t wDescriptorLength0;
} __attribute__((packed)) HidDescriptor;

typedef struct {
    struct usb_config_descriptor config;
    struct usb_interface_descriptor intf;
    HidDescriptor hid;
    struct usb_endpoint_descriptor ep_in;
    struct usb_endpoint_descriptor ep_out;
} __attribute__((packed)) HidConfigDescriptor;

#define HID_CONFIG(report_len, ep_out_addr, attrs)                                         \
    {                                                                                      \
        .config =                                                                          \
            {                                                                              \
                .bLength = sizeof(struct usb_config_descriptor),                           \
                .bDescriptorType = USB_DTYPE_CONFIGURATION,                                \
                .wTotalLength = sizeof(HidConfigDescriptor),                               \
                .bNumInterfaces = 1,                                                       \
                .bConfigurationValue = 1,                                                  \
                .iConfiguration = 0,                                                       \
                .bmAttributes = (attrs),                                                   \
                .bMaxPower = 250,                                                          \
            },                                                                             \
        .intf =                                                                            \
            {                                                                              \
                .bLength = sizeof(struct usb_interface_descriptor),                        \
                .bDescriptorType = USB_DTYPE_INTERFACE,                                    \
                .bInterfaceNumber = 0,                                                     \
                .bAlternateSetting = 0,                                                    \
                .bNumEndpoints = 2,                                                        \
                .bInterfaceClass = USB_CLASS_HID,                                          \
                .bInterfaceSubClass = 0,                                                   \
                .bInterfaceProtocol = 0,                                                   \
                .iInterface = 0,                                                           \
            },                                                                             \
        .hid =                                                                             \
            {                                                                              \
                .bLength = sizeof(HidDescriptor),                                          \
                .bDescriptorType = HID_DTYPE_HID,                                          \
                .bcdHID = 0x0111,                                                          \
                .bCountryCode = 0,                                                         \
                .bNumDescriptors = 1,                                                      \
                .bDescriptorType0 = HID_DTYPE_REPORT,                                      \
                .wDescriptorLength0 = (report_len),                                        \
            },                                                                             \
        .ep_in =                                                                           \
            {                                                                              \
                .bLength = sizeof(struct usb_endpoint_descriptor),                         \
                .bDescriptorType = USB_DTYPE_ENDPOINT,                                     \
                .bEndpointAddress = EP_IN,                                                 \
                .bmAttributes = USB_EPTYPE_INTERRUPT,                                      \
                .wMaxPacketSize = EP_SIZE,                                                 \
                .bInterval = 8,                                                            \
            },                                                                             \
        .ep_out =                                                                          \
            {                                                                              \
                .bLength = sizeof(struct usb_endpoint_descriptor),                         \
                .bDescriptorType = USB_DTYPE_ENDPOINT,                                     \
                .bEndpointAddress = (ep_out_addr),                                         \
                .bmAttributes = USB_EPTYPE_INTERRUPT,                                      \
                .wMaxPacketSize = EP_SIZE,                                                 \
                .bInterval = 8,                                                            \
            },                                                                             \
    }

/* Not const: bInterval is patched before each start */
static HidConfigDescriptor pro_cfg = HID_CONFIG(sizeof(pro_report_desc), PRO_EP_OUT, 0xA0);
static HidConfigDescriptor hori_cfg = HID_CONFIG(sizeof(hori_report_desc), HORI_EP_OUT, 0x80);

static const struct usb_device_descriptor pro_dev = {
    .bLength = sizeof(struct usb_device_descriptor),
    .bDescriptorType = USB_DTYPE_DEVICE,
    .bcdUSB = VERSION_BCD(2, 0, 0),
    .bDeviceClass = USB_CLASS_PER_INTERFACE,
    .bDeviceSubClass = USB_SUBCLASS_NONE,
    .bDeviceProtocol = USB_PROTO_NONE,
    .bMaxPacketSize0 = USB_EP0_SIZE,
    .idVendor = PRO_VID,
    .idProduct = PRO_PID,
    .bcdDevice = VERSION_BCD(2, 0, 0),
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

static const struct usb_device_descriptor hori_dev = {
    .bLength = sizeof(struct usb_device_descriptor),
    .bDescriptorType = USB_DTYPE_DEVICE,
    .bcdUSB = VERSION_BCD(2, 0, 0),
    .bDeviceClass = USB_CLASS_PER_INTERFACE,
    .bDeviceSubClass = USB_SUBCLASS_NONE,
    .bDeviceProtocol = USB_PROTO_NONE,
    .bMaxPacketSize0 = USB_EP0_SIZE,
    .idVendor = HORI_VID,
    .idProduct = HORI_PID,
    .bcdDevice = VERSION_BCD(5, 7, 2),
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 0,
    .bNumConfigurations = 1,
};

static const struct usb_string_descriptor pro_manuf = USB_STRING_DESC("Nintendo Co., Ltd.");
static const struct usb_string_descriptor pro_prod = USB_STRING_DESC("Pro Controller");
static const struct usb_string_descriptor pro_serial = USB_STRING_DESC("000000000001");
static const struct usb_string_descriptor hori_manuf = USB_STRING_DESC("HORI CO.,LTD.");
static const struct usb_string_descriptor hori_prod = USB_STRING_DESC("HORIPAD S");

/* ------------------------------------------------------------------ */
/* Shared state                                                       */
/* ------------------------------------------------------------------ */

static struct {
    usbd_device* dev;
    PadKind kind;
    uint8_t interval;
    bool active;
    FuriHalUsbInterface* prev;
    void (*on_rx)(const PadRx* rx, void* ctx);
    void* rx_ctx;
    PadStateFn state_fn;

    volatile bool configured;
    volatile bool suspended;
    volatile bool streaming;
    volatile uint32_t configured_at;
    PadUsbEvents ev;

    /* IN endpoint: true while a transfer is queued and not yet read */
    volatile bool in_busy;
    /* reply waiting for the next free slot (thread -> ISR) */
    volatile bool reply_pending;
    uint8_t reply[64];
    uint8_t reply_len;

    uint8_t seq; /* Pro report timer byte */

    /* poll statistics, written by the ISR */
    uint64_t last_poll_us;
    uint32_t poll_avg_q4; /* average poll interval, us << 4 */
    PollStats st;
} u;

static void stats_reset_locked(void) {
    memset(&u.st, 0, sizeof(u.st));
    u.st.min_us = UINT32_MAX;
}

/* ------------------------------------------------------------------ */
/* Reports                                                            */
/* ------------------------------------------------------------------ */

static void pack12(uint8_t* o, uint16_t a, uint16_t b) {
    o[0] = a & 0xFF;
    o[1] = ((a >> 8) & 0x0F) | ((b & 0x0F) << 4);
    o[2] = (b >> 4) & 0xFF;
}

static const uint8_t pro_imu_sample[12] = {
    0x00, 0xFC, 0x00, 0x00, 0x00, 0x10, /* accel x / y / z: ~1 g on Z */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* gyro */
};

void pad_pro_fill(uint8_t* r, uint8_t id, const PadState* st) {
    uint32_t b = st->buttons;
    memset(r, 0, 64);
    r[0] = id;
    r[1] = u.seq++;
    r[2] = 0x91; /* battery full, powered over USB */
    r[3] = b & 0xFF;
    r[4] = ((b >> 8) & 0xFF) | 0x80; /* 0x80: wired */
    r[5] = (b >> 16) & 0xFF;
    int mag = (st->lx && st->ly) ? STICK_DIAG : STICK_RANGE;
    pack12(&r[6], STICK_CENTER + st->lx * mag, STICK_CENTER + st->ly * mag);
    int rmag = (st->rx && st->ry) ? STICK_DIAG : STICK_RANGE;
    pack12(&r[9], STICK_CENTER + st->rx * rmag, STICK_CENTER + st->ry * rmag);
    r[12] = 0x0A; /* vibrator input report */
    if(id == 0x30) {
        for(int i = 0; i < 3; i++) memcpy(&r[13 + i * 12], pro_imu_sample, 12);
    }
}

static uint8_t hat(uint32_t b) {
    bool up = b & BTN_UP, dn = b & BTN_DOWN, lf = b & BTN_LEFT, rt = b & BTN_RIGHT;
    if(up && dn) up = dn = false;
    if(lf && rt) lf = rt = false;
    if(up) return rt ? 1 : (lf ? 7 : 0);
    if(dn) return rt ? 3 : (lf ? 5 : 4);
    if(rt) return 2;
    if(lf) return 6;
    return 8;
}

static uint8_t hori_axis(int dir, bool diag) {
    int mag = diag ? 90 : 127;
    return (uint8_t)(128 + dir * mag);
}

static uint8_t hori_fill(uint8_t* r, const PadState* st) {
    uint32_t b = st->buttons;
    uint16_t o = 0;
    if(b & BTN_Y) o |= 0x0001;
    if(b & BTN_B) o |= 0x0002;
    if(b & BTN_A) o |= 0x0004;
    if(b & BTN_X) o |= 0x0008;
    if(b & BTN_L) o |= 0x0010;
    if(b & BTN_R) o |= 0x0020;
    if(b & BTN_ZL) o |= 0x0040;
    if(b & BTN_ZR) o |= 0x0080;
    if(b & BTN_MINUS) o |= 0x0100;
    if(b & BTN_PLUS) o |= 0x0200;
    if(b & BTN_LSTICK) o |= 0x0400;
    if(b & BTN_RSTICK) o |= 0x0800;
    if(b & BTN_HOME) o |= 0x1000;
    if(b & BTN_CAPTURE) o |= 0x2000;
    r[0] = o & 0xFF;
    r[1] = o >> 8;
    r[2] = hat(b);
    bool d = st->lx && st->ly;
    r[3] = hori_axis(st->lx, d);
    r[4] = hori_axis(-st->ly, d); /* HID: up is 0 */
    bool rd = st->rx && st->ry;
    r[5] = hori_axis(st->rx, rd);
    r[6] = hori_axis(-st->ry, rd);
    r[7] = 0;
    return 8;
}

/* Builds the input report for the state at time t. Returns its length. */
static uint8_t build_input(uint8_t* r, uint64_t t) {
    PadState st = {0};
    if(u.state_fn) u.state_fn(t, &st);
    if(u.kind == PadHori) return hori_fill(r, &st);
    pad_pro_fill(r, 0x30, &st);
    return 64;
}

/* Queue the next transfer. Caller holds the critical section or is the ISR
 * (USB interrupt). Returns false if nothing was queued. */
static bool queue_next(uint64_t t) {
    if(!u.dev || !u.configured) return false;
    uint8_t r[64];
    uint8_t len = 0;
    if(u.reply_pending) {
        memcpy(r, u.reply, u.reply_len);
        len = u.reply_len;
        u.reply_pending = false;
    } else if(u.streaming) {
        len = build_input(r, t);
    } else {
        return false;
    }
    if(usbd_ep_write(u.dev, EP_IN, r, len) < 0) {
        u.st.write_fail++;
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* USB callbacks                                                      */
/* ------------------------------------------------------------------ */

static void ep_callback(usbd_device* dev, uint8_t event, uint8_t ep) {
    if(event == usbd_evt_eptx) {
        uint64_t now = pad_now_us();
        if(u.streaming) {
            if(u.last_poll_us) {
                uint64_t d64 = now - u.last_poll_us;
                uint32_t d = d64 > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)d64;
                u.st.polls++;
                u.st.sum_us += d;
                if(d < u.st.min_us) u.st.min_us = d;
                if(d > u.st.max_us) u.st.max_us = d;
                /* running average of normal intervals, for scheduling */
                if(d < 20000) {
                    if(!u.poll_avg_q4)
                        u.poll_avg_q4 = d << 4;
                    else
                        u.poll_avg_q4 = (uint32_t)(
                            (int32_t)u.poll_avg_q4 +
                            ((int32_t)(d << 4) - (int32_t)u.poll_avg_q4) / 16);
                }
                uint32_t bin = d / 500;
                u.st.hist[bin > 16 ? 16 : bin]++;
            }
            u.last_poll_us = now;
        }
        u.in_busy = queue_next(now);
    } else if(event == usbd_evt_eprx) {
        PadRx rx;
        int32_t n = usbd_ep_read(dev, ep, rx.data, sizeof(rx.data));
        u.ev.rx_packets++;
        if(n > 0 && u.on_rx) {
            rx.len = (uint8_t)n;
            u.on_rx(&rx, u.rx_ctx);
        }
    }
}

static usbd_respond ep_config(usbd_device* dev, uint8_t cfg) {
    uint8_t ep_out = u.kind == PadHori ? HORI_EP_OUT : PRO_EP_OUT;
    switch(cfg) {
    case 0:
        u.ev.deconfigured++;
        u.configured = false;
        u.streaming = false;
        u.in_busy = false;
        usbd_ep_deconfig(dev, EP_IN);
        usbd_ep_deconfig(dev, ep_out);
        usbd_reg_endpoint(dev, EP_IN, NULL);
        usbd_reg_endpoint(dev, ep_out, NULL);
        return usbd_ack;
    case 1:
        usbd_ep_config(dev, EP_IN, USB_EPTYPE_INTERRUPT, EP_SIZE);
        usbd_ep_config(dev, ep_out, USB_EPTYPE_INTERRUPT, EP_SIZE);
        usbd_reg_endpoint(dev, EP_IN, ep_callback);
        usbd_reg_endpoint(dev, ep_out, ep_callback);
        u.in_busy = false;
        u.reply_pending = false;
        u.suspended = false;
        u.last_poll_us = 0;
        u.poll_avg_q4 = 0;
        u.configured_at = furi_get_tick();
        u.ev.configured++;
        u.configured = true;
        /* HORI: the host just polls, stream right away. Pro: wait for 0x80 0x04. */
        u.streaming = (u.kind == PadHori);
        if(u.streaming) u.in_busy = queue_next(pad_now_us());
        return usbd_ack;
    default:
        return usbd_fail;
    }
}

static usbd_respond control(usbd_device* dev, usbd_ctlreq* req, usbd_rqc_callback* callback) {
    UNUSED(callback);
    const uint8_t* rd = u.kind == PadHori ? hori_report_desc : pro_report_desc;
    uint16_t rd_len = u.kind == PadHori ? sizeof(hori_report_desc) : sizeof(pro_report_desc);
    HidConfigDescriptor* cfg = u.kind == PadHori ? &hori_cfg : &pro_cfg;

    if(((USB_REQ_RECIPIENT | USB_REQ_TYPE) & req->bmRequestType) ==
           (USB_REQ_INTERFACE | USB_REQ_CLASS) &&
       req->wIndex == 0) {
        switch(req->bRequest) {
        case HID_REQ_SET_REPORT:
        case HID_REQ_SET_IDLE:
        case HID_REQ_SET_PROTOCOL:
            return usbd_ack;
        default:
            return usbd_fail;
        }
    }
    if(((USB_REQ_RECIPIENT | USB_REQ_TYPE) & req->bmRequestType) ==
           (USB_REQ_INTERFACE | USB_REQ_STANDARD) &&
       req->wIndex == 0 && req->bRequest == USB_STD_GET_DESCRIPTOR) {
        switch(req->wValue >> 8) {
        case HID_DTYPE_HID:
            dev->status.data_ptr = (void*)&cfg->hid;
            dev->status.data_count = sizeof(HidDescriptor);
            return usbd_ack;
        case HID_DTYPE_REPORT:
            dev->status.data_ptr = (void*)rd;
            dev->status.data_count = rd_len;
            return usbd_ack;
        default:
            return usbd_fail;
        }
    }
    return usbd_fail;
}

static void usb_init(usbd_device* dev, FuriHalUsbInterface* intf, void* ctx) {
    UNUSED(intf);
    UNUSED(ctx);
    u.dev = dev;
    u.configured = false;
    u.suspended = false;
    u.streaming = false;
    usbd_reg_config(dev, ep_config);
    usbd_reg_control(dev, control);
    usbd_connect(dev, true);
}

static void usb_deinit(usbd_device* dev) {
    u.configured = false;
    u.streaming = false;
    usbd_reg_config(dev, NULL);
    usbd_reg_control(dev, NULL);
    u.dev = NULL;
}

static void usb_wakeup(usbd_device* dev) {
    UNUSED(dev);
    u.suspended = false;
    u.ev.resumed++;
}

static void usb_suspend(usbd_device* dev) {
    UNUSED(dev);
    u.suspended = true;
    u.ev.suspended++;
}

static FuriHalUsbInterface pro_if = {
    .init = usb_init,
    .deinit = usb_deinit,
    .wakeup = usb_wakeup,
    .suspend = usb_suspend,
    .dev_descr = (struct usb_device_descriptor*)&pro_dev,
    .str_manuf_descr = (void*)&pro_manuf,
    .str_prod_descr = (void*)&pro_prod,
    .str_serial_descr = (void*)&pro_serial,
    .cfg_descr = (void*)&pro_cfg,
};

static FuriHalUsbInterface hori_if = {
    .init = usb_init,
    .deinit = usb_deinit,
    .wakeup = usb_wakeup,
    .suspend = usb_suspend,
    .dev_descr = (struct usb_device_descriptor*)&hori_dev,
    .str_manuf_descr = (void*)&hori_manuf,
    .str_prod_descr = (void*)&hori_prod,
    .str_serial_descr = NULL,
    .cfg_descr = (void*)&hori_cfg,
};

/* ------------------------------------------------------------------ */
/* API                                                                */
/* ------------------------------------------------------------------ */

void pad_usb_alloc(void (*on_rx)(const PadRx* rx, void* ctx), void* ctx) {
    memset(&u, 0, sizeof(u));
    u.on_rx = on_rx;
    u.rx_ctx = ctx;
    u.prev = furi_hal_usb_get_config();
    uint32_t ipus = furi_hal_cortex_instructions_per_microsecond();
    clk_div = ipus ? ipus : 64;
    FURI_CRITICAL_ENTER();
    stats_reset_locked();
    FURI_CRITICAL_EXIT();
}

void pad_usb_free(void) {
    pad_usb_stop();
    u.on_rx = NULL;
}

bool pad_usb_start(PadKind kind, uint8_t interval_ms) {
    if(interval_ms < 1) interval_ms = 1;
    /* Leave our current identity first so the host sees a clean replug,
     * even when only the interval changed. */
    if(u.active) {
        furi_hal_usb_set_config(u.prev, NULL);
        furi_delay_ms(400);
    } else {
        furi_hal_usb_unlock();
    }
    u.kind = kind;
    u.interval = interval_ms;
    u.configured = false;
    u.streaming = false;
    HidConfigDescriptor* cfg = kind == PadHori ? &hori_cfg : &pro_cfg;
    cfg->ep_in.bInterval = interval_ms;
    cfg->ep_out.bInterval = interval_ms;
    FURI_CRITICAL_ENTER();
    stats_reset_locked();
    FURI_CRITICAL_EXIT();
    u.active = true;
    if(!furi_hal_usb_set_config(kind == PadHori ? &hori_if : &pro_if, NULL)) {
        FURI_LOG_E(TAG, "USB mode switch refused");
        furi_hal_usb_set_config(u.prev, NULL);
        u.active = false;
        return false;
    }
    return true;
}

void pad_usb_stop(void) {
    if(!u.active) return;
    u.streaming = false;
    furi_hal_usb_set_config(u.prev, NULL);
    u.active = false;
    u.configured = false;
    u.suspended = false;
}

bool pad_usb_active(void) {
    return u.active;
}
PadKind pad_usb_kind(void) {
    return u.kind;
}
uint8_t pad_usb_interval(void) {
    return u.interval;
}
bool pad_usb_configured(void) {
    return u.active && u.configured;
}
bool pad_usb_suspended(void) {
    return u.suspended;
}
bool pad_usb_streaming(void) {
    return u.active && u.configured && u.streaming;
}
uint32_t pad_usb_configured_at_ms(void) {
    return u.configured_at;
}

void pad_usb_set_streaming(bool on) {
    FURI_CRITICAL_ENTER();
    u.streaming = on;
    if(on) {
        u.last_poll_us = 0;
        if(!u.in_busy) u.in_busy = queue_next(pad_now_us());
    }
    FURI_CRITICAL_EXIT();
}

uint32_t pad_usb_poll_period_us(void) {
    return u.poll_avg_q4 >> 4;
}

uint64_t pad_usb_last_poll_us(void) {
    return u.last_poll_us;
}

void pad_usb_set_state_fn(PadStateFn fn) {
    u.state_fn = fn;
}

bool pad_usb_send_reply(const uint8_t* data, uint8_t len) {
    if(len > 64) len = 64;
    /* wait for the previous reply to go out */
    for(int i = 0; i < 50 && u.reply_pending; i++) furi_delay_ms(1);
    bool ok = false;
    FURI_CRITICAL_ENTER();
    if(!u.reply_pending && u.configured) {
        memcpy(u.reply, data, len);
        u.reply_len = len;
        u.reply_pending = true;
        if(!u.in_busy) u.in_busy = queue_next(pad_now_us());
        ok = true;
    }
    FURI_CRITICAL_EXIT();
    return ok;
}

void pad_usb_kick(void) {
    FURI_CRITICAL_ENTER();
    if(u.configured && !u.in_busy && (u.streaming || u.reply_pending)) {
        u.in_busy = queue_next(pad_now_us());
    }
    FURI_CRITICAL_EXIT();
}

void pad_usb_take_stats(PollStats* out) {
    FURI_CRITICAL_ENTER();
    *out = u.st;
    stats_reset_locked();
    FURI_CRITICAL_EXIT();
}

void pad_usb_events(PadUsbEvents* out) {
    FURI_CRITICAL_ENTER();
    *out = u.ev;
    FURI_CRITICAL_EXIT();
}
