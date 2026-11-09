/*
 * Frame sequences: parse a text file into timed controller states and play
 * them back with microsecond timing from the USB interrupt.
 *
 * File format (one item per line, '#' starts a comment):
 *
 *   fps 60                     frames per second (default 60)
 *   param NAME VALUE [MIN MAX] a value the Flipper UI can nudge
 *   DUR [INPUT ...]            hold this whole controller state for DUR
 *   loop [N]                   the steps after this repeat N times (0 or no N:
 *                              until stopped)
 *
 * DUR is a number of frames (2), milliseconds (40ms), or a parameter ($wait).
 * INPUT is a button (A B X Y L R ZL ZR PLUS MINUS HOME CAPTURE LS RS
 * UP DOWN LEFT RIGHT) or a full stick tilt (LX+ LX- LY+ LY- RX+ RX- RY+ RY-).
 * Each line is the complete state: anything not listed is released.
 */
#pragma once

#include "pad_usb.h"

#define SEQ_MAX_STEPS 96
#define SEQ_MAX_PARAMS 12
#define SEQ_NAME_LEN 16

typedef enum {
    DurFrames,
    DurMs,
    DurParam,
} DurKind;

typedef struct {
    DurKind kind;
    uint16_t value; /* frames or ms, or the param index */
    uint16_t line; /* source line, for the UI */
    PadState st;
} SeqSrcStep;

typedef struct {
    char name[SEQ_NAME_LEN];
    int16_t value;
    int16_t min;
    int16_t max;
} SeqParam;

typedef struct {
    uint32_t end_us; /* cumulative end within its section */
    uint16_t line;
    PadState st;
} SeqStep;

typedef struct {
    /* parsed source */
    uint16_t fps;
    SeqSrcStep src[SEQ_MAX_STEPS];
    uint8_t n_src;
    uint8_t loop_at; /* index of the first looped step; == n_src when there is no loop */
    bool has_loop;
    uint32_t loop_count; /* 0 = forever */
    SeqParam params[SEQ_MAX_PARAMS];
    uint8_t n_params;

    /* compiled */
    SeqStep steps[SEQ_MAX_STEPS];
    uint32_t init_len_us;
    uint32_t loop_len_us;
} Sequence;

/* Parse a file. On failure returns false with a message and line number. */
bool seq_load(Sequence* s, const char* path, char* err, size_t err_len, uint16_t* err_line);
/* Recompute step times after a parameter change. */
void seq_compile(Sequence* s);
/* Total time of one run (init + one loop cycle), in microseconds. */
uint32_t seq_cycle_us(const Sequence* s);

/* ---- playback (state shared with the USB interrupt) ---- */

/* Live state used when no sequence plays. Thread writes, ISR reads. */
void seq_set_live(const PadState* st);

/* Start playing s at the console's next read. s must stay untouched until
 * seq_stop() or until playback ends. */
void seq_play(Sequence* s);

/* Frame-exact step times for a measured frame period (ns), instead of the
 * nominal fps. Milliseconds stay milliseconds. */
void seq_compile_period(Sequence* s, uint32_t period_ns);

/* Scheduled start: the console sees step 1 from t_start_us (pad_now_us
 * clock) on, i.e. from its first read at or after that time. Until then the
 * live state is sent. Step times are evaluated for the moment the console
 * will read the report, lead_us after it is queued (one poll period). */
void seq_play_at(Sequence* s, uint64_t t_start_us, uint32_t lead_us);
/* Frame mode for scheduled runs: each console read is mapped to a frame of
 * the grid (anchor_us + k * period_ns). A read at least safe_us into frame k
 * carries frame k's state; an earlier read still carries frame k-1's. With
 * reads every 8 ms and safe_us ~6000, every change reaches the console
 * 6-14 ms into its frame. Call before seq_play_at; period_ns 0 = off. */
void seq_set_frame_mode(uint64_t anchor_us, uint32_t period_ns, uint32_t safe_us);
/* Start time of the current/last scheduled run (0 if none) */
uint64_t seq_start_time(void);
void seq_stop(void);
bool seq_playing(void);
uint32_t seq_cycles_done(void);
uint16_t seq_current_line(void);

/* PadStateFn for pad_usb_set_state_fn() */
void seq_state_at(uint64_t t_us, PadState* out);

/* Edge log: the time (relative to start) of each state change as it was
 * handed to the console. Lets a test check the real durations. */
#define SEQ_EDGE_LOG 64
typedef struct {
    uint32_t t_us;
    uint32_t buttons;
} SeqEdge;
uint8_t seq_edges(SeqEdge* out, uint8_t max);
/* all state changes in the current/last run, including ones past the log */
uint32_t seq_edge_total(void);

/* Button name table, shared with the UI */
typedef struct {
    const char* name;
    uint32_t mask;
} SeqButtonName;
extern const SeqButtonName seq_button_names[];
extern const size_t seq_button_count;
