@echo off
rem Builds, then flashes over SWD with STM32CubeProgrammer (STM32_Programmer_CLI must be on PATH).
if not exist build\build.ninja cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake || exit /b 1
cmake --build build || exit /b 1
STM32_Programmer_CLI -c port=SWD mode=UR -w build\nucleo_classroom.elf -v -rst
