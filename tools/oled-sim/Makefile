# Host-side simulator: compiles firmware/Core/Src/main.c against RAM-backed registers.
CC      ?= gcc
CFLAGS  ?= -O1 -w
INC      = -Iinc -I../../firmware/Core/Inc -DSTM32F401xE

all: screens buttons
screens: screens.c regs.h inc/stm32f4xx.h ../../firmware/Core/Src/main.c
	$(CC) $(CFLAGS) $(INC) screens.c -o sim
	mkdir -p out && ./sim
buttons: buttons.c regs.h inc/stm32f4xx.h ../../firmware/Core/Src/main.c
	$(CC) $(CFLAGS) $(INC) buttons.c -o buttons && ./buttons
clean:
	rm -rf out sim buttons
.PHONY: all screens buttons clean
