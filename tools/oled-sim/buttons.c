/* Runs the real menu-button code (EXTI9_5 ISR + ui_button_poll) on a PC with
 * simulated edges: clean taps, bouncy taps, 2 s holds, a missed edge. Exit code 0 = all pass. */
#include <stdio.h>
#define main fw_main
#include "../../firmware/Core/Src/main.c"
#undef main
#include "regs.h"
static void wait_ms(uint32_t ms) { h_TIM2.CNT += ms * 1000; }
static void pin(int low) {                       /* level change + EXTI interrupt */
  if (low) h_GPIOA.IDR &= ~(1u << 7); else h_GPIOA.IDR |= (1u << 7);
  h_EXTI.PR = 1u << 7; EXTI9_5_IRQHandler();
}
static void bounce(int final_low) { for (int i = 0; i < 4; i++) { pin(i & 1 ? final_low : !final_low); wait_ms(1); } pin(final_low); }
static uint32_t tap_seen; static int selects;
static void loop_pass(void) { uint32_t t = ui_taps; while (tap_seen != t) { tap_seen++; ui_next_screen(); } scr_t before = scr; (void)before; ui_button_poll(); }
static void run_ms(uint32_t ms) { for (uint32_t t = 0; t < ms; t += 250) { wait_ms(250); loop_pass(); } }
int main(void) {
  h_USART2.SR = USART_SR_TXE; h_GPIOA.IDR = 1u << 7; h_TIM2.CNT = 5000000;
  int fail = 0;
  #define EXPECT(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fail++; } else printf("ok: %s\n", msg); } while (0)
  /* 1: clean short tap */
  pin(1); wait_ms(150); pin(0); run_ms(500);
  EXPECT(scr == SCR_ROSTER, "short tap -> next screen");
  /* 2: bouncy short tap */
  bounce(1); wait_ms(200); bounce(0); run_ms(500);
  EXPECT(scr == SCR_LOG, "bouncy tap counts once");
  /* 3: hold 2.5 s on LOG = select (CSV dump toast), screen unchanged on release */
  bounce(1); run_ms(2500); EXPECT(toast_on && toast[0]=='L', "hold fires select while still held");
  bounce(0); run_ms(500);
  EXPECT(scr == SCR_LOG, "release after a hold does not change screen");
  /* 4: hold 1 s then release = tap */
  pin(1); run_ms(1000); pin(0); run_ms(500);
  EXPECT(scr == SCR_CLASS, "1 s press is still a tap");
  /* 5: missed edge: pin goes low without the ISR seeing it (worst-case bounce) */
  h_GPIOA.IDR &= ~(1u << 7); run_ms(2500);
  EXPECT(ui_long_done, "poll resyncs a missed press and fires select");
  h_GPIOA.IDR |= (1u << 7); run_ms(500);
  EXPECT(scr == SCR_CLASS && !ui_down, "and resyncs the release without a tap");
  /* 6: wraps through all 5 screens back to HOME */
  for (int i = 0; i < 2; i++) { pin(1); wait_ms(100); pin(0); run_ms(250); }
  EXPECT(scr == SCR_HOME, "GRAPH -> HOME wrap");
  printf("%s\n", fail ? "FAILURES" : "all button cases pass");
  return fail;
}
