# 実機ファームのエントリコンポーネント。
#
# エミュレータ本体 (src/x68k/core/) は ESP32 非依存の純粋 C++17 として別に置き、
# ここからは platform/ 経由で駆動する。M0 時点ではまだ core/ が無いので
# 起動確認用の最小 main だけを持つ。
idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
)
