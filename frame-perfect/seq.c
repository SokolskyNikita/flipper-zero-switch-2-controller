#include "seq.h"

#include <storage/storage.h>
#include <toolbox/stream/stream.h>
#include <toolbox/stream/file_stream.h>
#include <stdlib.h>
#include <string.h>

const SeqButtonName seq_button_names[] = {
    {"A", BTN_A},          {"B", BTN_B},         {"X", BTN_X},         {"Y", BTN_Y},
    {"L", BTN_L},          {"R", BTN_R},         {"ZL", BTN_ZL},       {"ZR", BTN_ZR},
    {"PLUS", BTN_PLUS},    {"MINUS", BTN_MINUS}, {"HOME", BTN_HOME},   {"CAPTURE", BTN_CAPTURE},
    {"LS", BTN_LSTICK},    {"RS", BTN_RSTICK},   {"UP", BTN_UP},       {"DOWN", BTN_DOWN},
    {"LEFT", BTN_LEFT},    {"RIGHT", BTN_RIGHT},
};
const size_t seq_button_count = sizeof(seq_button_names) / sizeof(seq_button_names[0]);

/* ------------------------------------------------------------------ */
/* Parsing                                                            */
/* ------------------------------------------------------------------ */

static int split(char* buf, char** tok, int max) {
    int n = 0;
    char* p = buf;
    while(*p && n < max) {
        while(*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') *p++ = '\0';
        if(!*p || *p == '#') break;
        tok[n++] = p;
        while(*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != '#') {
            if(*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
            p++;
        }
        if(*p == '#') {
            *p = '\0';
            break;
        }
    }
    return n;
}

static bool parse_uint(const char* s, uint32_t* out) {
    if(!*s) return false;
    uint32_t v = 0;
    for(; *s; s++) {
        if(*s < '0' || *s > '9') return false;
        v = v * 10 + (uint32_t)(*s - '0');
        if(v > 1000000) return false;
    }
    *out = v;
    return true;
}

static bool parse_int(const char* s, int32_t* out) {
    bool neg = false;
    if(*s == '-') {
        neg = true;
        s++;
    }
    uint32_t v;
    if(!parse_uint(s, &v)) return false;
    *out = neg ? -(int32_t)v : (int32_t)v;
    return true;
}

static int find_param(const Sequence* s, const char* name) {
    for(int i = 0; i < s->n_params; i++) {
        if(strcmp(s->params[i].name, name) == 0) return i;
    }
    return -1;
}

static bool parse_input(const char* t, PadState* st) {
    if(strcmp(t, "-") == 0) return true;
    for(size_t i = 0; i < seq_button_count; i++) {
        if(strcmp(t, seq_button_names[i].name) == 0) {
            st->buttons |= seq_button_names[i].mask;
            return true;
        }
    }
    size_t n = strlen(t);
    if(n == 3 && (t[2] == '+' || t[2] == '-')) {
        int8_t v = t[2] == '+' ? 1 : -1;
        if(t[0] == 'L' && t[1] == 'X') {
            st->lx = v;
            return true;
        }
        if(t[0] == 'L' && t[1] == 'Y') {
            st->ly = v;
            return true;
        }
        if(t[0] == 'R' && t[1] == 'X') {
            st->rx = v;
            return true;
        }
        if(t[0] == 'R' && t[1] == 'Y') {
            st->ry = v;
            return true;
        }
    }
    return false;
}

#define FAIL(msg)                                       \
    do {                                                \
        snprintf(err, err_len, "%s", msg);              \
        *err_line = line_no;                            \
        ok = false;                                     \
        goto done;                                      \
    } while(0)

bool seq_load(Sequence* s, const char* path, char* err, size_t err_len, uint16_t* err_line) {
    memset(s, 0, sizeof(*s));
    s->fps = 60;
    err[0] = '\0';
    *err_line = 0;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    Stream* stream = file_stream_alloc(storage);
    FuriString* line = furi_string_alloc();
    bool ok = true;
    uint16_t line_no = 0;

    if(!file_stream_open(stream, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        snprintf(err, err_len, "Cannot open file");
        ok = false;
        goto done;
    }

    char buf[128];
    char* tok[12];
    while(stream_read_line(stream, line)) {
        line_no++;
        snprintf(buf, sizeof(buf), "%s", furi_string_get_cstr(line));
        int n = split(buf, tok, 12);
        if(n == 0) continue;

        if(strcmp(tok[0], "FPS") == 0) {
            uint32_t v;
            if(n < 2 || !parse_uint(tok[1], &v) || v < 1 || v > 240) FAIL("Bad fps");
            s->fps = (uint16_t)v;
        } else if(strcmp(tok[0], "PARAM") == 0) {
            int32_t v, mn = 0, mx = 999;
            if(n < 3 || !parse_int(tok[2], &v)) FAIL("param NAME VALUE");
            if(n >= 5 && (!parse_int(tok[3], &mn) || !parse_int(tok[4], &mx))) FAIL("Bad min/max");
            if(s->n_params >= SEQ_MAX_PARAMS) FAIL("Too many params");
            if(find_param(s, tok[1]) >= 0) FAIL("Duplicate param");
            SeqParam* p = &s->params[s->n_params++];
            snprintf(p->name, sizeof(p->name), "%s", tok[1]);
            p->min = (int16_t)mn;
            p->max = (int16_t)mx;
            p->value = (int16_t)(v < mn ? mn : (v > mx ? mx : v));
        } else if(strcmp(tok[0], "LOOP") == 0) {
            if(s->has_loop) FAIL("Only one loop");
            uint32_t c = 0;
            if(n >= 2 && !parse_uint(tok[1], &c)) FAIL("Bad loop count");
            s->has_loop = true;
            s->loop_count = c;
            s->loop_at = s->n_src;
        } else {
            if(s->n_src >= SEQ_MAX_STEPS) FAIL("Too many steps");
            SeqSrcStep* st = &s->src[s->n_src];
            memset(st, 0, sizeof(*st));
            st->line = line_no;
            const char* d = tok[0];
            size_t dl = strlen(d);
            uint32_t v;
            if(d[0] == '$') {
                int pi = find_param(s, d + 1);
                if(pi < 0) FAIL("Unknown param");
                st->kind = DurParam;
                st->value = (uint16_t)pi;
            } else if(dl > 2 && strcmp(d + dl - 2, "MS") == 0) {
                char tmp[12];
                if(dl - 2 >= sizeof(tmp)) FAIL("Bad duration");
                memcpy(tmp, d, dl - 2);
                tmp[dl - 2] = '\0';
                if(!parse_uint(tmp, &v) || v > 60000) FAIL("Bad duration");
                st->kind = DurMs;
                st->value = (uint16_t)v;
            } else {
                if(!parse_uint(d, &v) || v > 36000) FAIL("Bad duration");
                st->kind = DurFrames;
                st->value = (uint16_t)v;
            }
            for(int i = 1; i < n; i++) {
                if(!parse_input(tok[i], &st->st)) FAIL("Unknown input");
            }
            s->n_src++;
        }
    }
    if(!s->has_loop) s->loop_at = s->n_src;
    if(s->n_src == 0) FAIL("No steps");
    if(s->has_loop && s->loop_at == s->n_src) FAIL("Empty loop");
    seq_compile(s);

done:
    furi_string_free(line);
    stream_free(stream);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

void seq_compile(Sequence* s) {
    /* Frames and milliseconds are summed separately and converted at each
     * step boundary, so rounding never accumulates. */
    for(int section = 0; section < 2; section++) {
        uint8_t from = section == 0 ? 0 : s->loop_at;
        uint8_t to = section == 0 ? s->loop_at : s->n_src;
        uint64_t frames = 0, ms = 0;
        uint32_t end = 0;
        for(uint8_t i = from; i < to; i++) {
            const SeqSrcStep* src = &s->src[i];
            switch(src->kind) {
            case DurFrames:
                frames += src->value;
                break;
            case DurMs:
                ms += src->value;
                break;
            case DurParam: {
                int16_t v = s->params[src->value].value;
                if(v > 0) frames += (uint32_t)v;
                break;
            }
            }
            end = (uint32_t)((frames * 1000000ULL + s->fps / 2) / s->fps + ms * 1000ULL);
            s->steps[i].end_us = end;
            s->steps[i].line = src->line;
            s->steps[i].st = src->st;
        }
        if(section == 0)
            s->init_len_us = end;
        else
            s->loop_len_us = end;
    }
}

void seq_compile_period(Sequence* s, uint32_t period_ns) {
    for(int section = 0; section < 2; section++) {
        uint8_t from = section == 0 ? 0 : s->loop_at;
        uint8_t to = section == 0 ? s->loop_at : s->n_src;
        uint64_t frames = 0, ms = 0;
        uint32_t end = 0;
        for(uint8_t i = from; i < to; i++) {
            const SeqSrcStep* src = &s->src[i];
            if(src->kind == DurFrames)
                frames += src->value;
            else if(src->kind == DurMs)
                ms += src->value;
            else {
                int16_t v = s->params[src->value].value;
                if(v > 0) frames += (uint32_t)v;
            }
            end = (uint32_t)((frames * period_ns + 500) / 1000 + ms * 1000ULL);
            s->steps[i].end_us = end;
            s->steps[i].line = src->line;
            s->steps[i].st = src->st;
        }
        if(section == 0)
            s->init_len_us = end;
        else
            s->loop_len_us = end;
    }
}

uint32_t seq_cycle_us(const Sequence* s) {
    return s->init_len_us + (s->has_loop ? s->loop_len_us : 0);
}

/* ------------------------------------------------------------------ */
/* Playback                                                           */
/* ------------------------------------------------------------------ */

static struct {
    PadState live;
    Sequence* seq; /* non-NULL while playing */
    volatile bool armed; /* start at the next read */
    bool scheduled; /* armed for a fixed start time */
    uint64_t start_at;
    uint32_t lead_us;
    volatile uint64_t started_at;
    uint64_t fm_anchor;
    uint32_t fm_period_ns;
    uint32_t fm_safe_us;
    uint8_t section; /* 0 init, 1 loop */
    uint8_t idx;
    uint64_t start_us;
    uint64_t sec_t0;
    volatile uint32_t cycles;
    volatile uint16_t line;

    PadState last; /* last state handed out, for the edge log */
    bool last_valid;
    SeqEdge edges[SEQ_EDGE_LOG];
    volatile uint8_t n_edges;
    volatile uint32_t total_edges;
} e;

void seq_set_live(const PadState* st) {
    FURI_CRITICAL_ENTER();
    e.live = *st;
    FURI_CRITICAL_EXIT();
}

void seq_play(Sequence* s) {
    FURI_CRITICAL_ENTER();
    e.seq = s;
    e.armed = true;
    e.scheduled = false;
    e.lead_us = 0;
    e.section = 0;
    e.idx = 0;
    e.cycles = 0;
    e.n_edges = 0;
    e.total_edges = 0;
    e.last_valid = false;
    e.line = s->n_src ? s->src[0].line : 0;
    FURI_CRITICAL_EXIT();
}

void seq_play_at(Sequence* s, uint64_t t_start_us, uint32_t lead_us) {
    FURI_CRITICAL_ENTER();
    e.seq = s;
    e.armed = true;
    e.scheduled = true;
    e.start_at = t_start_us;
    e.lead_us = lead_us;
    e.started_at = 0;
    e.section = 0;
    e.idx = 0;
    e.cycles = 0;
    e.n_edges = 0;
    e.total_edges = 0;
    e.last_valid = false;
    e.line = s->n_src ? s->src[0].line : 0;
    FURI_CRITICAL_EXIT();
}

void seq_set_frame_mode(uint64_t anchor_us, uint32_t period_ns, uint32_t safe_us) {
    FURI_CRITICAL_ENTER();
    e.fm_anchor = anchor_us;
    e.fm_period_ns = period_ns;
    e.fm_safe_us = safe_us;
    FURI_CRITICAL_EXIT();
}

uint64_t seq_start_time(void) {
    return e.started_at;
}

void seq_stop(void) {
    FURI_CRITICAL_ENTER();
    e.seq = NULL;
    e.armed = false;
    FURI_CRITICAL_EXIT();
}

bool seq_playing(void) {
    return e.seq != NULL;
}

uint32_t seq_cycles_done(void) {
    return e.cycles;
}

uint16_t seq_current_line(void) {
    return e.line;
}

uint8_t seq_edges(SeqEdge* out, uint8_t max) {
    FURI_CRITICAL_ENTER();
    uint8_t n = e.n_edges < max ? e.n_edges : max;
    memcpy(out, e.edges, n * sizeof(SeqEdge));
    FURI_CRITICAL_EXIT();
    return n;
}

static uint64_t edge_t_read;
static void log_edge(uint64_t t_seq, const PadState* st) {
    uint64_t t = edge_t_read ? edge_t_read : t_seq;
    if(e.last_valid && memcmp(&e.last, st, sizeof(PadState)) == 0) return;
    e.last = *st;
    e.last_valid = true;
    e.total_edges++;
    if(e.n_edges < SEQ_EDGE_LOG) {
        e.edges[e.n_edges].t_us = (uint32_t)(t - e.start_us);
        e.edges[e.n_edges].buttons = st->buttons | ((uint32_t)(st->lx & 3) << 24) |
                                     ((uint32_t)(st->ly & 3) << 26) |
                                     ((uint32_t)(st->rx & 3) << 28) |
                                     ((uint32_t)(st->ry & 3) << 30);
        e.n_edges++;
    }
}

/* Runs in the USB interrupt. */
void seq_state_at(uint64_t t_queue, PadState* out) {
    Sequence* s = e.seq;
    if(!s) {
        *out = e.live;
        return;
    }
    edge_t_read = t_queue + e.lead_us;
    /* the console reads this report one poll later */
    uint64_t t_read = t_queue + e.lead_us;
    uint64_t t = t_read;
    if(e.scheduled && e.fm_period_ns && t_read > e.fm_anchor + e.fm_safe_us) {
        /* frame mode: evaluate the sequence at the start of the frame this
         * read belongs to (+2 us so it is past the rounded step boundary) */
        uint64_t k = ((t_read - e.fm_anchor - e.fm_safe_us) * 1000ULL) / e.fm_period_ns;
        t = e.fm_anchor + (k * e.fm_period_ns + 500) / 1000 + 2;
    }
    if(e.armed) {
        uint64_t t0 = t;
        if(e.scheduled) {
            if(t < e.start_at) {
                *out = e.live;
                return;
            }
            t0 = e.start_at;
        }
        e.armed = false;
        e.started_at = t0;
        e.start_us = t0;
        e.sec_t0 = t0;
        e.section = (s->loop_at == 0) ? 1 : 0;
        e.idx = e.section ? s->loop_at : 0;
    }
    for(int guard = 0; guard < 4; guard++) {
        uint8_t end_idx = e.section == 0 ? s->loop_at : s->n_src;
        uint64_t rel = t - e.sec_t0;
        while(e.idx < end_idx && rel >= s->steps[e.idx].end_us) e.idx++;
        if(e.idx < end_idx) {
            *out = s->steps[e.idx].st;
            e.line = s->steps[e.idx].line;
            log_edge(t, out);
            return;
        }
        /* section finished */
        if(e.section == 0) {
            e.sec_t0 += s->init_len_us;
            if(!s->has_loop) {
                e.cycles = 1;
                break;
            }
            e.section = 1;
            e.idx = s->loop_at;
            continue;
        }
        e.cycles++;
        if(s->loop_count && e.cycles >= s->loop_count) break;
        if(s->loop_len_us == 0) break;
        e.sec_t0 += s->loop_len_us;
        e.idx = s->loop_at;
    }
    /* done: release everything */
    e.seq = NULL;
    memset(out, 0, sizeof(*out));
    log_edge(t, out);
}

uint32_t seq_edge_total(void) {
    return e.total_edges;
}
