/* Host-side tests for Core/Inc/logic.h. No board needed:
 *   gcc -Wall -Wextra -ICore/Inc tests/host_test.c -o host_test && ./host_test
 */
#include <stdio.h>
#include "logic.h"

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

int main(void) {
  test_pot(); test_led_duty(); test_datetime();
  test_roster(); test_roster_full(); test_roster_wrap(); test_allowed();
  printf("%d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
