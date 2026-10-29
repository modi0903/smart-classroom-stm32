/* Renders every v2.1 screen from the firmware's own drawing code, with a sample class:
 * a student at 08:58, the teacher at 09:02, a student at 09:07 and a late one at 09:14.
 * Output: out/<screen>.pgm, 4x scale, viewable in most image viewers. */
#include <stdio.h>
#define main fw_main
#include "../../firmware/Core/Src/main.c"
#undef main
#include "regs.h"

static void set_time(int h, int m, int s) {
  h_RTC.TR = ((uint32_t)bcd(h) << 16) | ((uint32_t)bcd(m) << 8) | bcd(s);
  h_RTC.DR = ((uint32_t)bcd(26) << 16) | ((uint32_t)bcd(10) << 8) | bcd(28);
  h_TIM2.CNT += 1000000;
}
/* the same path as rfid_service() after a UID is read, without the SPI */
static void tap(uint32_t uid) {
  rf.last_uid = uid;
  switch (roster_tap(&rf.roster, uid, micros(), TAP_LOCKOUT_MS * 1000UL)) {
    case TAP_IN:  rf.last_txt = "IN";  rf.boost_on = true; rf.boost_t0 = micros(); feature_tap_in(uid); break;
    case TAP_OUT: rf.last_txt = "OUT"; feature_tap_out(uid); break;
    case TAP_FULL: rf.last_txt = "FULL"; break;
    default: break;
  }
  h_TIM2.CNT += 5000000;
}
static void save_pgm(const char *name) {
  char path[128]; snprintf(path, sizeof path, "out/%s.pgm", name);
  FILE *f = fopen(path, "wb");
  if (!f) { perror(path); return; }
  const int S = 4;
  fprintf(f, "P5\n%d %d\n255\n", 128 * S, 64 * S);
  for (int y = 0; y < 64 * S; y++)
    for (int x = 0; x < 128 * S; x++) {
      int on = (fb[(y / S) >> 3][x / S] >> ((y / S) & 7)) & 1;
      fputc(on ? 230 : 12, f);
    }
  fclose(f);
  printf("wrote %s\n", path);
}
static view_t V;
static void shot(scr_t s, const char *name) {
  scr = s; fb_clear();
  switch (s) {
    case SCR_HOME: draw_home(&V); break;   case SCR_ROSTER: draw_roster(); break;
    case SCR_LOG: draw_log(); break;       case SCR_CLASS: draw_class(); break;
    case SCR_GRAPH: draw_graph(&V); break; default: break;
  }
  save_pgm(name);
}

int main(void) {
  h_USART2.SR = USART_SR_TXE;               /* uart_putc never blocks */
  rtc_ok = true; rtc_lse = true; rf.ok = true; oled_addr = 0;
  session_init(&sess, LATE_MIN_DEFAULT);
  for (int i = 0; i < HIST_N; i++) {        /* 18 minutes of sample history */
    hist_push(&hist[0], (uint8_t)(24 + i / 30));
    hist_push(&hist[1], (uint8_t)(58 + (i > 70 && i < 80 ? 14 - (i > 75 ? (i - 75) * 3 : 0) : 0)));
    hist_push(&hist[2], (uint8_t)(i < 40 ? 0 : i < 55 ? 1 : i < 70 ? 2 : 3));
  }
  set_time(8, 58, 10); tap(0xA1B2C3D4);
  set_time(9, 2, 0);   tap(0x62E26506);     /* teacher: class starts */
  set_time(9, 7, 30);  tap(0x0BADF00D);
  set_time(9, 14, 5);  tap(0x7E57CAFE);     /* late: more than 10 min */
  set_time(9, 25, 41);
  V = (view_t){ {45, DIST_FAR}, {true, false}, 1, true, true, 26, 61, 750, 60, 66 };
  shot(SCR_HOME, "home");
  pick = 2;  shot(SCR_ROSTER, "roster");
  pick = 0;  shot(SCR_LOG, "log");
  late_pick = 15; shot(SCR_CLASS, "class");
  graph_kind = 0; shot(SCR_GRAPH, "graph_temp");
  graph_kind = 1; shot(SCR_GRAPH, "graph_hum");
  pickup_hold(&bright_pick); shot(SCR_HOME, "home_pickup"); bright_pick.held = false;
  set_toast("KEEP HOLDING...", 0); shot(SCR_LOG, "log_hold"); toast_on = false;
  set_time(9, 55, 3); tap(0x62E26506);      /* teacher leaves: class ends */
  late_pick = 255; shot(SCR_CLASS, "class_ended");
  int ok = !sess.active && sess.last_present == 3 && sess.last_late == 1;
  printf("class summary: present=%d late=%d -> %s\n", sess.last_present, sess.last_late, ok ? "ok" : "UNEXPECTED");
  return ok ? 0 : 1;
}
