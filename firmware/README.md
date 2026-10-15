# Smart Classroom: Nucleo-F401RE

2x HC-SR04 zones, DHT11, 2x LED, LM393 LDR, SH1106 OLED, **RC522 RFID at the
door**.

Bare-metal CMSIS, no HAL. HSI 16 MHz, no PLL, no interrupts.

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

All grounds common.
ECHO pins are 5V tolerant, no divider needed.
LDR: DO reads HIGH when dark on our module. If a replacement module reads
the other way, change `is_dark()` in `main.c`.

## LED modes

Set `ZONE_A_MODE` / `ZONE_B_MODE` at the top of `main.c` and reflash.

| Value | Mode | LED |
|---|---|---|
| 0 | OFF | always off |
| 1 | AUTO | full on only if zone occupied (or entry boost) **and** dark |
| 2 | DIM | ~10% always |
| 3 | MAX | full on always |

## Door RFID

Tap a card on the RC522 as you enter or leave.

- First time a card is seen it counts as **IN**. Next tap is **OUT**, and so on.
  The OLED line 2 shows `IN:<headcount> <last UID> <IN|OUT|DENY|FULL>`.
- A card resting on the reader counts once (same card ignored for 3 s after
  each sighting, `TAP_LOCKOUT_MS`).
- A tap-in makes **both zones count as occupied for 30 s** (`ENTRY_BOOST_MS`), so
  the lights react at the door without waiting for the 5 s debounce.
  After that the ultrasonic sensors take over.
- If everyone is "inside" but both zones have been empty for 2 minutes
  (`STALE_RESET_MS`), the roster is cleared. This repairs people forgetting to
  tap out.
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

## Screen

```
LIGHT: DARK
IN:2 3A4B5C6D IN
A: 45cm OCC AUT
B:FAR     AUT

T:30C   H:65%
Occupied: 1/2
RFID OK V92
```

## Debugging

- UART2 via the ST-LINK virtual COM port, 115200:
  `A=45(OCC) AUT  B=FAR      AUT  LDR=DARK  T=30 H=65  IN=2(boost)  RFID=ok  OLED=ok`
  Card taps print their own line: `RFID 3A4B5C6D IN inside=2`.
- LD2 toggles every loop = firmware alive.
- `NO ECHO`: sensor never answered, check its TRIG/ECHO/5V wiring.
- `FAR`: sensor works, nothing within ~4 m.
- `DHT11 ERROR`: no reply or bad checksum, check D8 and 3V3.
- `OLED=none`: nothing ACKed at 0x3C/0x3D. Retries every 2 s, no reflash needed.
- `RFID NOT FOUND` / `RFID=none`: the RC522 version register read 0x00 or
  0xFF. Check 3V3, GND and the four SPI wires (MISO and MOSI swapped is the
  usual mistake). Retries every 2 s. A working chip reports version 0x91 or
  0x92; some clones report others and are fine.
