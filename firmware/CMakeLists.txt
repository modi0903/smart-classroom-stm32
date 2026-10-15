cmake_minimum_required(VERSION 3.20)

project(nucleo_classroom C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(EXECUTABLE ${PROJECT_NAME})

set(CPU_FLAGS "-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard")
set(MCU_DEFS  "-DSTM32F401xE")

set(COMMON_FLAGS
    "${CPU_FLAGS} ${MCU_DEFS} -Wall -Wextra -fdata-sections -ffunction-sections -g3"
)

set(CMAKE_C_FLAGS   "${COMMON_FLAGS} -std=gnu11")
set(CMAKE_ASM_FLAGS "${COMMON_FLAGS} -x assembler-with-cpp")
set(CMAKE_EXE_LINKER_FLAGS
    "${CPU_FLAGS} -Wl,--gc-sections -Wl,-Map=${PROJECT_NAME}.map --specs=nano.specs --specs=nosys.specs"
)

add_executable(${EXECUTABLE}
    Core/Src/main.c
    Core/Src/system_stm32f4xx.c
    startup/startup_stm32f401xe.s
)

target_include_directories(${EXECUTABLE} PRIVATE Core/Inc)
set_target_properties(${EXECUTABLE} PROPERTIES SUFFIX ".elf")

target_link_options(${EXECUTABLE} PRIVATE "-T${CMAKE_SOURCE_DIR}/linker/STM32F401RE_FLASH.ld")

add_custom_command(TARGET ${EXECUTABLE} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:${EXECUTABLE}> ${PROJECT_NAME}.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${EXECUTABLE}> ${PROJECT_NAME}.bin
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${EXECUTABLE}>
    COMMENT "Building ${PROJECT_NAME}.hex / .bin"
)
