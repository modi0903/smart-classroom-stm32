# Testing

Three levels, from fastest to slowest.

## 1. Host tests: 551 checks on a PC

`logic.h` and `extras.h` contain all decision logic and no register access, so they compile
with any C compiler:

```sh
gcc -Wall -Wextra -Ifirmware/Core/Inc firmware/tests/host_test.c -o host_test && ./host_test
# 551 checks, 0 failed
```

They cover:

- **Roster**: toggle, 3 s lockout that refreshes while a card rests on the reader, full roster,
  slot reuse, TIM2 wrap-around.
- **Allow list.**
- **Light duty** for every mode × override × occupancy × darkness, and the potentiometer
  mapping with its dead band.
- **Fan**: levels, RFID gating, kick-start (including across a timer wrap), the 60 % cap.
- **Dates**: BCD, weekday, leap years, `T`/`D` parsing with invalid input, the compiler build
  time, the FNV-1a fingerprint.
- **v2**: card names, sessions and LATE marking (on time at exactly the limit, late one second
  after, re-entry keeps the mark, across midnight), the event log ring, knob steps with
  hysteresis, brightness pickup, graph scaling.

## 2. The PC simulator

[`tools/oled-sim`](../tools/oled-sim) compiles the real `firmware/Core/Src/main.c` on a PC,
with every peripheral replaced by a struct in RAM:

```sh
make -C tools/oled-sim
```

- **`screens`** plays a sample class and writes every screen as a `.pgm` image from the
  firmware's own drawing code. It checks the class summary (3 present, 1 late).
- **`buttons`** feeds simulated edges into the real `EXTI9_5_IRQHandler` and
  `ui_button_poll()`:
  - a clean tap and a bouncy tap (counted once)
  - a 2 s hold (fires while held; the release does nothing)
  - a 1 s press (still a tap)
  - a press the ISR missed (the poll re-syncs)
  - wrapping from GRAPH back to HOME

Both run on every push in GitHub Actions, together with the firmware build and the host tests.

## 3. On the board

| Action | Expected |
|---|---|
| Stand in zone A for 5 s with the LDR covered | `A: ..cm OCC`, zone A light on |
| Leave zone A for 10 s | OCC clears, light off |
| Turn the light knob on HOME (zone lit) | Brightness and `L%` follow |
| Tap a card, tap again | `IN` with count 1, then `OUT` with 0 |
| Tap in, press B1 once | `FAN:33 RUN`, fan kicks then runs |
| Tap D11; hold D11 2 s | Next screen; that screen's action |
| Teacher card, then a student after the late limit | `CLASS START`; student `LATE`, `L` on ROSTER |
| Type `T 10:15` | Clock shows 10:15:00 |
| Unplug the DHT11 | `DHT ERR`, everything else keeps running |
| Unplug the OLED, plug it back | Display returns within 2 s |
