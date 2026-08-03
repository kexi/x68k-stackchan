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
        "${X68K_ROOT}/src/x68k/core/dev/video.cpp"
        "${X68K_ROOT}/src/x68k/core/video/text_raster.cpp"
        "${X68K_ROOT}/src/x68k/core/video/cgrom_fallback.cpp"
        # --- platform: ESP32 専用 ---
        "${X68K_ROOT}/src/x68k/platform/storage_sd.cpp"
        "${X68K_ROOT}/src/x68k/platform/display_lcd.cpp"
        "${X68K_ROOT}/src/x68k/platform/input_touch.cpp"
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
)
