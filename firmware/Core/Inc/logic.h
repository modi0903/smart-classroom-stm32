/*
 * logic.h - RFID headcount logic, no hardware in it.
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

#endif
