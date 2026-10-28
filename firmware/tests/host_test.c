/* Host-side tests for Core/Inc/logic.h and Core/Inc/extras.h. No board needed:
 *   gcc -Wall -Wextra -ICore/Inc tests/host_test.c -o host_test && ./host_test
 */
#include <stdio.h>
#include <string.h>
#include "logic.h"
#include "extras.h"

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static void test_roster(void) {
  roster_t r; roster_clear(&r);
  const uint32_t LOCK = 3000000UL;
  uint32_t t = 10000000UL;
  CHECK(roster_count(&r) == 0);
  CHECK(roster_tap(&r, 0xAAAA0001, t, LOCK) == TAP_IN);
  CHECK(roster_count(&r) == 1);
  CHECK(roster_tap(&r, 0xAAAA0001, t + 1000000, LOCK) == TAP_IGNORED);   /* still on the reader */
  CHECK(roster_count(&r) == 1);
  /* lockout refreshes while the card keeps being seen */
  CHECK(roster_tap(&r, 0xAAAA0001, t + 3500000, LOCK) == TAP_IGNORED);
  CHECK(roster_tap(&r, 0xAAAA0001, t + 6000000, LOCK) == TAP_IGNORED);
  /* gone for > lockout, then tapped again: that is an exit */
  CHECK(roster_tap(&r, 0xAAAA0001, t + 10000000, LOCK) == TAP_OUT);
  CHECK(roster_count(&r) == 0);
  CHECK(roster_tap(&r, 0xAAAA0001, t + 20000000, LOCK) == TAP_IN);
  CHECK(roster_count(&r) == 1);
  /* two people */
  CHECK(roster_tap(&r, 0xBBBB0002, t + 21000000, LOCK) == TAP_IN);
  CHECK(roster_count(&r) == 2);
  roster_clear(&r);
  CHECK(roster_count(&r) == 0);
  /* a cleared card is "new" again, so its next tap is an entry */
  CHECK(roster_tap(&r, 0xAAAA0001, t + 30000000, LOCK) == TAP_IN);
}

static void test_roster_full(void) {
  roster_t r; roster_clear(&r);
  const uint32_t LOCK = 3000000UL;
  uint32_t t = 1000000UL;
  for (int i = 0; i < ROSTER_MAX; i++) CHECK(roster_tap(&r, 0x1000 + i, t + i * 10000000UL, LOCK) == TAP_IN);
  CHECK(roster_count(&r) == ROSTER_MAX);
  t += 200000000UL;
  CHECK(roster_tap(&r, 0x9999, t, LOCK) == TAP_FULL);              /* everyone inside, no slot */
  CHECK(roster_tap(&r, 0x1003, t + 5000000UL, LOCK) == TAP_OUT);   /* one leaves */
  CHECK(roster_count(&r) == ROSTER_MAX - 1);
  CHECK(roster_tap(&r, 0x9999, t + 10000000UL, LOCK) == TAP_IN);   /* newcomer takes the freed slot */
  CHECK(roster_count(&r) == ROSTER_MAX);
}

static void test_roster_wrap(void) {
  roster_t r; roster_clear(&r);
  const uint32_t LOCK = 3000000UL;
  uint32_t t = 0xFFFFFFFFUL - 1000000UL;                           /* 1 s before wrap */
  CHECK(roster_tap(&r, 0x42, t, LOCK) == TAP_IN);
  CHECK(roster_tap(&r, 0x42, t + 2000000UL, LOCK) == TAP_IGNORED); /* 2 s later, counter wrapped */
  CHECK(roster_tap(&r, 0x42, t + 2000000UL + 4000000UL, LOCK) == TAP_OUT);
}

static void test_allowed(void) {
  static const uint32_t any[]  = { 0 };
  static const uint32_t two[]  = { 0xDEADBEEF, 0x01020304, 0 };
  CHECK(uid_allowed(any, 0x1234));
  CHECK(uid_allowed(two, 0xDEADBEEF));
  CHECK(uid_allowed(two, 0x01020304));
  CHECK(!uid_allowed(two, 0x1234));
}

static void test_pot(void) {
  CHECK(pot_brightness(0, 500) == BRIGHT_MIN);
  CHECK(pot_brightness(4095, 500) == BRIGHT_MAX);
  CHECK(pot_brightness(5000, 500) == BRIGHT_MAX);           /* clamp */
  uint16_t mid = pot_brightness(2048, 0);
  CHECK(mid > 500 && mid < 550);
  CHECK(pot_brightness(2050, mid) == mid);                  /* noise ignored */
  CHECK(pot_brightness(2200, mid) != mid);                  /* real turn followed */
  uint16_t b = BRIGHT_MIN;                                  /* sweep is monotonic */
  for (int a = 0; a <= 4095; a += 16) { uint16_t n = pot_brightness((uint16_t)a, b); CHECK(n >= b); b = n; }
  CHECK(pot_brightness(4095, b) == BRIGHT_MAX);           /* end stop always reached */
}

static void test_led_duty(void) {
  CHECK(led_duty(OVR_ON, 0, false, false, 700, 100) == 700);  /* override beats mode OFF */
  CHECK(led_duty(OVR_OFF, 3, true, true, 700, 100) == 0);     /* override beats MAX */
  CHECK(led_duty(OVR_AUTO, 0, true, true, 700, 100) == 0);
  CHECK(led_duty(OVR_AUTO, 1, true, true, 700, 100) == 700);
  CHECK(led_duty(OVR_AUTO, 1, true, false, 700, 100) == 0);   /* bright room */
  CHECK(led_duty(OVR_AUTO, 1, false, true, 700, 100) == 0);   /* empty zone */
  CHECK(led_duty(OVR_AUTO, 2, false, false, 700, 100) == 100);
  CHECK(led_duty(OVR_AUTO, 2, false, false, 60, 100) == 60);  /* DIM never brighter than the pot */
  CHECK(led_duty(OVR_AUTO, 3, false, false, 700, 100) == 700);
}

static void test_datetime(void) {
  for (int v = 0; v < 100; v++) CHECK(unbcd(bcd((uint8_t)v)) == v);
  CHECK(bcd(59) == 0x59);
  CHECK(days_in_month(2, 2024) == 29 && days_in_month(2, 2026) == 28 && days_in_month(2, 2000) == 29);
  CHECK(weekday(18, 10, 2026) == 7);        /* Sunday */
  CHECK(weekday(1, 1, 2024) == 1);          /* Monday */
  CHECK(weekday(29, 2, 2024) == 4);         /* Thursday */

  dt_t t = {0};
  CHECK(parse_hms("08:30", &t) && t.h == 8 && t.m == 30 && t.s == 0);
  CHECK(parse_hms("23:59:58", &t) && t.h == 23 && t.m == 59 && t.s == 58);
  CHECK(parse_hms("8:05:07", &t) && t.h == 8 && t.m == 5 && t.s == 7);
  CHECK(!parse_hms("24:00", &t));
  CHECK(!parse_hms("12:60", &t));
  CHECK(!parse_hms("12", &t));
  CHECK(!parse_hms("12:30x", &t));
  CHECK(!parse_hms("", &t));

  CHECK(parse_dmy("18/10/26", &t) && t.d == 18 && t.mo == 10 && t.y == 26);
  CHECK(parse_dmy("1-1-2027", &t) && t.d == 1 && t.mo == 1 && t.y == 27);
  CHECK(parse_dmy("29/02/24", &t));
  CHECK(!parse_dmy("29/02/26", &t));
  CHECK(!parse_dmy("31/04/26", &t));
  CHECK(!parse_dmy("18/13/26", &t));
  CHECK(!parse_dmy("18/10", &t));

  CHECK(parse_build("Oct 18 2026", "08:31:05", &t) && t.d == 18 && t.mo == 10 && t.y == 26 &&
        t.h == 8 && t.m == 31 && t.s == 5);
  CHECK(parse_build("Feb  3 2027", "23:00:59", &t) && t.d == 3 && t.mo == 2 && t.y == 27);
  CHECK(!parse_build("Xyz 18 2026", "08:31:05", &t));
  CHECK(parse_build(__DATE__, __TIME__, &t));                 /* this compiler's own format */

  CHECK(fnv1a("a", "b") != fnv1a("a", "c"));
  CHECK(fnv1a("", "") != 0);
}

static void test_fan(void) {
  CHECK(fan_knob(0, 50) == 0);                                /* knob at the bottom = off */
  CHECK(fan_knob(FAN_KNOB_OFF - 1, 50) == 0);
  CHECK(fan_knob(FAN_KNOB_OFF, 0) == FAN_MIN_PCT);            /* first click on = minimum that spins */
  CHECK(fan_knob(4095, 50) == 100);
  uint8_t k = fan_knob(2048, 0);
  CHECK(k > FAN_MIN_PCT && k < 100);
  CHECK(fan_knob(2060, k) == k);                              /* noise ignored */
  CHECK(fan_knob(2600, k) > k);                               /* real turn followed */
  uint8_t prev = 0; int up = 1;                               /* sweep never goes backwards */
  for (int a = 0; a <= 4095; a += 32) { uint8_t n = fan_knob((uint16_t)a, prev); if (n < prev) up = 0; prev = n; }
  CHECK(up);

  CHECK(fan_cmd(OVR_AUTO, false, 80) == 0);                   /* nobody tapped in */
  CHECK(fan_cmd(OVR_AUTO, true, 80) == 80);                   /* someone inside: knob speed */
  CHECK(fan_cmd(OVR_AUTO, true, 0) == 0);                     /* knob off wins */
  CHECK(fan_cmd(OVR_ON, false, 60) == 60);                    /* override ON: knob speed, nobody needed */
  CHECK(fan_cmd(OVR_OFF, true, 100) == 0);

  fan_t f = {0, false, 0};
  CHECK(fan_apply(&f, 40, 1000) == 100);                      /* kick */
  CHECK(fan_apply(&f, 40, 1000 + FAN_KICK_US) == 40);
  CHECK(fan_apply(&f, 60, 9000000) == 60);                    /* no re-kick while running */
  CHECK(fan_apply(&f, 0, 9100000) == 0);
  CHECK(fan_apply(&f, 40, 9200000) == 100);                   /* re-kick after stop */
  fan_t g = {0, false, 0}; uint32_t t0 = 0xFFFFFFFFUL - 100000UL;
  CHECK(fan_apply(&g, 50, t0) == 100);
  CHECK(fan_apply(&g, 50, t0 + 250000UL) == 100);             /* timer wrapped, still kicking */
  CHECK(fan_apply(&g, 50, t0 + 350000UL) == 50);

  CHECK(fan_ccr(0, 60) == 0);
  CHECK(fan_ccr(100, 60) == 480);                             /* 60 % of 800 */
  CHECK(fan_ccr(100, 100) == 800);                            /* = ARR+1: fully on */
  CHECK(fan_ccr(200, 100) == 800);                            /* clamp */
  CHECK(fan_ccr(35, 60) == 168);
}

/* ============================ v2 features ============================ */
static const card_t CARDS_T[] = {
  { 0x62E26506, "TEACHER", ROLE_TEACHER },
  { 0x11112222, "RAHULKUMARX", ROLE_STUDENT },      /* longer than 8: cut */
};
#define NC (int)(sizeof CARDS_T / sizeof CARDS_T[0])

static void test_cards(void) {
  char n[9];
  CHECK(card_find(CARDS_T, NC, 0x62E26506) == 0);
  CHECK(card_find(CARDS_T, NC, 0xDEADBEEF) == -1);
  CHECK(card_is_teacher(CARDS_T, NC, 0x62E26506));
  CHECK(!card_is_teacher(CARDS_T, NC, 0x11112222));
  CHECK(!card_is_teacher(CARDS_T, NC, 0xDEADBEEF));
  card_name(CARDS_T, NC, 0x62E26506, n); CHECK(strcmp(n, "TEACHER") == 0);
  card_name(CARDS_T, NC, 0x11112222, n); CHECK(strcmp(n, "RAHULKUM") == 0);
  card_name(CARDS_T, NC, 0xDEADBEEF, n); CHECK(strcmp(n, "C-BEEF") == 0);
  card_name(CARDS_T, NC, 0x0000000A, n); CHECK(strcmp(n, "C-000A") == 0);
}

static void test_roster_helpers(void) {
  roster_t r; roster_clear(&r);
  int slots[ROSTER_MAX];
  CHECK(roster_inside(&r, slots) == 0);
  roster_tap(&r, 0xA, 0, 3000000); roster_tap(&r, 0xB, 0, 3000000); roster_tap(&r, 0xC, 0, 3000000);
  CHECK(roster_find(&r, 0xB) == 1);
  CHECK(roster_find(&r, 0xD) == -1);
  CHECK(roster_inside(&r, slots) == 3 && slots[0] == 0 && slots[2] == 2);
  roster_force_out(&r, 1);
  CHECK(roster_count(&r) == 2);
  CHECK(roster_inside(&r, slots) == 2 && slots[1] == 2);
  /* the forced-out card's next tap is an entry again */
  CHECK(roster_tap(&r, 0xB, 10000000, 3000000) == TAP_IN);
  roster_force_out(&r, -1); roster_force_out(&r, ROSTER_MAX);   /* out of range: no-op */
  CHECK(roster_count(&r) == 3);
}

static void test_knob(void) {
  CHECK(knob_pick(0, 1, 0) == 0);
  CHECK(knob_pick(4095, 1, 0) == 0);
  CHECK(knob_pick(0, 4, 255) == 0);
  CHECK(knob_pick(4095, 4, 255) == 3);
  CHECK(knob_pick(2100, 4, 255) == 2);
  /* boundary between step 1 and 2 is 2048: hysteresis keeps the current step */
  CHECK(knob_pick(2060, 4, 1) == 1);
  CHECK(knob_pick(2200, 4, 1) == 2);
  CHECK(knob_pick(2000, 4, 2) == 2);
  CHECK(knob_pick(1900, 4, 2) == 1);
  CHECK(knob_pick(5000, 4, 0) == 3);                 /* clamp */
  /* every step is reachable for 31 steps (late-minute setting 0..30) */
  int seen[31] = {0};
  for (int a = 0; a < 4096; a += 16) seen[knob_pick((uint16_t)a, 31, 255)] = 1;
  int all = 1; for (int i = 0; i < 31; i++) all &= seen[i];
  CHECK(all);
}

static void test_pickup(void) {
  pickup_t p = { false, 0 };
  CHECK(pickup_update(&p, 100, 900, 40));            /* not held: knob in control */
  pickup_hold(&p);
  CHECK(!pickup_update(&p, 100, 900, 40));           /* far below */
  CHECK(!pickup_update(&p, 500, 900, 40));
  CHECK(pickup_update(&p, 880, 900, 40));            /* within window: released */
  CHECK(pickup_update(&p, 100, 900, 40));            /* stays released */
  pickup_hold(&p);
  CHECK(!pickup_update(&p, 200, 600, 40));           /* below */
  CHECK(pickup_update(&p, 990, 600, 40));            /* jumped past in one step: crossing counts */
  pickup_hold(&p);
  CHECK(!pickup_update(&p, 999, 100, 40));           /* above */
  CHECK(!pickup_update(&p, 700, 100, 40));
  CHECK(pickup_update(&p, 50, 100, 40));             /* crossed from above */
}

static void test_log(void) {
  evlog_t l = {0};
  CHECK(log_get(&l, 0) == 0);
  log_push(&l, 100, 0xA, EV_IN);
  log_push(&l, 200, 0xB, EV_LATE);
  CHECK(l.count == 2);
  CHECK(log_get(&l, 0)->uid == 0xB && log_get(&l, 0)->type == EV_LATE);
  CHECK(log_get(&l, 1)->sod == 100);
  CHECK(log_get(&l, 2) == 0);
  for (int i = 0; i < 40; i++) log_push(&l, 1000 + i, i, EV_OUT);
  CHECK(l.count == LOG_MAX);
  CHECK(log_get(&l, 0)->sod == 1039);
  CHECK(log_get(&l, LOG_MAX - 1)->sod == 1039 - (LOG_MAX - 1));
  CHECK(log_get(&l, LOG_MAX) == 0);
  CHECK(strcmp(ev_txt(EV_START), "START") == 0 && strcmp(ev_txt(EV_RESET), "RESET") == 0);
  CHECK(strcmp(ev_txt(200), "?") == 0);
}

static void test_session(void) {
  session_t s; session_init(&s, 10);
  const uint32_t T0 = sod_of(9, 0, 0);
  CHECK(sod_of(23, 59, 59) == 86399);
  CHECK(sod_elapsed(sod_of(23, 50, 0), sod_of(0, 10, 0)) == 20 * 60);   /* across midnight */
  /* no session: every entry is a plain IN, nothing recorded */
  CHECK(session_student_in(&s, 0xA, T0) == EV_IN);
  CHECK(s.n_att == 0);
  /* teacher arrives at 09:00 with 0xA already inside */
  uint32_t inside[] = { 0xA };
  session_start(&s, 0x62E26506, T0, inside, 1);
  CHECK(s.active && s.n_att == 1 && !session_is_late(&s, 0xA));
  CHECK(session_student_in(&s, 0xB, T0 + 9 * 60) == EV_IN);            /* 9 min: on time */
  CHECK(session_student_in(&s, 0xC, T0 + 10 * 60) == EV_IN);           /* exactly 10: on time */
  CHECK(session_student_in(&s, 0xD, T0 + 10 * 60 + 1) == EV_LATE);     /* 10 min 1 s: late */
  CHECK(session_is_late(&s, 0xD) && !session_is_late(&s, 0xB));
  CHECK(session_student_in(&s, 0xD, T0 + 30 * 60) == EV_IN);           /* comes back: not late twice */
  CHECK(session_student_in(&s, 0xB, T0 + 40 * 60) == EV_IN);           /* on-time student stays on time */
  CHECK(!session_is_late(&s, 0xB));
  CHECK(s.n_att == 4 && session_late_count(&s) == 1);
  session_end(&s, T0 + 50 * 60);
  CHECK(!s.active && s.has_last && s.last_present == 4 && s.last_late == 1);
  CHECK(s.last_end - s.last_start == 50 * 60);
  session_end(&s, T0 + 60 * 60);                                         /* ending twice: no-op */
  CHECK(s.last_end == T0 + 50 * 60);
  /* new session starts clean */
  session_start(&s, 0x62E26506, T0 + 3600, 0, 0);
  CHECK(s.n_att == 0 && session_late_count(&s) == 0);
  /* late_min = 0: any entry after the start second is late */
  session_init(&s, 0);
  session_start(&s, 1, T0, 0, 0);
  CHECK(session_student_in(&s, 0xE, T0) == EV_IN);
  CHECK(session_student_in(&s, 0xF, T0 + 1) == EV_LATE);
  /* attendance list is bounded */
  session_init(&s, 10); session_start(&s, 1, T0, 0, 0);
  for (int i = 0; i < ATT_MAX + 5; i++) session_student_in(&s, 0x100 + i, T0);
  CHECK(s.n_att == ATT_MAX);
}

static void test_hist(void) {
  hist_t h = {0};
  uint8_t lo, hi;
  CHECK(!hist_range(&h, 4, &lo, &hi));
  hist_push(&h, HIST_GAP);
  CHECK(!hist_range(&h, 4, &lo, &hi));                 /* only gaps */
  hist_push(&h, 25); hist_push(&h, 26);
  CHECK(h.count == 3 && hist_get(&h, 0) == HIST_GAP && hist_get(&h, 2) == 26);
  CHECK(hist_range(&h, 4, &lo, &hi) && hi - lo >= 4 && lo <= 25 && hi >= 26);
  for (int i = 0; i < HIST_N + 10; i++) hist_push(&h, (uint8_t)(i % 50));
  CHECK(h.count == HIST_N);
  CHECK(hist_get(&h, HIST_N - 1) == (HIST_N + 9) % 50);  /* newest */
  CHECK(hist_get(&h, 0) == 10 % 50);                     /* oldest kept */
  hist_t z = {0}; hist_push(&z, 0); hist_push(&z, 0);
  CHECK(hist_range(&z, 4, &lo, &hi) && lo == 0 && hi == 4);  /* cannot go below 0 */
  CHECK(graph_y(10, 10, 20, 56) == 55);                  /* min at the bottom */
  CHECK(graph_y(20, 10, 20, 56) == 0);                   /* max at the top */
  CHECK(graph_y(5, 10, 20, 56) == 55 && graph_y(30, 10, 20, 56) == 0);
  CHECK(graph_y(15, 10, 20, 56) == 27);                  /* middle: 55 - 27.5 rounded */
  int mono = 1;
  for (int v = 10; v < 20; v++) if (graph_y((uint8_t)(v + 1), 10, 20, 56) > graph_y((uint8_t)v, 10, 20, 56)) mono = 0;
  CHECK(mono);
}

int main(void) {
  test_pot(); test_led_duty(); test_datetime(); test_fan();
  test_roster(); test_roster_full(); test_roster_wrap(); test_allowed();
  test_cards(); test_roster_helpers(); test_knob(); test_pickup(); test_log(); test_session(); test_hist();
  printf("%d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
