/*
 * USB side of Frame Perfect: descriptors, the console's polling, and the
 * reports we hand it.
 *
 * Derived from Controller Ducky (https://github.com/eric-m-holub/controller-ducky,
 * Apache-2.0, commit d63fbbe): the Pro Controller descriptors, the fake SPI
 * flash and the 0x30 / 0x21 report layout come from there. Changed for this
 * project: reports are written from the USB interrupt the moment the console
 * reads the previous one, the poll interval is selectable, every poll is
 * timestamped, and a HORIPAD-style identity was added.
 */
#pragma once

#include <furi.h>
#include <stdbool.h>
#include <stdint.h>

/* Button bits. Pro Controller 0x30 layout: byte3 | byte4 << 8 | byte5 << 16 */
#define BTN_Y       0x000001u
#define BTN_X       0x000002u
#define BTN_B       0x000004u
#define BTN_A       0x000008u
#define BTN_R       0x000040u
#define BTN_ZR      0x000080u
#define BTN_MINUS   0x000100u
#define BTN_PLUS    0x000200u
#define BTN_RSTICK  0x000400u
#define BTN_LSTICK  0x000800u
#define BTN_HOME    0x001000u
#define BTN_CAPTURE 0x002000u
#define BTN_DOWN    0x010000u
#define BTN_UP      0x020000u
#define BTN_RIGHT   0x040000u
#define BTN_LEFT    0x080000u
#define BTN_L       0x400000u
#define BTN_ZL      0x800000u

typedef enum {
    PadPro, /* Switch Pro Controller 057E:2009, needs the 0x80 handshake */
    PadHori, /* HORIPAD S 0F0D:00C1, plain HID, no handshake */
    PadKindCount,
} PadKind;

/* What the controller sends. Sticks are -1, 0 or +1 per axis (full tilt). */
typedef struct {
    uint32_t buttons;
    int8_t lx, ly, rx, ry; /* +x = right, +y = up */
} PadState;

/* Called from the USB interrupt each time a report is about to be written.
 * t_us is the time of the console's read that just completed. Must be quick. */
typedef void (*PadStateFn)(uint64_t t_us, PadState* out);

typedef struct {
    uint32_t polls; /* completed IN transfers (= console reads) */
    uint32_t sum_us; /* sum of the intervals between them */
    uint32_t min_us;
    uint32_t max_us;
    uint32_t hist[17]; /* interval in 0.5 ms bins, last bin = 8 ms and more */
    uint32_t write_fail; /* usbd_ep_write rejected a report */
} PollStats;

typedef struct {
    uint8_t data[64];
    uint8_t len;
} PadRx;

/* Microsecond clock (DWT cycle counter, extended to 64 bits). */
uint64_t pad_now_us(void);

/* on_rx runs in the USB interrupt with each OUT report from the console. */
void pad_usb_alloc(void (*on_rx)(const PadRx* rx, void* ctx), void* ctx);
void pad_usb_free(void);

/* Switch the Flipper's USB port to the given identity. interval_ms goes into
 * both interrupt endpoint descriptors. Returns false if the HAL refused. */
bool pad_usb_start(PadKind kind, uint8_t interval_ms);
/* Back to the Flipper's own USB mode (CLI). */
void pad_usb_stop(void);

bool pad_usb_active(void); /* we own the USB port */
PadKind pad_usb_kind(void);
uint8_t pad_usb_interval(void);
bool pad_usb_configured(void);
bool pad_usb_suspended(void);
bool pad_usb_streaming(void);
void pad_usb_set_streaming(bool on); /* Pro: after 0x80 0x04 */
uint32_t pad_usb_configured_at_ms(void); /* tick when the host configured us */

void pad_usb_set_state_fn(PadStateFn fn);
/* Average interval between the console's reads (0 until measured), and the
 * time of the last read, on the pad_now_us() clock. */
uint32_t pad_usb_poll_period_us(void);
uint64_t pad_usb_last_poll_us(void);

/* Queue a reply (Pro 0x81 / 0x21 reports). Thread context only. */
bool pad_usb_send_reply(const uint8_t* data, uint8_t len);

/* Restart the report chain if it stopped (no transfer in flight). */
void pad_usb_kick(void);

/* USB events counted in the interrupt, for the log */
typedef struct {
    uint32_t configured; /* host set configuration 1 */
    uint32_t deconfigured; /* configuration 0 / bus reset */
    uint32_t suspended;
    uint32_t resumed;
    uint32_t rx_packets;
} PadUsbEvents;
void pad_usb_events(PadUsbEvents* out);

/* Copy and reset the poll statistics. */
void pad_usb_take_stats(PollStats* out);

/* Fill a 64-byte Pro Controller report (id 0x21 or 0x30) from a state. */
void pad_pro_fill(uint8_t* r, uint8_t id, const PadState* st);
