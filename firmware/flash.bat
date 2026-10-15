@echo off
rem builds and flashes over SWD using STM32CubeProgrammer (same tool the Arduino IDE upload uses)
cmake --build build || exit /b 1
STM32_Programmer_CLI -c port=SWD mode=UR -w build\nucleo_classroom.elf -v -rst
