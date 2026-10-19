/*
 * logic.h - headcount, pot, and date/time logic, no hardware in it.
 *
 * Everything here is plain C on integers, so the same file is compiled into
 * the firmware AND into tests/host_test.c on a laptop. Time is always passed
 * in (as microseconds from TIM2) so tests can fake it.
 */
#ifndef LOGIC_H
#define LOGIC_H

#include <stdint.h>
#include <stdbool.h>

/* -------------------------------------------------------------- roster --
 * Who is inside the room, by card. A card seen for the first time is
 * "inside". Every later tap flips it. A card seen again within lockout_us
 * of its last sighting is ignored (and the window is refreshed), so a card
 * resting on the reader cannot toggle itself in and out.
 */
#define ROSTER_MAX 8

typedef struct { uint32_t uid; uint32_t last_us; bool used; bool inside; } rost_entry_t;
typedef struct { rost_entry_t e[ROSTER_MAX]; } roster_t;
typedef enum { TAP_IN, TAP_OUT, TAP_IGNORED, TAP_FULL } tap_t;

static inline void roster_clear(roster_t *r) {
  for (int i = 0; i < ROSTER_MAX; i++) r->e[i].used = false;
}

static inline int roster_count(const roster_t *r) {
  int n = 0;
  for (int i = 0; i < ROSTER_MAX; i++) if (r->e[i].used && r->e[i].inside) n++;
  return n;
}

static inline tap_t roster_tap(roster_t *r, uint32_t uid, uint32_t now_us, uint32_t lockout_us) {
  for (int i = 0; i < ROSTER_MAX; i++) {
    if (r->e[i].used && r->e[i].uid == uid) {
      bool locked = (now_us - r->e[i].last_us) < lockout_us;
      r->e[i].last_us = now_us;
      if (locked) return TAP_IGNORED;
      r->e[i].inside = !r->e[i].inside;
      return r->e[i].inside ? TAP_IN : TAP_OUT;
    }
  }
  /* new card: free slot first, else evict the card that left longest ago */
  int slot = -1;
  for (int i = 0; i < ROSTER_MAX; i++) if (!r->e[i].used) { slot = i; break; }
  if (slot < 0) {
    uint32_t oldest = 0;
    for (int i = 0; i < ROSTER_MAX; i++) {
      if (r->e[i].inside) continue;
      uint32_t age = now_us - r->e[i].last_us;
      if (slot < 0 || age > oldest) { slot = i; oldest = age; }
    }
  }
  if (slot < 0) return TAP_FULL;
  r->e[slot].used = true;
  r->e[slot].uid = uid;
  r->e[slot].inside = true;
  r->e[slot].last_us = now_us;
  return TAP_IN;
}

/* Zero-terminated allow list. An empty list ({0}) accepts every card. */
static inline bool uid_allowed(const uint32_t *list, uint32_t uid) {
  if (list[0] == 0) return true;
  for (; *list; list++) if (*list == uid) return true;
  return false;
}

/* ----------------------------------------------------------------- pot --
 * One pot dims both zones. 12-bit ADC -> ON brightness in PWM counts,
 * 50..999 (5..100 %). Never 0: switching off is the button's / mode's job.
 * Changes smaller than POT_STEP counts are ignored so ADC noise does not
 * make the LEDs shimmer.
 */
#define BRIGHT_MIN 50
#define BRIGHT_MAX 999
#define POT_STEP   8

static inline uint16_t pot_brightness(uint16_t adc, uint16_t cur) {
  if (adc > 4095) adc = 4095;
  int target = BRIGHT_MIN + (int)adc * (BRIGHT_MAX - BRIGHT_MIN) / 4095;
  int d = target - (int)cur;
  if (d < 0) d = -d;
  if (d < POT_STEP && target != BRIGHT_MIN && target != BRIGHT_MAX) return cur;
  return (uint16_t)target;
}

/* ------------------------------------------------------------ override --
 * The blue button cycles every LED through AUTO -> ON -> OFF -> AUTO.
 */
typedef enum { OVR_AUTO = 0, OVR_ON = 1, OVR_OFF = 2 } ovr_t;

static inline uint16_t led_duty(ovr_t ovr, int mode, bool occupied, bool dark,
                                uint16_t bright, uint16_t dim) {
  if (ovr == OVR_ON)  return bright;
  if (ovr == OVR_OFF) return 0;
  switch (mode) {
    case 0:  return 0;
    case 1:  return (occupied && dark) ? bright : 0;
    case 2:  return dim < bright ? dim : bright;
    default: return bright;
  }
}

/* ----------------------------------------------------------------- fan --
 * One fan for the room. The knob (fan pot) sets the speed; RFID decides
 * whether anyone is inside. Bottom of the knob's travel = off, after that
 * FAN_MIN_PCT..100 % (a hobby motor stalls below about a third).
 * Knob changes under FAN_KNOB_STEP are ignored so ADC noise doesn't hunt.
 */
#define FAN_MIN_PCT    35
#define FAN_KNOB_OFF   200      /* ADC counts: below this the knob means "off" */
#define FAN_KNOB_STEP  3

static inline uint8_t fan_knob(uint16_t adc, uint8_t cur) {
  if (adc > 4095) adc = 4095;
  if (adc < FAN_KNOB_OFF) return 0;
  int target = FAN_MIN_PCT + (int)(adc - FAN_KNOB_OFF) * (100 - FAN_MIN_PCT) / (4095 - FAN_KNOB_OFF);
  int d = target - (int)cur;
  if (d < 0) d = -d;
  if (cur != 0 && d < FAN_KNOB_STEP && target != FAN_MIN_PCT && target != 100) return cur;
  return (uint8_t)target;
}

/* Button override first (ON runs it even with nobody inside, OFF stops it),
 * otherwise: someone tapped in -> knob speed, nobody inside -> off. */
static inline uint8_t fan_cmd(ovr_t ovr, bool someone_inside, uint8_t knob) {
  if (ovr == OVR_OFF) return 0;
  if (ovr == OVR_ON)  return knob;
  return someone_inside ? knob : 0;
}

/* A stopped motor will not start on 35 %. When the command goes from 0 to
 * something, give it full allowed speed for FAN_KICK_US first. */
#define FAN_KICK_US 300000UL

typedef struct { uint8_t prev; bool kicking; uint32_t t0; } fan_t;

static inline uint8_t fan_apply(fan_t *f, uint8_t pct, uint32_t now_us) {
  if (pct == 0) { f->prev = 0; f->kicking = false; return 0; }
  if (f->prev == 0) { f->kicking = true; f->t0 = now_us; }
  f->prev = pct;
  if (f->kicking) {
    if (now_us - f->t0 < FAN_KICK_US) return 100;
    f->kicking = false;
  }
  return pct;
}

/* Percent -> timer compare value. max_pct caps the average motor voltage:
 * 9 V battery x 60 % ~ 5.4 V for 3-6 V hobby motors. ARR 799: 800 = always on. */
static inline uint16_t fan_ccr(uint8_t pct, uint8_t max_pct) {
  if (pct > 100) pct = 100;
  return (uint16_t)((uint32_t)pct * max_pct * 8 / 100);
}

/* ----------------------------------------------------------- date/time --
 */
typedef struct { uint8_t h, m, s, d, mo, y; } dt_t;   /* y = years since 2000 */

static inline uint8_t bcd(uint8_t v)   { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static inline uint8_t unbcd(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 15)); }

static inline int days_in_month(int mo, int year) {
  static const uint8_t dm[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (mo == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) return 29;
  return dm[mo - 1];
}

/* 1 = Monday .. 7 = Sunday, the RTC's convention (Sakamoto's method). */
static inline uint8_t weekday(int d, int mo, int year) {
  static const uint8_t t[12] = {0,3,2,5,0,3,5,1,4,6,2,4};
  if (mo < 3) year -= 1;
  int w = (year + year / 4 - year / 100 + year / 400 + t[mo - 1] + d) % 7;   /* 0 = Sunday */
  return (uint8_t)(w == 0 ? 7 : w);
}

static inline bool dt_valid(const dt_t *t) {
  return t->h < 24 && t->m < 60 && t->s < 60 && t->y < 100 &&
         t->mo >= 1 && t->mo <= 12 && t->d >= 1 && t->d <= days_in_month(t->mo, 2000 + t->y);
}

/* reads 1-2 digits, returns pointer past them or 0 */
static inline const char *read_num(const char *p, int *out, int maxdig) {
  int v = 0, n = 0;
  while (*p >= '0' && *p <= '9' && n < maxdig) { v = v * 10 + (*p - '0'); p++; n++; }
  if (!n) return 0;
  *out = v; return p;
}

/* "HH:MM" or "HH:MM:SS" */
static inline bool parse_hms(const char *p, dt_t *t) {
  int h, m, s = 0;
  if (!(p = read_num(p, &h, 2)) || *p++ != ':') return false;
  if (!(p = read_num(p, &m, 2))) return false;
  if (*p == ':') { if (!(p = read_num(p + 1, &s, 2))) return false; }
  if (*p != 0 || h > 23 || m > 59 || s > 59) return false;
  t->h = (uint8_t)h; t->m = (uint8_t)m; t->s = (uint8_t)s;
  return true;
}

/* "DD/MM/YY" or "DD/MM/YYYY" ('-' also accepted) */
static inline bool parse_dmy(const char *p, dt_t *t) {
  int d, mo, y;
  if (!(p = read_num(p, &d, 2)) || (*p != '/' && *p != '-')) return false;
  if (!(p = read_num(p + 1, &mo, 2)) || (*p != '/' && *p != '-')) return false;
  if (!(p = read_num(p + 1, &y, 4)) || *p != 0) return false;
  if (y >= 2000) y -= 2000;
  if (y < 0 || y > 99 || mo < 1 || mo > 12 || d < 1 || d > days_in_month(mo, 2000 + y)) return false;
  t->d = (uint8_t)d; t->mo = (uint8_t)mo; t->y = (uint8_t)y;
  return true;
}

/* The compiler's __DATE__ ("Oct 18 2026") and __TIME__ ("08:31:05"). */
static inline bool parse_build(const char *date, const char *time, dt_t *t) {
  static const char mon[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  int mo = 0;
  for (int i = 0; i < 12; i++)
    if (date[0] == mon[i*3] && date[1] == mon[i*3+1] && date[2] == mon[i*3+2]) mo = i + 1;
  if (!mo) return false;
  int d = (date[4] == ' ' ? 0 : date[4] - '0') * 10 + (date[5] - '0');
  int y = (date[9] - '0') * 10 + (date[10] - '0');
  t->d = (uint8_t)d; t->mo = (uint8_t)mo; t->y = (uint8_t)y;
  t->h = (uint8_t)((time[0] - '0') * 10 + time[1] - '0');
  t->m = (uint8_t)((time[3] - '0') * 10 + time[4] - '0');
  t->s = (uint8_t)((time[6] - '0') * 10 + time[7] - '0');
  return dt_valid(t);
}

/* FNV-1a, used to tell one build from the next */
static inline uint32_t fnv1a(const char *a, const char *b) {
  uint32_t h = 2166136261UL;
  for (; *a; a++) h = (h ^ (uint8_t)*a) * 16777619UL;
  for (; *b; b++) h = (h ^ (uint8_t)*b) * 16777619UL;
  return h | 1;                                  /* never 0: 0 means "backup domain empty" */
}

#endif
