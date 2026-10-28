# Smart Classroom: Nucleo-F401RE

2x HC-SR04 zones, DHT11, 2x LED, LM393 LDR, SH1106 OLED, RC522 RFID at the
door, light dimmer pot, on-chip RTC clock, manual override button, **one fan
motor (IRLZ44N, 9 V battery) with its own speed pot, running only while
someone is tapped in on the RFID**.

Bare-metal CMSIS, no HAL. HSI 16 MHz, no PLL. Interrupts: buttons (EXTI15_10,
plus EXTI9_5 in v2) and USART2 RX; everything else is polled.

**v2 (`FEATURES 1`, the default):** one extra button, five screens, card
names, teacher sessions with LATE marking, an event log and OLED graphs. The
blue button sets the fan level (the fan pot is not used in v2). See
"v2: screens, buttons, teacher sessions" below. `FEATURES 0` builds the v1
screen and behaviour (tag `v1-final` / `smart-classroom-v1-final.zip`).

Two zones instead of three: less wiring and lower cost per classroom.

## Wiring

Arduino-header labels where the pin has one. PB12-PB15 and PC5 are only
on the ST morpho headers; find them in the pin table of ST's UM1724.

| Part | Pin | Nucleo |
|---|---|---|
| Zone A HC-SR04 | TRIG / ECHO | D2 (PA10) / D3 (PB3) |
| Zone B HC-SR04 | TRIG / ECHO | D4 (PB5) / D5 (PB4) |
| Both HC-SR04 | VCC / GND | 5V / GND |
| DHT11 | DATA / VCC / GND | D8 (PA9) / 3V3 / GND |
| OLED | SCL / SDA / VCC / GND | D15 (PB8) / D14 (PB9) / 3V3 / GND |
| Zone A LED | anode via 220-330 ohm, cathode to GND | D9 (PC7), TIM3_CH2 |
| Zone B LED | anode via 220-330 ohm, cathode to GND | D10 (PB6), TIM4_CH1 |
| LM393 LDR module | DO / VCC / GND | A3 (PB0) / 3V3 / GND (AO unused) |
| **RC522 RFID** | SCK / MISO / MOSI | PB13 / PB14 / PB15 (SPI2) |
| | SDA (=SS) / RST | PB12 / PC5 |
| | 3.3V / GND | **3V3 only, never 5V** / GND (IRQ unused) |
| **Light pot (10k)** | wiper (middle) / ends | **A0 (PA0)** / 3V3 and GND |
| Override button | | B1, the blue button on the board (PC13). Nothing to wire |
| **Fan speed pot (10k)** | wiper (middle) / ends | **A2 (PA4)** / 3V3 and GND |
| **Fan gate** | via 220 ohm, 10k gate to GND | **D7 (PA8)**, TIM1_CH1 |
| RTC | | inside the STM32. Nothing to wire |
| **UI button** (v2) | one leg / other leg | **D11 (PA7)** / GND. Internal pull-up, no resistor |
| Fan speed pot (v2) | | not used in v2; leave A2 unconnected or wired, it is not read |

All grounds common.
ECHO pins are 5V tolerant, no divider needed.
LDR: DO reads HIGH when dark on our module. If a replacement module reads
the other way, change `is_dark()` in `main.c`.

## LED modes, pot and button

Set `ZONE_A_MODE` / `ZONE_B_MODE` at the top of `main.c` and reflash.

| Value | Mode | LED |
|---|---|---|
| 0 | OFF | always off |
| 1 | AUTO | on only if zone occupied **and** dark |
| 2 | DIM | ~10% always (never brighter than the pot) |
| 3 | MAX | on always |

**Pot** = how bright "on" is, for both zones, 5% to 100%. It never goes to 0;
switching off is the button's or the mode's job. Pot not wired yet? Set
`USE_POT 0` or A0 floats and the brightness wanders.

**Blue button (B1)** cycles a manual override for both LEDs, on an
interrupt: `AUTO` (modes above) -> `ON` (pot brightness, ignores sensors) ->
`OFF` -> `AUTO`. It doesn't survive a reset.

## Fan (motor)

One IRLZ44N stage per motor:

```
   9V (+) ───────────┬──────────────┐
                     │              │
                   MOTOR         1N4007   (stripe end to 9V +)
                     ├──────────────┘
                   DRAIN
   D7 ──[220]──┬── GATE    IRLZ44N   (writing facing you, legs down: G D S)
             [10k] SOURCE
               │     │
   Nucleo GND ─┴─────┴────── 9V (-)
```

- **9V (-) must join Nucleo GND.** 9V (+) goes nowhere near the Nucleo.
- The diode is required: without it the motor's turn-off spike kills the
  MOSFET or resets the board.
- The 10k keeps the motor off while the board boots or is being flashed.
- Keep motor wires away from the OLED's SCL/SDA.
- Test by hand first: gate wire to 3V3, motor spins; to GND, it stops.

Behaviour (same override button as the lights):

| | Fan |
|---|---|
| Override OFF | off |
| Override ON | runs at the knob's speed, even with nobody tapped in |
| AUTO, nobody tapped in on the RFID | off |
| AUTO, at least one card inside | runs at the knob's speed |
| Knob turned fully down (bottom ~5 %) | off |

The fan pot on **A2** sets speed from 35 % (slowest that reliably spins a
hobby motor) to 100 %. "100 %" is capped at `FAN_MAX_PCT` (60) of the
battery: about 5.4 V average, right for 3-6 V motors. Set it to 100 for 9 V
motors. Every start from stopped gets 0.3 s at full allowed speed so the
motor breaks free. PWM is 20 kHz, so no whine.

"Inside" is the RFID headcount, so the fan stops when the last card taps
out, or when the roster self-clears after both zones have been empty for
2 minutes (people who forgot to tap out). No fan pot wired yet? Set
`USE_FAN_POT 0` and it runs at full allowed speed when on.

## Clock (RTC)

The STM32's own RTC, clocked from the Nucleo's 32.768 kHz crystal (X2).

- On flashing, the clock starts from the **time you compiled**, so it is
  about 30 s behind. Set it exactly over serial (below).
- A plain reset (black button) keeps the time.
- **Unplugging USB loses the time.** The Nucleo ties the RTC's battery pin
  to the main 3.3 V, so there is no backup. After power-up it restarts from
  the compile time; set it again. A DS3231 module would fix this if needed.
- If the board has no crystal fitted (very old Nucleo revisions), it falls
  back to the internal LSI oscillator. The OLED clock line shows `LSI`.
  LSI is only good to a few minutes per day.

Set it from any serial terminal (PuTTY, Tera Term, Arduino serial monitor)
on the ST-LINK COM port, 115200, line ending CR or LF:

```
T 08:30:00      set time (T 08:30 also works)
D 18/10/26      set date (D 18/10/2026 also works)
?               print date and time
```

What you type is echoed, and the status line pauses for 5 s while you type.
Received characters arrive through the USART2 RX interrupt.

## Door RFID

Tap a card on the RC522 as you enter or leave.

- First time a card is seen it counts as **IN**. Next tap is **OUT**, and so on.
  The OLED line 2 shows `IN:<headcount> <last UID> <IN|OUT|DENY|FULL>`.
- A card resting on the reader counts once (same card ignored for 3 s after
  each sighting, `TAP_LOCKOUT_MS`).
- Taps do **not** switch lights. Lights follow the zone sensors only; the
  headcount drives the fan. (An earlier build also lit both zones for 30 s
  after a tap-in; that was removed.)
- If everyone is "inside" but both zones have been empty for 2 minutes
  (`STALE_RESET_MS`), the roster is cleared. This repairs people forgetting to
  tap out. The timer does not start until 30 s after the last tap-in
  (`ENTRY_BOOST_MS`), and in v2 it is 10 minutes while a class is live
  (`STALE_CLASS_MS`), so a teacher standing outside both zones does not
  end the class.
- Up to 8 cards tracked at once. Only 4-byte UIDs (MIFARE Classic, the kit
  card and tag) are handled; 7-byte cards show `7B` and are ignored.
- Restrict who counts: tap each card, read its UID off the UART
  (`RFID 3A4B5C6D IN inside=1`) and list them in `ALLOWED_UIDS` in `main.c`.
  Empty list = every card is accepted.

Limits to know about:

- RFID only sees people who tap. Someone walking in behind a person, or
  without a card, is invisible to it. The ultrasonic zones are the backstop,
  and the headcount is "cards inside", not a true head count.
- One reader with toggling means a missed tap flips the meaning of the next
  one. The stale reset repairs it eventually. For a clean count you need
  separate IN and OUT readers.
- Hold the card to the reader for about half a second. The loop checks the
  reader many times a second, but not during the DHT11 read or the UART print.

## Occupancy debounce

A zone only shows OCC after something has been within 100 cm for 5 s
straight (`OCC_MS`), and only goes back to empty after 10 s straight of
nothing (`VACANT_MS`). Any break in the streak restarts the timer.
Distance on the OLED/UART is live; the OCC flag and the LED follow the
debounced state.

## Build, test, flash

Self-contained project: `CMakeLists.txt`, `cmake/gcc-arm-none-eabi.cmake`,
`STM32F401RETX_FLASH.ld`, ST's `startup_stm32f401xe.s` and
`system_stm32f4xx.c` are all included. Needs `arm-none-eabi-gcc`, CMake,
Ninja and `STM32_Programmer_CLI` on PATH. From this folder, `flash.bat`
alone configures (first time), builds and flashes.

```
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake
flash.bat
```

`flash.bat` builds, then flashes over SWD with `STM32_Programmer_CLI`
(needs it on PATH, it lives inside the STM32CubeIDE install under
`plugins\...cubeprogrammer...\tools\bin`).

OpenOCD fails with `open failed` on this board's ST-LINK (2019 firmware).
Updating it via STM32CubeProgrammer -> Firmware upgrade should fix
OpenOCD and F5 debugging in VS Code; until then use `flash.bat`.

The headcount logic is in `Core/Inc/logic.h` and
tested on a laptop, no board needed:

```
gcc -Wall -Wextra -ICore/Inc tests/host_test.c -o host_test && ./host_test
```

## Screen (v1, `FEATURES 0`)

```
LIGHT: DARK
IN:2 3A4B5C6D IN
A: 45cm OCC AUT
B:FAR     AUT
18/10 08:30:12
T:30C   H:65%
OCC1/2 AUTO L75 F60
RFID OK V92
```

## v2: screens, buttons, teacher sessions

Two buttons, one knob:

| Control | Does |
|---|---|
| **UI button** (D11), tap | next screen: HOME -> ROSTER -> LOG -> CLASS -> GRAPH -> HOME (on release) |
| **UI button** (D11), hold 2 s | SELECT: the action of the current screen. `KEEP HOLDING...` shows while you hold; it fires at 2 s without waiting for release, and releasing after that does not change screen |
| **B1** (blue) | fan level 0 -> 33 -> 66 -> 100 -> 0, on any screen |
| **Light knob** (A0) | HOME: brightness. Other screens: the menu knob (pick / scroll / set) |

**Fan (v2).** The level only sets the speed; the fan runs only while at
least one card is tapped in (as before). The shown 33/66/100 are driven as
35/68/100 % of the allowed speed, because the motor stalls below about 35 %,
and the allowed speed is still capped at 60 % of the 9 V battery. Starts at 0
after a reset, so press B1 once to choose a speed. HOME shows
`FAN:66 RUN` or `FAN:66 STOP` (STOP = nobody tapped in).

**Lights (v2).** No override any more. Each zone follows its mode (AUTO:
on at the pot brightness when the zone is occupied and the room is dark).

Screens (row 0 is the inverted title bar, row 7 is the hint line; a
SELECT result shows there for 2.5 s):

| Screen | Shows | Light knob | SELECT |
|---|---|---|---|
| HOME | zones, last tap, T/H, light %, fan level, class status | brightness | shows a hint |
| ROSTER | everyone inside: name, time in, `T` teacher, `L` late | picks a row | force that person OUT |
| LOG | last 32 events, newest first: time, name, IN/OUT/LATE/START/END/DENY/FULL/FORCE/RESET | scrolls | prints the whole log as CSV on serial |
| CLASS | live: start time, minutes, present, late, inside now. Idle: last class summary | sets "late after" 0-30 min | saves it |
| GRAPH | last 18 min of temperature, humidity or headcount, one sample per 10 s, auto-scaled | left = TEMP, middle = HUM, right = PEOPLE | freeze / unfreeze (`HOLD`) |

**Brightness pickup.** When you come back to HOME the knob is wherever the
menus left it. The lights keep their brightness and HOME shows `L75*` until
the knob is turned to (or past) the current brightness; from then on it
controls the lights again. Nothing jumps.

**Teacher sessions.** Cards are named in `CARDS[]` in `main.c`; unnamed
cards show as `C-` plus the last 4 hex digits of the UID. The teacher card
is `62E26506`.

- Teacher taps IN: a class starts (`START`). Students already inside are
  on time.
- A student's **first** entry more than "late after" minutes after the start
  is `LATE`. Leaving and coming back keeps the first mark.
- Teacher taps OUT (or is forced out, or the roster self-clears): the class
  ends (`END`). Serial prints the summary and every student with first-in
  time and on time / LATE.
- Changing "late after" during a class applies to entries from then on.
  It is kept in RTC backup register 1, so it survives a reset; a new flash
  or a power cut returns it to 10 min.
- Sessions only record attendance. Lights and fan behave exactly as without
  a class.

**Serial commands (v2):** `R` roster, `L` log as CSV, `S` class status and
attendance, plus `T`, `D`, `?` as before.

**Display.** v2 draws into a 1 KB copy of the screen and sends only the
128-byte pages that changed, checking the RFID reader between pages.

## Debugging

- UART2 via the ST-LINK virtual COM port, 115200:
  `[08:30:12] A=45(OCC) AUT  B=FAR      AUT  LDR=DARK  T=30 H=65  IN=2(boost)  RFID=ok  OVR=AUTO BRT=75%  OLED=ok`
  Card taps and button presses print their own timestamped lines:
  `[08:30:14] RFID 3A4B5C6D IN inside=2`, `[08:31:02] Button: override ON`.
  v2 adds the card name and class events:
  `[09:02:00] RFID 62E26506 TEACHER IN CLASS START  inside=2`.
- v2 UI button does nothing: check D11 goes to GND through the button.
  The pin uses an internal pull-up; a button wired to 3V3 will never register.
- LD2 toggles every loop = firmware alive.
- `NO ECHO`: sensor never answered, check its TRIG/ECHO/5V wiring.
- `FAR`: sensor works, nothing within ~4 m.
- `DHT11 ERROR`: no reply or bad checksum, check D8 and 3V3.
- `OLED=none`: nothing ACKed at 0x3C/0x3D. Retries every 2 s, no reflash needed.
- `RFID NOT FOUND` / `RFID=none`: the RC522 version register read 0x00 or
  0xFF. Check 3V3, GND and the four SPI wires (MISO and MOSI swapped is the
  usual mistake). Retries every 2 s. A working chip reports version 0x91 or
  0x92; some clones report others and are fine.
- `RTC ERROR`: the RTC failed to start. Should not happen; reset the board.
- Button does nothing: check the UART for `Button:` lines. If they appear,
  the override is working and the mode logic is what you are seeing.
- Brightness jumps around: pot wiper not on A0, or an end of the pot not on
  3V3/GND.
