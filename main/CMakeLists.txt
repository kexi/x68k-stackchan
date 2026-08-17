# 実機ファームのエントリコンポーネント。
#
# core/ (ESP32 非依存のエミュレータ本体) と platform/ (ESP32 バックエンド) を
# ここからまとめてビルドする。core/ をコンポーネントに分けないのは、
# ホスト側のビルド (test/CMakeLists.txt) と実体を共有したいため。
# 分けると同じソース一覧を 2 か所で管理することになる。
set(X68K_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

idf_component_register(
    SRCS
        "main.cpp"
        # --- core: ESP32 非依存 ---
        "${X68K_ROOT}/src/x68k/core/cpu/m68k.cpp"
        "${X68K_ROOT}/src/x68k/core/cpu/m68k_ops_move.cpp"
        "${X68K_ROOT}/src/x68k/core/cpu/m68k_ops_alu.cpp"
        "${X68K_ROOT}/src/x68k/core/cpu/m68k_ops_misc.cpp"
        "${X68K_ROOT}/src/x68k/core/cpu/m68k_ops_group4.cpp"
        "${X68K_ROOT}/src/x68k/core/bus.cpp"
        "${X68K_ROOT}/src/x68k/core/machine.cpp"
        "${X68K_ROOT}/src/x68k/core/dev/sram.cpp"
        "${X68K_ROOT}/src/x68k/core/dev/mfp.cpp"
        "${X68K_ROOT}/src/x68k/core/dev/rtc.cpp"
        "${X68K_ROOT}/src/x68k/core/dev/dmac.cpp"
        "${X68K_ROOT}/src/x68k/core/dev/fdc.cpp"
        "${X68K_ROOT}/src/x68k/core/dev/iosc.cpp"
        "${X68K_ROOT}/src/x68k/core/dev/scc.cpp"
        "${X68K_ROOT}/src/x68k/core/dev/sprite.cpp"
        "${X68K_ROOT}/src/x68k/core/dev/opm.cpp"
        "${X68K_ROOT}/src/x68k/core/dev/adpcm.cpp"
        "${X68K_ROOT}/src/x68k/core/dev/video.cpp"
        "${X68K_ROOT}/src/x68k/core/io/ascii_keymap.cpp"
        "${X68K_ROOT}/src/x68k/core/video/text_raster.cpp"
        "${X68K_ROOT}/src/x68k/core/video/graphic_raster.cpp"
        "${X68K_ROOT}/src/x68k/core/video/sprite_raster.cpp"
        "${X68K_ROOT}/src/x68k/core/video/text_scrape.cpp"
        "${X68K_ROOT}/src/x68k/core/video/cgrom_fallback.cpp"
        # --- platform: ESP32 専用 ---
        "${X68K_ROOT}/src/x68k/platform/storage_sd.cpp"
        "${X68K_ROOT}/src/x68k/platform/display_lcd.cpp"
        "${X68K_ROOT}/src/x68k/platform/frame_channel.cpp"
        "${X68K_ROOT}/src/x68k/platform/key_queue.cpp"
        "${X68K_ROOT}/src/x68k/platform/input_touch.cpp"
        # 音声の受け渡し。ESP-IDF に触らないのでホストのテストと実体を共有する
        # (M5Unified を叩くのはヘッダオンリーの speaker_m5.h)。
        "${X68K_ROOT}/src/x68k/platform/audio.cpp"
        # --- スタックチャン: 顔 ⇄ X68000 の切り替え ---
        # app_mode.cpp と avatar.cpp は ESP-IDF に触らない (ホストのテストと
        # 実体を共有する)。顔は RGB565 の配列へ画素を置くだけで、LCD へ送るのは
        # DisplayLcd::pushFrame (avatar.h の冒頭を見よ)。
        # servo.cpp だけが ESP-IDF (LEDC) を必要とする。
        "${X68K_ROOT}/src/x68k/platform/app_mode.cpp"
        "${X68K_ROOT}/src/x68k/platform/avatar.cpp"
        "${X68K_ROOT}/src/x68k/platform/servo.cpp"
    INCLUDE_DIRS
        "."
        "${X68K_ROOT}/src/x68k/core"
        "${X68K_ROOT}/src/x68k/core/cpu"
        "${X68K_ROOT}/src/x68k/platform"
    REQUIRES
        # SD カードのマウントに使う。
        fatfs
        sdmmc
        esp_driver_sdspi
        esp_driver_spi
        # シリアルコンソール (ホストから X68000 へキーを送る)。
        esp_driver_usb_serial_jtag
        # 首振りサーボの PWM (servo.cpp)。
        esp_driver_ledc
        esp_driver_gpio
)

# ディスク読み出しの実時間を測る計測 (storage_sd.cpp)。
# 既定は 0。just measure-disk が -DX68K_MEASURE_DISK=1 で有効にする。
# 判断が済んだら消す。
if(DEFINED X68K_MEASURE_DISK)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE X68K_MEASURE_DISK=${X68K_MEASURE_DISK})
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE X68K_MEASURE_DISK=0)
endif()
