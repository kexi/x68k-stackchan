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
  #
  # ただし macOS (Darwin) では ASan を外し、UBSan だけにする。
  #
  # Why not macOS でも ASan を有効にするか:
  #   macOS 26 (Darwin 25) の dyld と ASan ランタイムが初期化中にデッドロックする。
  #   ASan の InitializeShadowMemory() が get_dyld_hdr() 経由で
  #   dyld_shared_cache_iterate_text_swift() を呼び、その内部の _Block_copy が
  #   malloc する。その malloc を ASan 自身の interceptor が捕まえて
  #   AsanInitFromRtl() へ再入し、初期化用の StaticSpinMutex を二重に取ろうとして
  #   sched_yield を回し続ける。main に入る前に固まるので --version すら返らない。
  #   Nix の clang 固有ではなく Apple 供給の /usr/bin/clang でも同一に再現するため、
  #   ツールチェーンの差し替え (clangUseLLVM 等) では解決しない。OS 側の問題。
  #
  # Why not サニタイザ自体を諦めるか:
  #   UBSan は単独なら macOS でも正常に動く (shadow memory を張らないので
  #   上記の初期化経路を通らない)。符号付きオーバーフローや不正シフト量の検出は
  #   68000 コアで最も価値が高い部分なので、手元でもここまでは回せるようにする。
  #   ASan の配列外アクセス検出は Linux の CI が担保する。
  #
  # Why not -lubsan が無いから UBSan も無理、としないか:
  #   あのリンクエラーは g++ の話。Nix の gcc は macOS 向けのサニタイザランタイムを
  #   同梱していない。clang は compiler-rt の
  #   libclang_rt.ubsan_osx_dynamic.dylib を持っているので問題なくリンクできる。
  #   test-san が clang++ を明示しているのはこのため。
  if(APPLE)
    set(X68K_SAN_FLAGS -fsanitize=undefined)
  else()
    set(X68K_SAN_FLAGS -fsanitize=address,undefined)
  endif()

  # Why -fno-sanitize-recover=all が要るか:
  #   UBSan は既定では検出しても報告を出して実行を続け、終了コードは 0 のまま。
  #   ctest は終了コードしか見ないので、UB を踏んでいるのにテストが緑になる。
  #   ASan は検出時に abort するのでこの差が出るのは UBSan 側だけだが、
  #   Linux/macOS で判定を揃えるため両方に付ける。
  add_compile_options(${X68K_SAN_FLAGS} -fno-sanitize-recover=all -fno-omit-frame-pointer -g)
  add_link_options(${X68K_SAN_FLAGS})
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
  ${X68K_ROOT}/src/x68k/core/dev/scc.cpp
  ${X68K_ROOT}/src/x68k/core/dev/opm.cpp
  ${X68K_ROOT}/src/x68k/core/dev/adpcm.cpp
  ${X68K_ROOT}/src/x68k/core/dev/sprite.cpp
  ${X68K_ROOT}/src/x68k/core/dev/video.cpp
  ${X68K_ROOT}/src/x68k/core/io/ascii_keymap.cpp
  ${X68K_ROOT}/src/x68k/core/video/text_raster.cpp
  ${X68K_ROOT}/src/x68k/core/video/graphic_raster.cpp
  ${X68K_ROOT}/src/x68k/core/video/sprite_raster.cpp
  ${X68K_ROOT}/src/x68k/core/video/text_scrape.cpp
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
  test_m68k_alu.cpp
  # メインメモリを仮想関数抜きで触る直接経路。速度のためだけに入れた経路なので、
  # バス経由と結果が変わらないことを実際の命令実行で押さえる。
  test_m68k_fastram.cpp
  test_sram.cpp
  test_sram_persist.cpp
  test_bus.cpp
  test_gvram_bus.cpp
  test_text_raster.cpp
  test_graphic_raster.cpp
  test_sprite.cpp
  test_text_scrape.cpp
  test_cgrom_fallback.cpp
  test_boot.cpp
  test_boot_disk.cpp
  test_sasi.cpp
  test_rtc.cpp
  test_dmac.cpp
  test_mfp.cpp
  test_fdc.cpp
  test_scc.cpp
  test_mouse_queue.cpp
  test_opm.cpp
  test_adpcm.cpp
  test_gui_smoke.cpp
  test_gui_demo.cpp
  test_app_mode.cpp
  test_avatar.cpp
  test_audio.cpp
  ${X68K_ROOT}/src/x68k/platform/key_queue.cpp
  # 音声の受け渡し。platform/ にあるが ESP-IDF に触らないのでホストで
  # ビルドできる (M5Unified を叩くのは speaker_m5.h の方)。実機では音を
  # 耳でしか確かめられないので、「キーオンしたら出口へ非無音のサンプルが
  # 届く」という完了条件の芯はここで検査する。
  ${X68K_ROOT}/src/x68k/platform/audio.cpp
  # 顔 ⇄ X68000 の切り替え FSM。platform/ にあるが ESP-IDF に触らないので
  # そのままホストでビルドできる (app_mode.h の冒頭を見よ)。
  # 切り替えの副作用 (マウスのボタンを離す、画面を作り直す) は実機に焼くと
  # 見落とすので、全遷移をここで回す。
  ${X68K_ROOT}/src/x68k/platform/app_mode.cpp
  # 顔の描画。platform/ にあるが M5GFX には触らず RGB565 の配列へ画素を
  # 置くだけなので、ホストでビルドできる (avatar.h の冒頭を見よ)。
  # 実機の LCD は直接見られないため、まばたきと表情が絵として出ているかを
  # 画素で検査できることがこの設計の目的。
  #
  # servo.h はヘッダオンリーの部分 (角度→パルス幅→デューティの変換) だけを
  # 使う。LEDC を叩く servo.cpp は ESP-IDF が要るのでここには入れない。
  ${X68K_ROOT}/src/x68k/platform/avatar.cpp
  # GUI デモの生成器。ランナー (--gui-demo) と同じ絵をテストが検査するため、
  # 実体を共有する。別々に持つと「目視した絵」と「テストが見た絵」がずれる。
  ${X68K_ROOT}/host/gui_demo.cpp
)
target_link_libraries(x68k_tests PRIVATE x68k_core doctest)
target_include_directories(x68k_tests PRIVATE ${X68K_ROOT}/host)
# sram_persist.h は platform/ にあるが ESP-IDF に触らないヘッダオンリーなので、
# ホストのテストから直接読める。sram.dat の保存/復元の方針はここにしか無く、
# 電源断の隙間を疑似ファイルシステムで再現して検査する必要がある。
target_include_directories(x68k_tests PRIVATE ${X68K_ROOT}/src/x68k/platform)

# key_queue.cpp (MouseQueue) を core/ のテストと一緒にビルドするための代役ヘッダ。
#
# MouseQueue は FreeRTOS のキューとミューテックスしか実機依存を持たない。
# freertos_shim/ に同名のヘッダを置いてインクルードパスを通せば、そのまま
# ホストでコンパイルできる。
#
# Why not platform/ 全体をテスト対象にしないか: display_lcd.cpp などは
# M5Unified と LovyanGFX を直接叩くので代役では済まない。MouseQueue は
# 「SCC に断られたレポートを差し戻して送り直す」という、実機に焼かないと
# 確かめられないと壊れたままになる判断を持つので、ここだけを引き上げる。
target_include_directories(x68k_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/freertos_shim)
add_test(NAME x68k_tests COMMAND x68k_tests)

# --- ホストのエミュレータランナー -------------------------------------------
# 実機に焼かずに IPL-ROM と Human68k の起動をデバッグするための入口。
# core/ が ESP32 非依存だからこそ成立する。
add_executable(x68k-run ${X68K_ROOT}/host/main.cpp ${X68K_ROOT}/host/gui_demo.cpp)
target_link_libraries(x68k-run PRIVATE x68k_core)
target_include_directories(x68k-run PRIVATE ${X68K_ROOT}/host)
