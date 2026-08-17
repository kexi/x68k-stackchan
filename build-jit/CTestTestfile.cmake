# CMake generated Testfile for 
# Source directory: /Users/kei/ghq/github.com/kexi/x68k-stackchan/test
# Build directory: /Users/kei/ghq/github.com/kexi/x68k-stackchan/build-jit
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(x68k_tests "/Users/kei/ghq/github.com/kexi/x68k-stackchan/build-jit/x68k_tests")
set_tests_properties(x68k_tests PROPERTIES  _BACKTRACE_TRIPLES "/Users/kei/ghq/github.com/kexi/x68k-stackchan/test/CMakeLists.txt;190;add_test;/Users/kei/ghq/github.com/kexi/x68k-stackchan/test/CMakeLists.txt;0;")
add_test(settled_is_unforgeable "/nix/store/f05vi9a1dyq8xrczrsc52rb4j5zmx6dm-cmake-4.1.2/bin/cmake" "--build" "/Users/kei/ghq/github.com/kexi/x68k-stackchan/build-jit" "--target" "x68k_compile_fail_settled" "--config" "RelWithDebInfo")
set_tests_properties(settled_is_unforgeable PROPERTIES  WILL_FAIL "TRUE" _BACKTRACE_TRIPLES "/Users/kei/ghq/github.com/kexi/x68k-stackchan/test/CMakeLists.txt;206;add_test;/Users/kei/ghq/github.com/kexi/x68k-stackchan/test/CMakeLists.txt;0;")
