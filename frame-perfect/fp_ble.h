/*
 * Frame Perfect's own Bluetooth link to the Mac.
 *
 * While active, the Flipper runs a copy of its stock Bluetooth serial profile
 * (same name, address and services, so an existing Mac pairing still works)
 * that asks for a 15 ms connection interval instead of the stock 7.5-45 ms
 * range, and delivers the data to Frame Perfect instead of the RPC service.
 *
 * Sync messages (all little-endian):
 *   Mac -> Flipper  'S' u32 seq                              ping
 *   Flipper -> Mac  'R' u32 seq u64 t_rx_us u64 t_tx_us     t_* on pad_now_us()
 *
 * Also registers the CLI command `fp_clock` (USB): after it starts, every byte
 * received is answered at once with "c <t_us>\n", t on the same clock. It is
 * the reference the Bluetooth sync is checked against. 'q' ends it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* hid: add an HID service (mouse appearance) so macOS gives the link a
 * shorter interval; uses its own address and key file. */
bool fp_ble_start(bool hid);
void fp_ble_stop(void);

/* Command channel (HID mode). The Mac writes messages that start with an
 * upper-case letter ('S' pings are answered inside fp_ble); the app takes
 * them here, with the receive time on the pad_now_us() clock. */
#define FP_MSG_MAX 31 /* Flipper -> Mac */
#define FP_CMD_MAX 100 /* Mac -> Flipper */
typedef struct {
    uint64_t t_rx;
    uint8_t len;
    uint8_t d[FP_CMD_MAX];
} FpMsg;
bool fp_ble_take_cmd(FpMsg* m);
/* Queue a message for the Mac (sent from fp_ble_poll). false if full. */
bool fp_ble_send(const void* data, uint8_t len);
bool fp_ble_connected(void);
/* Answer pending pings. Call often from the app's main loop. */
void fp_ble_poll(void);
uint32_t fp_ble_pings(void);
#include <furi.h>
void fp_ble_stats(FuriString* out);
/* Send a 1-byte notification when nothing was sent for this long (0 = off). */
void fp_ble_set_keepalive_ms(uint32_t ms);
/* After each connect, tell the radio to ignore peripheral latency (listen on
 * every connection event). Uses an unexported radio command; see fp_ble.c. */
void fp_ble_set_no_latency(bool on);
/* Test: drop pings without answering (Mac->Flipper load only). */
void fp_ble_set_no_reply(bool on);
/* Ask for this PHY (1 = 1M, 2 = 2M) once the first write shows the connection handle. */
void fp_ble_set_phy(uint8_t phy);
/* -2 not sent yet, -1 sender not found, 0 ok, else the radio's error */
int fp_ble_latency_status(void);

void fp_clock_cli_register(void);
void fp_clock_cli_unregister(void);
