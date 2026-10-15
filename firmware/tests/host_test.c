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

int main(void) {
  test_roster(); test_roster_full(); test_roster_wrap(); test_allowed();
  printf("%d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
