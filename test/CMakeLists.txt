# x68k-stackchan — ホスト側ビルド (core/ の単体テストとエミュレータランナー)。
#
# ESP-IDF のビルド (ルートの CMakeLists.txt) とは独立したプロジェクトにしている。
# ESP-IDF の project.cmake はツリー全体を支配するので、同居させるとホスト側が壊れる。
#
# ここでビルドするのは src/x68k/core/ だけ。core/ は ESP32 非依存の純粋 C++17 で、
# 68000 もデバイスも Mac 上で動く。実機に焼かずに Human68k の起動までデバッグできる
# ことが開発速度を決めるので、この分離は本プロジェクトの中核。
cmake_minimum_required(VERSION 3.16)
project(x68k_host CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# clangd がインクルードパスと C++17 を解決できるようにする。
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(ENABLE_SANITIZERS "Build host tests with ASan/UBSan" OFF)

# 68000 エミュレータは符号付き/符号なしの取り違えと暗黙の切り詰めがバグの中心。
# -Wconversion / -Wsign-conversion を最初から有効にして、後から直す羽目を避ける。
add_compile_options(-Wall -Wextra -Wshadow -Wconversion -Wsign-conversion)

if(ENABLE_SANITIZERS)
  # ASan は EA 計算ミスによる配列外アクセスを、UBSan は符号付きオーバーフローや
  # 不正なシフト量を捕まえる。どちらも 68000 コアで踏みやすい種類のバグ。
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()

set(X68K_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

# --- core: ESP32 非依存のエミュレータ本体 -----------------------------------
add_library(x68k_core STATIC
  ${X68K_ROOT}/src/x68k/core/cpu/m68k.cpp
  ${X68K_ROOT}/src/x68k/core/cpu/m68k_ops_move.cpp
  ${X68K_ROOT}/src/x68k/core/cpu/m68k_ops_alu.cpp
  ${X68K_ROOT}/src/x68k/core/cpu/m68k_ops_misc.cpp
  ${X68K_ROOT}/src/x68k/core/cpu/m68k_ops_group4.cpp
  ${X68K_ROOT}/src/x68k/core/bus.cpp
  ${X68K_ROOT}/src/x68k/core/machine.cpp
  ${X68K_ROOT}/src/x68k/core/dev/sram.cpp
  ${X68K_ROOT}/src/x68k/core/dev/mfp.cpp
  ${X68K_ROOT}/src/x68k/core/dev/rtc.cpp
  ${X68K_ROOT}/src/x68k/core/dev/dmac.cpp
  ${X68K_ROOT}/src/x68k/core/dev/fdc.cpp
  ${X68K_ROOT}/src/x68k/core/dev/video.cpp
  ${X68K_ROOT}/src/x68k/core/video/text_raster.cpp
  ${X68K_ROOT}/src/x68k/core/video/cgrom_fallback.cpp
)
target_include_directories(x68k_core PUBLIC
  ${X68K_ROOT}/src/x68k/core
  ${X68K_ROOT}/src/x68k/core/cpu
)

# --- doctest (vendored) -----------------------------------------------------
add_library(doctest INTERFACE)
target_include_directories(doctest SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/doctest)

enable_testing()

add_executable(x68k_tests
  test_main.cpp
  test_memmap.cpp
  test_m68k_vectors.cpp
  test_sram.cpp
  test_bus.cpp
  test_text_raster.cpp
  test_cgrom_fallback.cpp
  test_boot.cpp
  test_sasi.cpp
  test_rtc.cpp
  test_dmac.cpp
  test_mfp.cpp
  test_fdc.cpp
)
target_link_libraries(x68k_tests PRIVATE x68k_core doctest)
add_test(NAME x68k_tests COMMAND x68k_tests)

# --- ホストのエミュレータランナー -------------------------------------------
# 実機に焼かずに IPL-ROM と Human68k の起動をデバッグするための入口。
# core/ が ESP32 非依存だからこそ成立する。
add_executable(x68k-run ${X68K_ROOT}/host/main.cpp)
target_link_libraries(x68k-run PRIVATE x68k_core)
