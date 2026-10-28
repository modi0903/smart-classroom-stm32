/*
 * extras.h - logic for the v2 extras: card names and roles, teacher
 * sessions with late marking, the event log, knob helpers and the graph
 * history. Like logic.h it touches no hardware, so tests/host_test.c runs
 * all of it on a laptop.
 *
 * Times of day are "sod": seconds since midnight, 0..86399, taken from the RTC.
 */
#ifndef EXTRAS_H
#define EXTRAS_H

#include "logic.h"

#define DAY_S 86400UL

static inline uint32_t sod_of(uint8_t h, uint8_t m, uint8_t s) {
  return (uint32_t)h * 3600UL + (uint32_t)m * 60UL + s;
}
/* seconds from a to b, correct across midnight (sessions are < 24 h) */
static inline uint32_t sod_elapsed(uint32_t a, uint32_t b) {
  return (b + DAY_S - a) % DAY_S;
}

/* ------------------------------------------------------- card directory --
 * Known cards get a name (8 characters shown) and a role. Unknown cards are
 * shown as "C-" plus the last 4 hex digits of the UID, e.g. C-6506.
 */
typedef enum { ROLE_STUDENT = 0, ROLE_TEACHER = 1 } role_t;
typedef struct { uint32_t uid; const char *name; role_t role; } card_t;

static inline int card_find(const card_t *t, int n, uint32_t uid) {
  for (int i = 0; i < n; i++) if (t[i].uid == uid) return i;
  return -1;
}

static inline bool card_is_teacher(const card_t *t, int n, uint32_t uid) {
  int i = card_find(t, n, uid);
  return i >= 0 && t[i].role == ROLE_TEACHER;
}

/* out must hold 9 bytes */
static inline void card_name(const card_t *t, int n, uint32_t uid, char *out) {
  int i = card_find(t, n, uid);
  if (i >= 0) {
    int k = 0;
    for (const char *s = t[i].name; *s && k < 8; s++) out[k++] = *s;
    out[k] = 0;
    return;
  }
  static const char hx[] = "0123456789ABCDEF";
  out[0] = 'C'; out[1] = '-';
  for (int k = 0; k < 4; k++) out[2 + k] = hx[(uid >> (12 - 4 * k)) & 15];
  out[6] = 0;
}

/* ------------------------------------------------------- roster helpers -- */
static inline int roster_find(const roster_t *r, uint32_t uid) {
  for (int i = 0; i < ROSTER_MAX; i++) if (r->e[i].used && r->e[i].uid == uid) return i;
  return -1;
}

/* slot numbers of everyone inside, in slot order; returns how many */
static inline int roster_inside(const roster_t *r, int *slots) {
  int n = 0;
  for (int i = 0; i < ROSTER_MAX; i++) if (r->e[i].used && r->e[i].inside) slots[n++] = i;
  return n;
}

static inline void roster_force_out(roster_t *r, int slot) {
  if (slot >= 0 && slot < ROSTER_MAX) r->e[slot].inside = false;
}

/* --------------------------------------------------------------- knobs --
 * knob_pick: split the 12-bit pot range into n equal steps and return the
 * step the knob is in. To stop flicker at a boundary, the current step is
 * kept until the knob is KNOB_MARGIN counts past the edge.
 */
#define KNOB_MARGIN 60

static inline uint8_t knob_pick(uint16_t adc, uint8_t n, uint8_t cur) {
  if (n <= 1) return 0;
  if (adc > 4095) adc = 4095;
  uint8_t target = (uint8_t)((uint32_t)adc * n / 4096);
  if (cur >= n) return target;
  if (target == cur) return cur;
  int lo = (int)((uint32_t)cur * 4096 / n), hi = (int)((uint32_t)(cur + 1) * 4096 / n);
  if ((int)adc >= lo - KNOB_MARGIN && (int)adc < hi + KNOB_MARGIN) return cur;
  return target;
}

/* Pickup ("soft takeover"). While held, the knob is ignored until it reaches
 * or crosses the value it controls; then it takes over again. This stops the
 * lights jumping when you come back to HOME with the knob somewhere else. */
typedef struct { bool held; int8_t side; } pickup_t;

static inline void pickup_hold(pickup_t *p) { p->held = true; p->side = 0; }

/* returns true when the knob is in control */
static inline bool pickup_update(pickup_t *p, int knob_val, int cur_val, int window) {
  if (!p->held) return true;
  int d = knob_val - cur_val;
  int8_t side = (int8_t)(d > 0 ? 1 : (d < 0 ? -1 : 0));
  if ((d <= window && d >= -window) || (p->side != 0 && side != p->side)) {
    p->held = false;
    return true;
  }
  if (p->side == 0) p->side = side;
  return false;
}

/* ------------------------------------------------------------ event log -- */
typedef enum { EV_IN, EV_LATE, EV_OUT, EV_DENY, EV_FULL,
               EV_START, EV_END, EV_FORCE, EV_RESET } ev_t;

static inline const char *ev_txt(uint8_t e) {
  static const char *t[] = { "IN", "LATE", "OUT", "DENY", "FULL",
                             "START", "END", "FORCE", "RESET" };
  return e <= EV_RESET ? t[e] : "?";
}

#define LOG_MAX 32
typedef struct { uint32_t sod; uint32_t uid; uint8_t type; } event_t;
typedef struct { event_t e[LOG_MAX]; uint8_t head; uint8_t count; } evlog_t;

static inline void log_push(evlog_t *l, uint32_t sod, uint32_t uid, ev_t type) {
  l->e[l->head].sod = sod; l->e[l->head].uid = uid; l->e[l->head].type = (uint8_t)type;
  l->head = (uint8_t)((l->head + 1) % LOG_MAX);
  if (l->count < LOG_MAX) l->count++;
}

/* i = 0 is the newest event */
static inline const event_t *log_get(const evlog_t *l, int i) {
  if (i < 0 || i >= l->count) return 0;
  return &l->e[(l->head + LOG_MAX - 1 - i) % LOG_MAX];
}

/* ------------------------------------------------------- class session --
 * The teacher's tap IN starts a session; the teacher's tap OUT ends it.
 * A student's FIRST entry during a session is marked LATE if it comes more
 * than late_min minutes after the start. Students already inside when the
 * teacher arrives are on time. Leaving and coming back keeps the first mark.
 */
#define ATT_MAX 16
typedef struct { uint32_t uid; uint32_t first_sod; bool late; } att_t;

typedef struct {
  bool     active;
  uint32_t teacher_uid;
  uint32_t start_sod;
  uint8_t  late_min;
  att_t    att[ATT_MAX];
  uint8_t  n_att;
  bool     has_last;                 /* summary of the last finished session */
  uint32_t last_start, last_end;
  uint8_t  last_present, last_late;
} session_t;

static inline void session_init(session_t *s, uint8_t late_min) {
  s->active = false; s->n_att = 0; s->has_last = false; s->late_min = late_min;
}

static inline int session_find(const session_t *s, uint32_t uid) {
  for (int i = 0; i < s->n_att; i++) if (s->att[i].uid == uid) return i;
  return -1;
}

static inline void session_add(session_t *s, uint32_t uid, uint32_t now, bool late) {
  if (s->n_att >= ATT_MAX || session_find(s, uid) >= 0) return;
  s->att[s->n_att].uid = uid; s->att[s->n_att].first_sod = now; s->att[s->n_att].late = late;
  s->n_att++;
}

/* inside[] = UIDs already in the room (teacher excluded by the caller) */
static inline void session_start(session_t *s, uint32_t teacher, uint32_t now,
                                 const uint32_t *inside, int n) {
  s->active = true; s->teacher_uid = teacher; s->start_sod = now; s->n_att = 0;
  for (int i = 0; i < n; i++) session_add(s, inside[i], now, false);
}

static inline uint8_t session_late_count(const session_t *s) {
  uint8_t n = 0;
  for (int i = 0; i < s->n_att; i++) if (s->att[i].late) n++;
  return n;
}

static inline void session_end(session_t *s, uint32_t now) {
  if (!s->active) return;
  s->active = false; s->has_last = true;
  s->last_start = s->start_sod; s->last_end = now;
  s->last_present = s->n_att; s->last_late = session_late_count(s);
}

/* A student tapped IN. Returns EV_LATE only for a late FIRST entry. */
static inline ev_t session_student_in(session_t *s, uint32_t uid, uint32_t now) {
  if (!s->active || session_find(s, uid) >= 0) return EV_IN;
  bool late = sod_elapsed(s->start_sod, now) > (uint32_t)s->late_min * 60UL;
  session_add(s, uid, now, late);
  return late ? EV_LATE : EV_IN;
}

static inline bool session_is_late(const session_t *s, uint32_t uid) {
  int i = session_find(s, uid);
  return i >= 0 && s->att[i].late;
}

/* -------------------------------------------------------------- history --
 * One byte per sample for the graphs. HIST_GAP marks a missing reading.
 */
#define HIST_N   108
#define HIST_GAP 0xFF
typedef struct { uint8_t v[HIST_N]; uint8_t head; uint8_t count; } hist_t;

static inline void hist_push(hist_t *h, uint8_t v) {
  h->v[h->head] = v;
  h->head = (uint8_t)((h->head + 1) % HIST_N);
  if (h->count < HIST_N) h->count++;
}

/* i = 0 is the OLDEST sample still held */
static inline uint8_t hist_get(const hist_t *h, int i) {
  return h->v[(h->head + HIST_N - h->count + i) % HIST_N];
}

/* y range for plotting: data min/max, widened to at least min_span */
static inline bool hist_range(const hist_t *h, uint8_t min_span, uint8_t *lo, uint8_t *hi) {
  int mn = 256, mx = -1;
  for (int i = 0; i < h->count; i++) {
    uint8_t v = hist_get(h, i);
    if (v == HIST_GAP) continue;
    if (v < mn) mn = v;
    if (v > mx) mx = v;
  }
  if (mx < 0) return false;
  while (mx - mn < min_span) { if (mn > 0) mn--; if (mx - mn < min_span) mx++; }
  *lo = (uint8_t)mn; *hi = (uint8_t)mx;
  return true;
}

/* pixel row for value v in a plot h pixels tall: 0 = top (= hi), h-1 = bottom (= lo) */
static inline uint8_t graph_y(uint8_t v, uint8_t lo, uint8_t hi, uint8_t h) {
  if (v <= lo) return (uint8_t)(h - 1);
  if (v >= hi) return 0;
  return (uint8_t)((h - 1) - ((uint32_t)(v - lo) * (h - 1) + (hi - lo) / 2) / (hi - lo));
}

#endif
