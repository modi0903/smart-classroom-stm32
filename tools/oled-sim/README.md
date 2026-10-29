# oled-sim

Runs the real firmware (`firmware/Core/Src/main.c`) on a PC.

`inc/stm32f4xx.h` replaces the device header when compiling here: it keeps ST's register layouts
but points every peripheral (`GPIOA`, `TIM2`, `RTC`, `EXTI`…) at a struct in RAM, defined in
`regs.h`. The firmware's drawing, session and button code then runs unchanged, and the program
drives it by writing those structs (setting the RTC time, advancing `TIM2->CNT`, changing a pin
in `GPIOA->IDR` and calling the interrupt handler).

```sh
make            # both
make screens    # renders every screen to out/*.pgm (4x scale)
make buttons    # tap / hold / bounce / missed-edge tests of the menu button
```

Needs only `gcc` and `make`. `.pgm` files open in GIMP, IrfanView or most image viewers; the
PNGs in `docs/images/screens` were made from the same output.

The sample class used by `screens`: a student taps in at 08:58, the teacher at 09:02, a student
at 09:07 (on time), one at 09:14 (late, limit 10 min), and the teacher leaves at 09:55.
