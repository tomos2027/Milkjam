cmake_minimum_required(VERSION 3.13)

#Milkjam: Raspberry Pi Pico 2 W (RP2350A + CYW43 WiFi) 対応 (IchigoJam BASICベース)
#※pico_sdk_import.cmakeのinclude時点でSDK初期化(ボード決定)が行われるため、必ずこれより前に置く
#  (コマンドラインの-DPICO_BOARD=...での上書きも可能)
set(PICO_BOARD pico2_w CACHE STRING "Board type" FORCE)

# initialize the SDK based on PICO_SDK_PATH
# note: this must happen before project()
include(pico_sdk_import.cmake)
include(pico_extras_import.cmake)

project(milkjam)

# initialize the Raspberry Pi Pico SDK
pico_sdk_init()

# DVI映像出力ピン (common_dvi_pin_configs.h):
# Pico 2 W (RP2350A) はGPIO0-29のみでGP32-39が存在しないため、Pico DVI sock準拠の
# pico_sock_cfg (TMDS: GP12/18/16 (+1が各々のN)、CLK: GP14/15) を使用する
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

target_include_directories(milkjam PUBLIC
	${CMAKE_CURRENT_LIST_DIR}
	# Make sure TinyUSB can find tusb_config.h
	${CMAKE_CURRENT_LIST_DIR}/src
	${CMAKE_CURRENT_LIST_DIR}/src/ntsc
)

# PicoDVI/software/apps/mandelbrot/CMakeLists.txt から引用
target_compile_definitions(milkjam PRIVATE
	DVI_DEFAULT_SERIAL_CONFIG=${DVI_DEFAULT_SERIAL_CONFIG}
	# 文字表示はモノクロ2bpp。TMDSバッファを1レーン分に削減し、全レーンで共有する
	DVI_MONOCHROME_TMDS=1
	# core0スタック予約を32KBに拡大 (複雑な式でスタック溢れしないように、IchigoJam_Pのmemmap拡大と同じ目的)
	PICO_STACK_SIZE=0x8000
	# core1 (DVIスキャンライン) のスタックは2KB明示指定 (PICO_CORE1_STACK_SIZEの省略時はPICO_STACK_SIZEに追随するため)
	PICO_CORE1_STACK_SIZE=0x800
)

# SCRATCH_Y (core0スタック領域) を64KBに拡大 (IchigoJam_Pのmemmap_ichigojam_p.ldの意図をRP2350のメモリマップで再現)
# RP2350 SRAM 520KB = RAM 452KB + SCRATCH_X 4KB + SCRATCH_Y 64KB
pico_set_linker_script_var(milkjam RAM_LENGTH 0x71000)
pico_set_linker_script_var(milkjam SCRATCH_X_ORIGIN 0x20071000)
pico_set_linker_script_var(milkjam SCRATCH_Y_ORIGIN 0x20072000)
pico_set_linker_script_var(milkjam SCRATCH_Y_LENGTH 0x10000)

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
	hardware_spi
	hardware_sleep
	hardware_pio
	pico_cyw43_arch_lwip_threadsafe_background
	pico_lwip_sntp
)

# WS.LED (WS2812B) はPIOで波形を生成する (CPUの長時間割込み禁止を避ける)
pico_generate_pio_header(milkjam ${CMAKE_CURRENT_LIST_DIR}/src/ws2812.pio)

# pico_enable_stdio_usb(milkjam 0)
pico_enable_stdio_uart(milkjam 1)

# create map/bin/hex file etc.
pico_add_extra_outputs(milkjam)
