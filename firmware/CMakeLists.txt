cmake_minimum_required(VERSION 3.20)
project(nucleo_classroom C ASM)

set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE MinSizeRel)
endif()

add_executable(nucleo_classroom
  Core/Src/main.c
  Core/Src/system_stm32f4xx.c
  Core/Src/crt_stubs.c
  Core/Src/startup_stm32f401xe.s
)
target_include_directories(nucleo_classroom PRIVATE Core/Inc)
target_compile_definitions(nucleo_classroom PRIVATE STM32F401xE)
target_link_options(nucleo_classroom PRIVATE
  -T${CMAKE_SOURCE_DIR}/STM32F401RETX_FLASH.ld
  -Wl,-Map=${CMAKE_BINARY_DIR}/nucleo_classroom.map)
set_target_properties(nucleo_classroom PROPERTIES SUFFIX ".elf" LINK_DEPENDS ${CMAKE_SOURCE_DIR}/STM32F401RETX_FLASH.ld)

add_custom_command(TARGET nucleo_classroom POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:nucleo_classroom> ${CMAKE_BINARY_DIR}/nucleo_classroom.hex
  COMMAND ${CMAKE_SIZE} $<TARGET_FILE:nucleo_classroom>)
