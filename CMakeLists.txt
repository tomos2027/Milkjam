cmake_minimum_required(VERSION 3.13)

#Milkjam: Pico W (RP2040 + CYW43 WiFi) 対応 (IchigoJam BASICベース)
#※pico_sdk_import.cmakeのinclude時点でSDK初期化(ボード決定)が行われるため、必ずこれより前に置く
set(PICO_BOARD pico_w CACHE STRING "Board type" FORCE)

# initialize the SDK based on PICO_SDK_PATH
# note: this must happen before project()
include(pico_sdk_import.cmake)
include(pico_extras_import.cmake)

project(milkjam)

# initialize the Raspberry Pi Pico SDK
pico_sdk_init()

set(DVI_DEFAULT_SERIAL_CONFIG "pico_sock_cfg" CACHE STRING
	"Select a default pin configuration from common_dvi_pin_configs.h" FORCE)
include_directories(
	PicoDVI/software/include
)
add_subdirectory(PicoDVI/software/libdvi)
add_subdirectory(PicoDVI/software/libsprite)

add_executable(milkjam
	src/main.c
	src/ntsc/rp2040_pwm_ntsc_textgraph.c
)

pico_set_linker_script(milkjam ${CMAKE_CURRENT_LIST_DIR}/memmap_ichigojam_p.ld)

target_include_directories(milkjam PUBLIC
	${CMAKE_CURRENT_LIST_DIR}
	# Make sure TinyUSB can find tusb_config.h
	${CMAKE_CURRENT_LIST_DIR}/src
	${CMAKE_CURRENT_LIST_DIR}/src/ntsc
)

# PicoDVI/software/apps/mandelbrot/CMakeLists.txt から引用
target_compile_definitions(milkjam PRIVATE
	DVI_DEFAULT_SERIAL_CONFIG=${DVI_DEFAULT_SERIAL_CONFIG}
)

target_link_libraries(milkjam PUBLIC
	pico_stdlib
	pico_multicore
	pico_util
	libdvi
	libsprite
	tinyusb_host
	tinyusb_board
	hardware_flash
	hardware_watchdog
	hardware_adc
	hardware_pwm
	hardware_dma
	hardware_i2c
	hardware_sleep
	pico_cyw43_arch_lwip_threadsafe_background
	pico_lwip_sntp
)

# pico_enable_stdio_usb(milkjam 0)
pico_enable_stdio_uart(milkjam 1)

# create map/bin/hex file etc.
pico_add_extra_outputs(milkjam)
