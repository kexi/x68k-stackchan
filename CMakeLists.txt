# x68k-stackchan — ESP-IDF プロジェクトのエントリポイント (実機ファーム)。
#
# ここは実機 (M5Stack CoreS3 / ESP32-S3) 向けのビルドだけを定義する。
# core/ をホストで検証するビルドは test/CMakeLists.txt が独立プロジェクトとして
# 持つ (`just test-host`)。両者を 1 つの CMakeLists に同居させると、
# ESP-IDF の project.cmake がツリー全体を支配してホスト側が壊れるため分けている。
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(x68k-stackchan)
