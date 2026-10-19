/*
 * Smart Classroom, Nucleo-F401RE, bare metal CMSIS, no HAL.
 * 2 zones: HC-SR04 occupancy + class-light LED each, shared DHT11 + OLED,
 * LM393 module for the AUTO mode's darkness check, RC522 RFID reader at the
 * door, one pot dimming both LEDs, on-chip RTC for the clock, one fan motor
 * per zone.
 *
 * Zone A  TRIG PA10 (D2)   ECHO PB3 (D3)   LED PC7 (D9)   TIM3_CH2
 * Zone B  TRIG PB5  (D4)   ECHO PB4 (D5)   LED PB6 (D10)  TIM4_CH1
 * DHT11   PA9 (D8)
 * OLED    SCL PB8 (D15)  SDA PB9 (D14), I2C1
 * LDR     PB0 (A3), LM393 DO pin, HIGH = dark on this module
 * RC522   SPI2: SCK PB13  MISO PB14  MOSI PB15  SDA(SS) PB12  RST PC5   3V3 ONLY
 * POT     light: wiper on PA0 (A0), ADC1 ch0.  fan: wiper on PA4 (A2), ADC1 ch4.
 *         Pot ends on 3V3 and GND.
 * FAN     PA8 (D7)  TIM1_CH1 -> 220R -> IRLZ44N gate (10k gate-GND), 20 kHz PWM.
 *         Motor on the 9 V battery, 1N4007 across it, battery (-) to Nucleo GND.
 * BUTTON  B1 (blue, on the board), PC13, EXTI line 13 interrupt
 * HC-SR04 on 5V, everything else 3V3, grounds common.
 *
 * Each zone's mode (OFF/AUTO/DIM/MAX) is a #define below; the pot sets how
 * bright "on" is, the blue button cycles a manual override AUTO -> ON -> OFF
 * for lights and fan together. The fan runs while RFID says someone is
 * inside, at the speed set by the fan pot, capped at FAN_MAX_PCT of the battery.
 * Occupancy is debounced: needs OCC_MS of continuous detection to go
 * occupied, VACANT_MS of continuous clear to go back to empty. Tapping a card
 * at the door toggles that card in/out of the headcount; a tap-in also makes
 * both zones count as occupied for ENTRY_BOOST_MS.
 *
 * Clock: on-chip RTC, 32.768 kHz LSE crystal (falls back to the rough LSI if
 * the crystal is missing). Set from the build time on flashing; set it exactly
 * over the serial port with "T 08:30:00" and "D 18/10/26".
 *
 * Interrupts: EXTI15_10 (button) and USART2 RX (serial commands). Everything
 * else is polled. HSI 16MHz, no PLL. UART2 prints raw state every loop and
 * LD2 toggles as a heartbeat. Pure logic lives in logic.h (host tested).
 */

#include "stm32f4xx.h"
#include <stdint.h>
#include <stdbool.h>
#include "logic.h"

#define ZONE_A_MODE 1   /* 0 off, 1 auto, 2 dim, 3 max */
#define ZONE_B_MODE 1
#define FAN_MAX_PCT 60  /* 9 V battery x 60 % = ~5.4 V average for 3-6 V motors. 100 for 9 V motors */
#ifndef USE_POT
#define USE_POT     1   /* 0 until the pot is wired, or A0 floats and brightness wanders */
#endif
#ifndef USE_FAN_POT
#define USE_FAN_POT 1   /* fan speed knob on A2. 0 = fan runs at full allowed speed when on */
#endif

#define OCC_CM      100
#define OCC_MS      5000
#define VACANT_MS   10000
#define DHT_PERIOD_US 2000000
#define PING_GAP_MS 60
#define I2C_TIMEOUT_US 2000

#define ENTRY_BOOST_MS 30000UL       /* after a tap-in, both zones count as occupied */
#define TAP_LOCKOUT_MS 3000UL        /* same card ignored this long after a sighting */
#define STALE_RESET_MS 120000UL      /* headcount > 0 but zones empty this long: clear it */
#define POT_SAMPLES    8             /* ADC reads averaged per pot reading */
#define BTN_DEBOUNCE_US 250000UL     /* ignore button edges closer than this */
#define RX_QUIET_US    5000000UL     /* hold the status line this long after you type */

#define DIST_NO_ECHO 0xFFFE
#define DIST_FAR     0xFFFF

#define LED_OFF  0
#define LED_DIM  100
#define LED_FULL 999

typedef struct { GPIO_TypeDef *port; uint8_t pin; } pin_t;

static const pin_t TRIG[2] = { {GPIOA, 10}, {GPIOB, 5} };
static const pin_t ECHO[2] = { {GPIOB, 3},  {GPIOB, 4} };
static const pin_t DHT  = {GPIOA, 9};
static const pin_t LED  = {GPIOA, 5};        /* LD2 */
static const pin_t LDR  = {GPIOB, 0};
static const pin_t RC_CS  = {GPIOB, 12};    /* RC522 SDA/SS, software chip select */
static const pin_t RC_RST = {GPIOC, 5};

/* Cards that may open the door. 0 ends the list; {0} alone = accept any card.
 * Tap a card, read its UID off the UART or OLED, paste it here as 0xAABBCCDD. */
static const uint32_t ALLOWED_UIDS[] = { 0 };
static const int   mode[2] = { ZONE_A_MODE, ZONE_B_MODE };
static const char *MODE_TXT[4] = { "OFF", "AUT", "DIM", "MAX" };

static void pin_mode(pin_t p, uint32_t mode) {
  p.port->MODER = (p.port->MODER & ~(3U << (p.pin * 2))) | (mode << (p.pin * 2));
}
static void pin_pull(pin_t p, uint32_t pupd) {
  p.port->PUPDR = (p.port->PUPDR & ~(3U << (p.pin * 2))) | (pupd << (p.pin * 2));
}
static void pin_od(pin_t p, bool od) {
  if (od) p.port->OTYPER |= (1U << p.pin); else p.port->OTYPER &= ~(1U << p.pin);
}
static void pin_af(pin_t p, uint32_t af) {
  uint32_t i = p.pin >> 3, sh = (p.pin & 7) * 4;
  p.port->AFR[i] = (p.port->AFR[i] & ~(0xFU << sh)) | (af << sh);
}
static inline void pin_hi(pin_t p) { p.port->BSRR = (1U << p.pin); }
static inline void pin_lo(pin_t p) { p.port->BSRR = (1U << (p.pin + 16)); }
static inline bool pin_rd(pin_t p) { return (p.port->IDR >> p.pin) & 1U; }

static inline uint32_t micros(void) { return TIM2->CNT; }
static void delay_us(uint32_t us) { uint32_t s = micros(); while (micros() - s < us); }
static void delay_ms(uint32_t ms) { while (ms--) delay_us(1000); }

static void uart_init(void) {
  pin_t tx = {GPIOA, 2};
  pin_af(tx, 7); pin_mode(tx, 2);
  USART2->BRR = 139;
  USART2->CR1 = USART_CR1_TE | USART_CR1_UE;
}
static void uart_putc(char c) { while (!(USART2->SR & USART_SR_TXE)); USART2->DR = (uint8_t)c; }
static void uart_puts(const char *s) { while (*s) uart_putc(*s++); }
static void uart_putu(uint32_t v) {
  char b[10]; int i = 0;
  do { b[i++] = '0' + v % 10; v /= 10; } while (v);
  while (i--) uart_putc(b[i]);
}
static void uart_puthex8(uint8_t v) {
  const char *h = "0123456789ABCDEF";
  uart_putc(h[v >> 4]); uart_putc(h[v & 15]);
}

/* ---------- interrupt-fed state ---------- */
/* USART2 RX: the ISR drops each byte into a ring buffer, main() reads it. */
#define RX_SIZE 64
static volatile uint8_t  rx_buf[RX_SIZE];
static volatile uint8_t  rx_head;            /* written only by the ISR  */
static volatile uint8_t  rx_tail;            /* written only by main()   */
static volatile uint32_t rx_last_us;
static volatile bool     rx_any;             /* anything typed since boot */

/* Blue button: the ISR only counts presses; main() decides what they mean. */
static volatile uint32_t btn_presses;
static volatile uint32_t btn_last_us;

void USART2_IRQHandler(void) {
  uint32_t sr = USART2->SR;
  if (sr & (USART_SR_RXNE | USART_SR_ORE)) {
    uint8_t c = (uint8_t)USART2->DR;         /* reading DR clears RXNE and ORE */
    uint8_t next = (uint8_t)((rx_head + 1) % RX_SIZE);
    if (next != rx_tail) { rx_buf[rx_head] = c; rx_head = next; }   /* full: drop */
    rx_last_us = TIM2->CNT;
    rx_any = true;
  }
}

void EXTI15_10_IRQHandler(void) {
  if (EXTI->PR & (1U << 13)) {
    EXTI->PR = (1U << 13);                   /* write 1 to clear the pending flag */
    uint32_t now = TIM2->CNT;
    if (now - btn_last_us >= BTN_DEBOUNCE_US) { btn_presses++; btn_last_us = now; }
  }
}

static void uart_rx_init(void) {
  pin_t rx = {GPIOA, 3};
  pin_af(rx, 7); pin_mode(rx, 2);
  USART2->CR1 |= USART_CR1_RE | USART_CR1_RXNEIE;
  NVIC_EnableIRQ(USART2_IRQn);
}

static void button_init(void) {
  pin_t b1 = {GPIOC, 13};                    /* board has its own pull-up, active low */
  pin_mode(b1, 0);
  RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
  SYSCFG->EXTICR[3] = (SYSCFG->EXTICR[3] & ~SYSCFG_EXTICR4_EXTI13) | SYSCFG_EXTICR4_EXTI13_PC;
  EXTI->FTSR |= (1U << 13);                  /* falling edge = pressed */
  EXTI->RTSR &= ~(1U << 13);
  EXTI->PR    = (1U << 13);
  EXTI->IMR  |= (1U << 13);
  NVIC_EnableIRQ(EXTI15_10_IRQn);
}

static int uart_getc(void) {
  if (rx_tail == rx_head) return -1;
  int c = rx_buf[rx_tail];
  rx_tail = (uint8_t)((rx_tail + 1) % RX_SIZE);
  return c;
}

static uint16_t read_cm(int z) {
  if (pin_rd(ECHO[z])) return DIST_NO_ECHO;
  pin_hi(TRIG[z]); delay_us(10); pin_lo(TRIG[z]);
  uint32_t s = micros();
  while (!pin_rd(ECHO[z])) if (micros() - s > 5000) return DIST_NO_ECHO;
  uint32_t t0 = micros();
  while (pin_rd(ECHO[z])) if (micros() - t0 > 25000) return DIST_FAR;
  return (uint16_t)((micros() - t0) / 58);
}

static bool wait_level(pin_t p, bool level, uint32_t timeout_us) {
  uint32_t s = micros();
  while (pin_rd(p) != level) if (micros() - s > timeout_us) return false;
  return true;
}

static bool dht11_read(uint8_t *temp_c, uint8_t *hum) {
  uint8_t d[5] = {0};
  pin_lo(DHT); delay_ms(20);
  pin_hi(DHT);
  if (!wait_level(DHT, 0, 100)) return false;
  if (!wait_level(DHT, 1, 100)) return false;
  if (!wait_level(DHT, 0, 100)) return false;
  for (int i = 0; i < 40; i++) {
    if (!wait_level(DHT, 1, 100)) return false;
    uint32_t t = micros();
    if (!wait_level(DHT, 0, 100)) return false;
    if (micros() - t > 45) d[i / 8] |= (uint8_t)(1U << (7 - (i % 8)));
  }
  if ((uint8_t)(d[0] + d[1] + d[2] + d[3]) != d[4]) return false;
  *hum = d[0]; *temp_c = d[2];
  return true;
}

static const pin_t SCL = {GPIOB, 8};
static const pin_t SDA = {GPIOB, 9};

static void i2c_bus_clear(void) {
  pin_od(SCL, true); pin_od(SDA, true);
  pin_pull(SCL, 1); pin_pull(SDA, 1);
  pin_hi(SCL); pin_hi(SDA);
  pin_mode(SCL, 1); pin_mode(SDA, 1);
  for (int i = 0; i < 9; i++) { pin_lo(SCL); delay_us(5); pin_hi(SCL); delay_us(5); }
  pin_lo(SDA); delay_us(5); pin_hi(SCL); delay_us(5); pin_hi(SDA); delay_us(5);
}

static void i2c_init(void) {
  i2c_bus_clear();
  pin_af(SCL, 4); pin_af(SDA, 4);
  SCL.port->OSPEEDR |= (3U << (SCL.pin * 2)) | (3U << (SDA.pin * 2));
  pin_mode(SCL, 2); pin_mode(SDA, 2);
  RCC->APB1RSTR |= RCC_APB1RSTR_I2C1RST;
  RCC->APB1RSTR &= ~RCC_APB1RSTR_I2C1RST;
  I2C1->CR2 = 16;
  I2C1->CCR = 80;
  I2C1->TRISE = 17;
  I2C1->CR1 = I2C_CR1_PE;
}

static bool i2c_wait(uint32_t flag) {
  uint32_t s = micros();
  while (!(I2C1->SR1 & flag)) {
    if (I2C1->SR1 & I2C_SR1_AF) return false;
    if (micros() - s > I2C_TIMEOUT_US) return false;
  }
  return true;
}

static bool i2c_write(uint8_t addr7, uint8_t ctrl, const uint8_t *buf, uint32_t len) {
  uint32_t s = micros();
  while (I2C1->SR2 & I2C_SR2_BUSY)
    if (micros() - s > I2C_TIMEOUT_US) { i2c_init(); return false; }
  I2C1->CR1 |= I2C_CR1_START;
  if (!i2c_wait(I2C_SR1_SB)) goto fail;
  I2C1->DR = (uint8_t)(addr7 << 1);
  if (!i2c_wait(I2C_SR1_ADDR)) goto fail;
  (void)I2C1->SR1; (void)I2C1->SR2;
  if (!i2c_wait(I2C_SR1_TXE)) goto fail;
  I2C1->DR = ctrl;
  for (uint32_t i = 0; i < len; i++) {
    if (!i2c_wait(I2C_SR1_TXE)) goto fail;
    I2C1->DR = buf[i];
  }
  if (!i2c_wait(I2C_SR1_BTF)) goto fail;
  I2C1->CR1 |= I2C_CR1_STOP;
  return true;
fail:
  I2C1->SR1 &= ~I2C_SR1_AF;
  I2C1->CR1 |= I2C_CR1_STOP;
  return false;
}

static uint8_t oled_addr = 0;
static const uint8_t font5x7[][5] = {
#include "font_table.inc"
};

static bool oled_cmd(uint8_t c) { return i2c_write(oled_addr, 0x00, &c, 1); }

static bool oled_probe_and_init(void) {
  static const uint8_t addrs[2] = {0x3C, 0x3D};
  static const uint8_t init[] = {
    0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40, 0xAD, 0x8B,
    0xA1, 0xC8, 0xDA, 0x12, 0x81, 0xCF, 0xD9, 0x1F, 0xDB, 0x40,
    0xA4, 0xA6, 0xAF
  };
  oled_addr = 0;
  for (int a = 0; a < 2; a++) {
    uint8_t off = 0xAE;
    if (i2c_write(addrs[a], 0x00, &off, 1)) { oled_addr = addrs[a]; break; }
  }
  if (!oled_addr) return false;
  for (uint32_t i = 0; i < sizeof(init); i++)
    if (!oled_cmd(init[i])) { oled_addr = 0; return false; }
  return true;
}

static int glyph_index(char c) {
  if (c >= 'a' && c <= 'z' && c != 'c' && c != 'm') c -= 32;
  if (c >= ' ' && c <= 'Z') return c - ' ';
  if (c == 'c') return 59;
  if (c == 'm') return 60;
  return '?' - ' ';
}

static void oled_line(uint8_t page, const char *s) {
  if (!oled_addr) return;
  uint8_t buf[128] = {0};
  for (int ch = 0; ch < 21; ch++) {
    char c = *s ? *s++ : ' ';
    const uint8_t *g = font5x7[glyph_index(c)];
    for (int k = 0; k < 5; k++) buf[ch * 6 + k] = g[k];
  }
  oled_cmd(0xB0 | page);
  oled_cmd(0x02);
  oled_cmd(0x10);
  if (!i2c_write(oled_addr, 0x40, buf, 128)) oled_addr = 0;
}

static char *put_str(char *b, const char *s) { while (*s) *b++ = *s++; return b; }
static char *put_u(char *b, uint32_t v, int width) {
  char t[10]; int n = 0;
  do { t[n++] = '0' + v % 10; v /= 10; } while (v);
  for (int i = n; i < width; i++) *b++ = ' ';
  while (n--) *b++ = t[n];
  return b;
}

static char *put_u2(char *b, uint32_t v) { *b++ = (char)('0' + v / 10 % 10); *b++ = (char)('0' + v % 10); return b; }

static void zone_line(char *b, char zone, uint16_t cm, bool occ, int mode) {
  *b++ = zone; *b++ = ':';
  if (cm == DIST_NO_ECHO) b = put_str(b, "NO ECHO ");
  else if (cm == DIST_FAR) b = put_str(b, "FAR     ");
  else { b = put_u(b, cm, 3); b = put_str(b, "cm "); b = put_str(b, occ ? "OCC " : "--- "); }
  b = put_str(b, MODE_TXT[mode]);
  *b = 0;
}

/* TIM3_CH2 on PC7 (D9, zone A LED), TIM4_CH1 on PB6 (D10, zone B LED). 1kHz PWM. */
static void pwm_init_leds(void) {
  RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN | RCC_AHB1ENR_GPIOCEN;
  RCC->APB1ENR |= RCC_APB1ENR_TIM3EN | RCC_APB1ENR_TIM4EN;
  pin_t a = {GPIOC, 7}, b = {GPIOB, 6};
  pin_af(a, 2); pin_mode(a, 2);
  pin_af(b, 2); pin_mode(b, 2);

  TIM3->PSC = 16 - 1;
  TIM3->ARR = 999;
  TIM3->CCMR1 = (TIM3->CCMR1 & ~TIM_CCMR1_OC2M) | (6U << 12) | TIM_CCMR1_OC2PE;
  TIM3->CCER |= TIM_CCER_CC2E;
  TIM3->CR1 |= TIM_CR1_ARPE | TIM_CR1_CEN;

  TIM4->PSC = 16 - 1;
  TIM4->ARR = 999;
  TIM4->CCMR1 = (TIM4->CCMR1 & ~TIM_CCMR1_OC1M) | (6U << 4) | TIM_CCMR1_OC1PE;
  TIM4->CCER |= TIM_CCER_CC1E;
  TIM4->CR1 |= TIM_CR1_ARPE | TIM_CR1_CEN;
}
static inline void led_set(int z, uint16_t duty) {
  if (z == 0) TIM3->CCR2 = duty; else TIM4->CCR1 = duty;
}

static inline bool is_dark(void) { return pin_rd(LDR); }   /* HIGH = dark on this module */

/* ===================================================================== */
/* Fan: TIM1_CH1 on PA8 (D7). 16 MHz / 800 = 20 kHz, above hearing, so  */
/* the motor doesn't whine. CCR 800 = 100 %.                             */
/* ===================================================================== */
static void fan_init(void) {
  RCC->APB2ENR |= RCC_APB2ENR_TIM1EN;
  pin_t a = {GPIOA, 8};
  pin_af(a, 1); pin_mode(a, 2);                     /* AF1 = TIM1 */
  TIM1->PSC = 0;
  TIM1->ARR = 799;
  TIM1->CCMR1 = (TIM1->CCMR1 & ~TIM_CCMR1_OC1M) | (6U << 4) | TIM_CCMR1_OC1PE;
  TIM1->CCR1 = 0;
  TIM1->CCER |= TIM_CCER_CC1E;
  TIM1->BDTR |= TIM_BDTR_MOE;                       /* advanced timer: no output without MOE */
  TIM1->EGR = TIM_EGR_UG;
  TIM1->CR1 |= TIM_CR1_ARPE | TIM_CR1_CEN;
}
static inline void fan_set(uint8_t pct) { TIM1->CCR1 = fan_ccr(pct, FAN_MAX_PCT); }

/* ===================================================================== */
/* Pot: ADC1 channel 0 on PA0.                                           */
/* ===================================================================== */
#if USE_POT || USE_FAN_POT
static void adc_init(void) {
  RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;
  pin_t a0 = {GPIOA, 0}, a2 = {GPIOA, 4};
  pin_mode(a0, 3); pin_mode(a2, 3);                 /* analog */
  ADC1->SMPR2 = (6U << 0) | (6U << 12);             /* ch0, ch4: 144 cycles, fine for 10k pots */
  ADC1->SQR1 = 0;                                   /* 1 conversion per sequence */
  ADC1->CR2 = ADC_CR2_ADON;
  delay_us(20);
}
static bool adc_read(uint8_t ch, uint16_t *out) {
  ADC1->SQR3 = ch;                                  /* the one conversion is this channel */
  uint32_t sum = 0;
  for (int i = 0; i < POT_SAMPLES; i++) {
    ADC1->CR2 |= ADC_CR2_SWSTART;
    uint32_t s = micros();
    while (!(ADC1->SR & ADC_SR_EOC)) if (micros() - s > 200) return false;
    sum += ADC1->DR;                                /* reading DR clears EOC */
  }
  *out = (uint16_t)(sum / POT_SAMPLES);
  return true;
}
#endif

/* ===================================================================== */
/* RTC: on-chip, in the backup domain, keeps running through a reset.    */
/* ===================================================================== */
static bool rtc_lse;                                /* true = crystal, false = LSI fallback */
static bool rtc_ok;

static void rtc_unlock(void) { RTC->WPR = 0xCA; RTC->WPR = 0x53; }
static void rtc_lock(void)   { RTC->WPR = 0xFF; }

static bool rtc_wait_sync(void) {                   /* shadow registers caught up */
  rtc_unlock();
  RTC->ISR &= ~RTC_ISR_RSF;
  rtc_lock();
  uint32_t s = micros();
  while (!(RTC->ISR & RTC_ISR_RSF)) if (micros() - s > 50000) return false;
  return true;
}

static bool rtc_write(const dt_t *t) {
  if (!dt_valid(t)) return false;
  rtc_unlock();
  RTC->ISR |= RTC_ISR_INIT;
  uint32_t s = micros();
  while (!(RTC->ISR & RTC_ISR_INITF))
    if (micros() - s > 50000) { rtc_lock(); return false; }
  /* 32768 Hz / 128 / 256 = 1 Hz. LSI is ~32 kHz: / 128 / 250. Two writes, S then A. */
  RTC->PRER = rtc_lse ? 255U : 249U;
  RTC->PRER |= (127U << 16);
  RTC->TR = ((uint32_t)bcd(t->h) << 16) | ((uint32_t)bcd(t->m) << 8) | bcd(t->s);
  RTC->DR = ((uint32_t)bcd(t->y) << 16) | ((uint32_t)weekday(t->d, t->mo, 2000 + t->y) << 13) |
            ((uint32_t)bcd(t->mo) << 8) | bcd(t->d);
  RTC->CR &= ~RTC_CR_FMT;                           /* 24 hour */
  RTC->ISR &= ~RTC_ISR_INIT;
  rtc_lock();
  return rtc_wait_sync();
}

static void rtc_read(dt_t *t) {
  uint32_t tr = RTC->TR;                            /* reading TR freezes DR until DR is read */
  uint32_t dr = RTC->DR;
  t->h  = unbcd((uint8_t)((tr >> 16) & 0x3F));
  t->m  = unbcd((uint8_t)((tr >> 8) & 0x7F));
  t->s  = unbcd((uint8_t)(tr & 0x7F));
  t->y  = unbcd((uint8_t)((dr >> 16) & 0xFF));
  t->mo = unbcd((uint8_t)((dr >> 8) & 0x1F));
  t->d  = unbcd((uint8_t)(dr & 0x3F));
}

/* BKP0R holds a fingerprint of the build that last set the clock. Same build
 * (plain reset) = leave the running clock alone. New build or empty backup
 * domain (power was off) = start again from this build's compile time. */
static bool rtc_init(void) {
  const uint32_t magic = fnv1a(__DATE__, __TIME__);
  RCC->APB1ENR |= RCC_APB1ENR_PWREN;
  PWR->CR |= PWR_CR_DBP;                            /* allow writes to the backup domain */

  if ((RCC->BDCR & RCC_BDCR_RTCEN) && RTC->BKP0R == magic) {
    rtc_lse = (RCC->BDCR & RCC_BDCR_RTCSEL) == RCC_BDCR_RTCSEL_0;
    if (!rtc_lse) {                                 /* LSI is switched off by every reset */
      RCC->CSR |= RCC_CSR_LSION;
      uint32_t s = micros();
      while (!(RCC->CSR & RCC_CSR_LSIRDY)) if (micros() - s > 10000) return false;
    }
    return rtc_wait_sync();
  }

  RCC->BDCR |= RCC_BDCR_BDRST;                      /* clean slate: only way to change RTCSEL */
  RCC->BDCR &= ~RCC_BDCR_BDRST;
  RCC->BDCR |= RCC_BDCR_LSEON;
  uint32_t s = micros();
  rtc_lse = true;
  while (!(RCC->BDCR & RCC_BDCR_LSERDY))
    if (micros() - s > 2500000) { rtc_lse = false; break; }   /* crystal can take ~2 s */

  if (rtc_lse) {
    RCC->BDCR |= RCC_BDCR_RTCSEL_0;                 /* 01 = LSE */
  } else {
    RCC->BDCR &= ~RCC_BDCR_LSEON;
    RCC->CSR |= RCC_CSR_LSION;
    s = micros();
    while (!(RCC->CSR & RCC_CSR_LSIRDY)) if (micros() - s > 10000) return false;
    RCC->BDCR |= RCC_BDCR_RTCSEL_1;                 /* 10 = LSI */
  }
  RCC->BDCR |= RCC_BDCR_RTCEN;

  dt_t t;
  if (!parse_build(__DATE__, __TIME__, &t) || !rtc_write(&t)) return false;
  RTC->BKP0R = magic;
  return true;
}

static void uart_put2(uint8_t v) { uart_putc((char)('0' + v / 10)); uart_putc((char)('0' + v % 10)); }
static void uart_time(void) {
  if (!rtc_ok) return;
  dt_t t; rtc_read(&t);
  uart_putc('['); uart_put2(t.h); uart_putc(':'); uart_put2(t.m); uart_putc(':'); uart_put2(t.s);
  uart_puts("] ");
}
static void uart_datetime(void) {
  if (!rtc_ok) { uart_puts("RTC not running\r\n"); return; }
  dt_t t; rtc_read(&t);
  uart_put2(t.d); uart_putc('/'); uart_put2(t.mo); uart_puts("/20"); uart_put2(t.y); uart_putc(' ');
  uart_put2(t.h); uart_putc(':'); uart_put2(t.m); uart_putc(':'); uart_put2(t.s);
  uart_puts(rtc_lse ? "  (crystal)\r\n" : "  (LSI fallback: drifts minutes per day)\r\n");
}

/* Serial commands, typed into PuTTY / Tera Term / Arduino serial monitor:
 *   T 08:30:00   set time       D 18/10/26   set date       ?   show both */
static void handle_command(char *cmd) {
  while (*cmd == ' ') cmd++;
  char op = *cmd;
  if (op >= 'a' && op <= 'z') op = (char)(op - 32);
  char *arg = cmd + 1;
  while (*arg == ' ') arg++;
  dt_t t;
  if (op == 'T' || op == 'D') {
    if (!rtc_ok) { uart_puts("RTC not running\r\n"); return; }
    rtc_read(&t);
    bool good = (op == 'T') ? parse_hms(arg, &t) : parse_dmy(arg, &t);
    if (good && rtc_write(&t)) { uart_puts("OK, now "); uart_datetime(); }
    else uart_puts(op == 'T' ? "Bad time. Use: T HH:MM or T HH:MM:SS\r\n"
                             : "Bad date. Use: D DD/MM/YY\r\n");
  } else if (op == '?' || op == 0) {
    uart_datetime();
  } else {
    uart_puts("Commands: T 08:30:00 | D 18/10/26 | ?\r\n");
  }
}

static void serial_service(void) {
  static char cmd[24];
  static uint8_t n;
  int c;
  while ((c = uart_getc()) >= 0) {
    if (c == '\r' || c == '\n') {
      if (n) { uart_puts("\r\n"); cmd[n] = 0; handle_command(cmd); n = 0; }
    } else if ((c == 8 || c == 127) && n) {
      n--; uart_puts("\b \b");
    } else if (c >= ' ' && n < sizeof(cmd) - 1) {
      cmd[n++] = (char)c; uart_putc((char)c);       /* echo, terminals don't by default */
    }
  }
}


/* ===================================================================== */
/* RFID: RC522 over SPI2, mode 0, 1 MHz. Register-level, no library.     */
/* ===================================================================== */
#define RC_COMMAND   0x01
#define RC_CONTROL   0x0C
#define RC_COMIRQ    0x04
#define RC_ERROR     0x06
#define RC_FIFODATA  0x09
#define RC_FIFOLEVEL 0x0A
#define RC_BITFRAME  0x0D
#define RC_COLL      0x0E
#define RC_MODE      0x11
#define RC_TXCONTROL 0x14
#define RC_TXASK     0x15
#define RC_TMODE     0x2A
#define RC_TPRESC    0x2B
#define RC_TRELOADH  0x2C
#define RC_TRELOADL  0x2D
#define RC_VERSION   0x37

#define RC_CMD_IDLE        0x00
#define RC_CMD_TRANSCEIVE  0x0C
#define RC_CMD_SOFTRESET   0x0F

#define PICC_REQA    0x26
#define PICC_SEL_CL1 0x93

typedef enum { RFID_NONE, RFID_OK, RFID_UNSUPPORTED } rfid_res_t;

static void spi2_init(void) {
  pin_t sck = {GPIOB, 13}, miso = {GPIOB, 14}, mosi = {GPIOB, 15};
  pin_af(sck, 5); pin_af(miso, 5); pin_af(mosi, 5);
  GPIOB->OSPEEDR |= (2U << (13 * 2)) | (2U << (14 * 2)) | (2U << (15 * 2));
  pin_mode(sck, 2); pin_mode(miso, 2); pin_mode(mosi, 2);
  pin_hi(RC_CS); pin_mode(RC_CS, 1);                 /* idle high = not selected */
  pin_hi(RC_RST); pin_mode(RC_RST, 1);
  /* master, fPCLK/16 = 1 MHz, CPOL 0 CPHA 0, 8 bit, MSB first, software NSS */
  SPI2->CR1 = SPI_CR1_MSTR | (3U << 3) | SPI_CR1_SSM | SPI_CR1_SSI;
  SPI2->CR1 |= SPI_CR1_SPE;
}
static uint8_t spi2_xfer(uint8_t b) {
  uint32_t s = micros();
  while (!(SPI2->SR & SPI_SR_TXE))  if (micros() - s > 1000) return 0;
  SPI2->DR = b;
  while (!(SPI2->SR & SPI_SR_RXNE)) if (micros() - s > 1000) return 0;
  return (uint8_t)SPI2->DR;
}

/* Address byte: bit7 = 1 for read, bits 6..1 = register, bit0 = 0. */
static void rc_write(uint8_t reg, uint8_t v) {
  pin_lo(RC_CS);
  spi2_xfer((uint8_t)((reg << 1) & 0x7E));
  spi2_xfer(v);
  pin_hi(RC_CS);
}
static uint8_t rc_read(uint8_t reg) {
  pin_lo(RC_CS);
  spi2_xfer((uint8_t)(((reg << 1) & 0x7E) | 0x80));
  uint8_t v = spi2_xfer(0);
  pin_hi(RC_CS);
  return v;
}
static void rc_set(uint8_t reg, uint8_t mask) { rc_write(reg, rc_read(reg) | mask); }
static void rc_clr(uint8_t reg, uint8_t mask) { rc_write(reg, rc_read(reg) & (uint8_t)~mask); }

static uint8_t rc_version(void) { return rc_read(RC_VERSION); }
static bool rc_present(void) { uint8_t v = rc_version(); return v != 0x00 && v != 0xFF; }

static bool rc522_init(void) {
  pin_lo(RC_RST); delay_ms(2);                       /* hard reset */
  pin_hi(RC_RST); delay_ms(50);                      /* oscillator start-up */
  rc_write(RC_COMMAND, RC_CMD_SOFTRESET);
  delay_ms(10);
  if (!rc_present()) return false;
  rc_write(RC_TMODE, 0x80);                          /* timer starts itself after each transmit */
  rc_write(RC_TPRESC, 0xA9);                         /* 13.56 MHz / (2*169+1) = 40 kHz, 25 us ticks */
  rc_write(RC_TRELOADH, 0x01);
  rc_write(RC_TRELOADL, 0x40);                       /* 320 ticks = 8 ms: no answer by then = no card */
  rc_write(RC_TXASK, 0x40);                          /* 100 % ASK modulation, required by ISO 14443A */
  rc_write(RC_MODE, 0x3D);                           /* CRC preset 0x6363 */
  rc_set(RC_TXCONTROL, 0x03);                        /* antenna on */
  return true;
}

/* Send tx[] to the card, wait for the answer. txbits = number of valid bits in
 * the last tx byte (0 = all 8, REQA uses 7). Returns false on timeout/error. */
static bool rc_transceive(const uint8_t *tx, uint8_t txlen, uint8_t txbits,
                          uint8_t *rx, uint8_t *rxlen, uint8_t *rxbits) {
  uint8_t rxmax = *rxlen;
  rc_write(RC_COMMAND, RC_CMD_IDLE);
  rc_write(RC_COMIRQ, 0x7F);                         /* clear every pending flag */
  rc_write(RC_FIFOLEVEL, 0x80);                      /* flush FIFO */
  for (uint8_t i = 0; i < txlen; i++) rc_write(RC_FIFODATA, tx[i]);
  rc_write(RC_BITFRAME, txbits);
  rc_write(RC_COMMAND, RC_CMD_TRANSCEIVE);
  rc_set(RC_BITFRAME, 0x80);                         /* StartSend */

  uint32_t s = micros();
  for (;;) {
    uint8_t irq = rc_read(RC_COMIRQ);
    if (irq & 0x30) break;                           /* RxIRq or IdleIRq: got something */
    if (irq & 0x01) return false;                    /* TimerIRq: nobody answered */
    if (micros() - s > 15000) return false;          /* belt and braces */
  }
  if (rc_read(RC_ERROR) & 0x1B) return false;        /* buffer overflow, collision, parity, protocol */
  uint8_t n = rc_read(RC_FIFOLEVEL);
  if (n > rxmax) n = rxmax;
  for (uint8_t i = 0; i < n; i++) rx[i] = rc_read(RC_FIFODATA);
  *rxlen = n;
  *rxbits = rc_read(RC_CONTROL) & 0x07;                    /* ControlReg: valid bits in last byte */
  return true;
}

/* A card that is IDLE answers REQA with its 2-byte ATQA. Cards that have
 * already been read ignore REQA until they leave the field, which is exactly
 * the "one tap, one event" behaviour we want. */
static bool rc_reqa(void) {
  uint8_t cmd = PICC_REQA, rx[2], n = 2, bits = 0;
  rc_clr(RC_COLL, 0x80);
  return rc_transceive(&cmd, 1, 7, rx, &n, &bits) && n == 2 && bits == 0;
}

/* Anticollision, cascade level 1: ask for the 4 UID bytes + BCC check byte. */
static rfid_res_t rc_read_uid(uint32_t *uid) {
  uint8_t cmd[2] = { PICC_SEL_CL1, 0x20 }, rx[5], n = 5, bits = 0;
  if (!rc_transceive(cmd, 2, 0, rx, &n, &bits) || n != 5 || bits != 0) return RFID_NONE;
  if ((uint8_t)(rx[0] ^ rx[1] ^ rx[2] ^ rx[3]) != rx[4]) return RFID_NONE;
  if (rx[0] == 0x88) return RFID_UNSUPPORTED;        /* cascade tag: 7 or 10 byte UID, not handled */
  *uid = ((uint32_t)rx[0] << 24) | ((uint32_t)rx[1] << 16) | ((uint32_t)rx[2] << 8) | rx[3];
  if (*uid == 0 || *uid == 0xFFFFFFFFUL) return RFID_NONE;   /* bus noise, not a card */
  return RFID_OK;
}

/* ---- door state, updated by rfid_service() from anywhere in the loop ---- */
static struct {
  roster_t roster;
  bool     ok;                /* reader answered the last probe */
  uint32_t last_uid;          /* 0 = nothing yet */
  const char *last_txt;       /* "IN", "OUT", "DENY", "FULL", "7B" */
  bool     boost_on;
  uint32_t boost_t0;
} rf = { .last_txt = "" };

static inline int  rf_count(void) { return roster_count(&rf.roster); }
static bool rf_boost(void) {
  if (rf.boost_on && micros() - rf.boost_t0 >= ENTRY_BOOST_MS * 1000UL) rf.boost_on = false;
  return rf.boost_on;
}

static void uart_puthex32(uint32_t v) {
  for (int i = 3; i >= 0; i--) uart_puthex8((uint8_t)(v >> (i * 8)));
}

/* Poll once. Cheap when nobody is there (about 8 ms for the timeout). */
static void rfid_service(void) {
  if (!rf.ok) return;
  if (!rc_reqa()) return;
  uint32_t uid = 0;
  rfid_res_t r = rc_read_uid(&uid);
  if (r == RFID_NONE) return;
  if (r == RFID_UNSUPPORTED) {
    rf.last_uid = 0; rf.last_txt = "7B";
    uart_puts("RFID: 7/10-byte UID card, not supported\r\n");
    return;
  }
  rf.last_uid = uid;
  uart_time(); uart_puts("RFID "); uart_puthex32(uid); uart_puts(" ");
  if (!uid_allowed(ALLOWED_UIDS, uid)) {
    rf.last_txt = "DENY";
    uart_puts("DENIED\r\n");
    return;
  }
  switch (roster_tap(&rf.roster, uid, micros(), TAP_LOCKOUT_MS * 1000UL)) {
    case TAP_IN:
      rf.last_txt = "IN";
      rf.boost_on = true; rf.boost_t0 = micros();
      uart_puts("IN");
      break;
    case TAP_OUT:
      rf.last_txt = "OUT";
      if (rf_count() == 0) rf.boost_on = false;
      uart_puts("OUT");
      break;
    case TAP_FULL:
      rf.last_txt = "FULL";
      uart_puts("ROSTER FULL");
      break;
    default:
      return;                                        /* same card still on the reader */
  }
  uart_puts("  inside="); uart_putu((uint32_t)rf_count()); uart_puts("\r\n");
}

/* delay_ms that keeps listening for cards while it waits */
static void wait_poll(uint32_t ms) {
  uint32_t s = micros(), us = ms * 1000UL;
  while (micros() - s < us) { rfid_service(); serial_service(); delay_ms(2); }
}

static void setup(void) {
  RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOBEN | RCC_AHB1ENR_GPIOCEN;
  RCC->APB1ENR |= RCC_APB1ENR_TIM2EN | RCC_APB1ENR_I2C1EN | RCC_APB1ENR_USART2EN |
                  RCC_APB1ENR_SPI2EN;

  TIM2->PSC = 16 - 1;
  TIM2->ARR = 0xFFFFFFFF;
  TIM2->EGR = TIM_EGR_UG;
  TIM2->CR1 = TIM_CR1_CEN;

  for (int z = 0; z < 2; z++) {
    pin_lo(TRIG[z]); pin_mode(TRIG[z], 1);
    pin_mode(ECHO[z], 0); pin_pull(ECHO[z], 2);
  }
  pin_od(DHT, true); pin_pull(DHT, 1); pin_hi(DHT); pin_mode(DHT, 1);
  pin_mode(LED, 1);
  pin_mode(LDR, 0); pin_pull(LDR, 1);   /* pull-up, same reasoning as the arduino build */

  pwm_init_leds();
  led_set(0, 0); led_set(1, 0);
  fan_init();                                       /* motors off from the first instant */

  uart_init();
  uart_puts("\r\nSmart Classroom - Nucleo-F401RE\r\n");

  rtc_ok = rtc_init();
  uart_puts("RTC: "); uart_datetime();
  if (rtc_ok && !rtc_lse) uart_puts("RTC: no 32.768 kHz crystal answered, using LSI\r\n");
#if USE_POT || USE_FAN_POT
  adc_init();
#endif
  uart_rx_init();
  button_init();
  uart_puts("Commands: T 08:30:00 | D 18/10/26 | ?\r\n");

  i2c_init();
  delay_ms(100);
  if (oled_probe_and_init()) {
    uart_puts("OLED found at 0x"); uart_puthex8(oled_addr); uart_puts("\r\n");
    for (uint8_t p = 0; p < 8; p++) oled_line(p, "");
  } else {
    uart_puts("OLED: no ACK - check wiring\r\n");
  }

  spi2_init();
  rf.ok = rc522_init();
  if (rf.ok) { uart_puts("RC522 found, version 0x"); uart_puthex8(rc_version()); uart_puts("\r\n"); }
  else uart_puts("RC522: no answer - check wiring (3V3 only!)\r\n");
  delay_ms(1000);
}

static char *put_hex8(char *b, uint8_t v) {
  const char *h = "0123456789ABCDEF";
  *b++ = h[v >> 4]; *b++ = h[v & 15];
  return b;
}
static char *put_hex32(char *b, uint32_t v) {
  const char *h = "0123456789ABCDEF";
  for (int i = 7; i >= 0; i--) *b++ = h[(v >> (i * 4)) & 15];
  return b;
}

int main(void) {
  setup();

  uint8_t temp = 0, hum = 0;
  bool dht_ok = false;
  fan_t fan = {0, false, 0};
  uint8_t fan_now = 0;
  uint8_t knob = 100;                               /* fan speed from the A2 pot; 100 if no pot */
  uint32_t last_dht = micros() - DHT_PERIOD_US;
  uint16_t cm[2];
  bool occ[2] = {false, false};
  uint32_t pending_since[2] = {0, 0};
  bool stale_timing = false;
  uint32_t stale_since = 0;
  char line[24];
  ovr_t ovr = OVR_AUTO;
  uint32_t btn_seen = btn_presses;
  uint16_t bright = BRIGHT_MAX;
  static const char *OVR_TXT[3] = { "AUTO", "ON", "OFF" };

  for (;;) {
    for (int z = 0; z < 2; z++) { cm[z] = read_cm(z); wait_poll(PING_GAP_MS); }

    if (micros() - last_dht >= DHT_PERIOD_US) {
      last_dht = micros();
      dht_ok = dht11_read(&temp, &hum);
      if (!oled_addr && oled_probe_and_init())
        for (uint8_t p = 0; p < 8; p++) oled_line(p, "");
      rf.ok = rf.ok ? rc_present() : rc522_init();   /* notice a reader that was unplugged or plugged in */
    }

    serial_service();

    uint32_t presses = btn_presses;                 /* one read: the ISR may bump it any time */
    if (presses != btn_seen) {
      ovr = (ovr_t)((ovr + (presses - btn_seen)) % 3);
      btn_seen = presses;
      uart_time(); uart_puts("Button: override "); uart_puts(OVR_TXT[ovr]); uart_puts("\r\n");
    }
#if USE_POT
    { uint16_t v; if (adc_read(0, &v)) bright = pot_brightness(v, bright); }
#endif
#if USE_FAN_POT
    { uint16_t v; if (adc_read(4, &v)) knob = fan_knob(v, knob); }
#endif

    bool dark = is_dark();
    bool boost = rf_boost();
    int occ_n = 0;
    for (int z = 0; z < 2; z++) {
      bool raw = (cm[z] != DIST_NO_ECHO && cm[z] != DIST_FAR && cm[z] < OCC_CM);
      if (raw != occ[z]) {
        if (pending_since[z] == 0) pending_since[z] = micros();
        uint32_t need_us = (raw ? OCC_MS : VACANT_MS) * 1000UL;
        if (micros() - pending_since[z] >= need_us) {
          occ[z] = raw;
          pending_since[z] = 0;
        }
      } else {
        pending_since[z] = 0;
      }
      if (occ[z]) occ_n++;
      bool occ_eff = occ[z] || boost;               /* a recent tap-in counts as "someone is here" */


      led_set(z, led_duty(ovr, mode[z], occ_eff, dark, bright, LED_DIM));
    }

    /* Fan: on while at least one card is tapped in, speed from the knob. */
    fan_now = fan_cmd(ovr, rf_count() > 0, knob);
    fan_set(fan_apply(&fan, fan_now, micros()));

    /* Everyone tapped in but the room has been empty for ages: they left without tapping out. */
    if (rf_count() > 0 && occ_n == 0 && !boost) {
      if (!stale_timing) { stale_timing = true; stale_since = micros(); }
      else if (micros() - stale_since >= STALE_RESET_MS * 1000UL) {
        roster_clear(&rf.roster);
        rf.last_txt = "RESET";
        stale_timing = false;
        uart_puts("RFID roster cleared: zones empty too long\r\n");
      }
    } else {
      stale_timing = false;
    }

    oled_line(0, dark ? "LIGHT: DARK" : "LIGHT: BRIGHT");
    rfid_service();
    {
      char *b = put_str(line, "IN:"); b = put_u(b, (uint32_t)rf_count(), 1); b = put_str(b, " ");
      b = rf.last_uid ? put_hex32(b, rf.last_uid) : put_str(b, "--------");
      b = put_str(b, " "); b = put_str(b, rf.last_txt); *b = 0;
    }
    oled_line(1, line);
    zone_line(line, 'A', cm[0], occ[0], mode[0]); oled_line(2, line);
    zone_line(line, 'B', cm[1], occ[1], mode[1]); oled_line(3, line);
    rfid_service();
    if (dht_ok) {
      char *b = line;
      b = put_str(b, "T:"); b = put_u(b, temp, 2); b = put_str(b, "C   H:");
      b = put_u(b, hum, 2); b = put_str(b, "%"); *b = 0;
    } else {
      put_str(line, "DHT11 ERROR")[0] = 0;
    }
    oled_line(5, line);
    rfid_service();
    if (rtc_ok) {
      dt_t t; rtc_read(&t);
      char *b = put_u2(line, t.d); *b++ = '/'; b = put_u2(b, t.mo); *b++ = ' ';
      b = put_u2(b, t.h); *b++ = ':'; b = put_u2(b, t.m); *b++ = ':'; b = put_u2(b, t.s);
      b = put_str(b, rtc_lse ? "" : " LSI"); *b = 0;
    } else {
      put_str(line, "RTC ERROR")[0] = 0;
    }
    oled_line(4, line);
    {
      /* "OCC1/2 AUTO L75 F60": occupancy, override, light %, fan % (0 = off) */
      char *b = put_str(line, "OCC"); b = put_u(b, occ_n, 1); b = put_str(b, "/2 ");
      b = put_str(b, OVR_TXT[ovr]); b = put_str(b, " L");
      b = put_u(b, (uint32_t)bright * 100 / BRIGHT_MAX, 1); b = put_str(b, " F");
      b = put_u(b, fan_now, 1); *b = 0;
    }
    oled_line(6, line);
    if (rf.ok) { char *b = put_str(line, "RFID OK V"); b = put_hex8(b, rc_version()); *b = 0; }
    else put_str(line, "RFID NOT FOUND")[0] = 0;
    oled_line(7, line);
    rfid_service();

    serial_service();
    if (rx_any && micros() - rx_last_us < RX_QUIET_US) goto heartbeat;   /* you are typing: keep the terminal clean */

    uart_time();
    const char *zn = "AB";
    for (int z = 0; z < 2; z++) {
      uart_putc(zn[z]); uart_putc('=');
      if (cm[z] == DIST_NO_ECHO) uart_puts("NOECHO");
      else if (cm[z] == DIST_FAR) uart_puts("FAR");
      else uart_putu(cm[z]);
      uart_puts(occ[z] ? "(OCC) " : "      ");
      uart_puts(MODE_TXT[mode[z]]); uart_puts("  ");
    }
    uart_puts("LDR="); uart_puts(dark ? "DARK" : "BRIGHT"); uart_puts("  ");
    if (dht_ok) { uart_puts("T="); uart_putu(temp); uart_puts(" H="); uart_putu(hum); }
    else uart_puts("DHT=ERR");
    uart_puts("  IN="); uart_putu((uint32_t)rf_count()); uart_puts(boost ? "(boost)" : "");
    uart_puts(rf.ok ? "  RFID=ok" : "  RFID=none");
    uart_puts("  OVR="); uart_puts(OVR_TXT[ovr]);
    uart_puts(" BRT="); uart_putu((uint32_t)bright * 100 / BRIGHT_MAX); uart_putc('%');
    uart_puts(" FAN="); uart_putu(fan_now); uart_putc('%');
    uart_puts(oled_addr ? "  OLED=ok\r\n" : "  OLED=none\r\n");

heartbeat:
    LED.port->ODR ^= (1U << LED.pin);
  }
}
