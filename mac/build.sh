#!/bin/sh
# 在 macOS 上构建 WifiDebugHelper
#
# 依赖：
#   - Xcode Command Line Tools（xcode-select --install）
#   - Qt 6（brew install qt，或 Qt Online Installer）
#
# 用法：
#   sh mac/build.sh            # Release
#   sh mac/build.sh debug      # Debug
set -e

cd "$(dirname "$0")/.."

CONFIG=release
[ "$1" = "debug" ] && CONFIG=debug

# Homebrew 装的 Qt 默认不在 PATH 里，先找一圈
QMAKE=${QMAKE:-}
if [ -z "$QMAKE" ]; then
    if command -v qmake >/dev/null 2>&1; then
        QMAKE=$(command -v qmake)
    else
        for candidate in \
            /opt/homebrew/opt/qt/bin/qmake \
            /usr/local/opt/qt/bin/qmake \
            "$HOME"/Qt/*/macos/bin/qmake; do
            if [ -x "$candidate" ]; then
                QMAKE=$candidate
                break
            fi
        done
    fi
fi

if [ -z "$QMAKE" ]; then
    echo "找不到 qmake。请先安装 Qt 6：brew install qt，或设置 QMAKE=/path/to/qmake" >&2
    exit 1
fi

echo "使用 qmake: $QMAKE"
"$QMAKE" WifiDebugHelper.pro -spec macx-clang "CONFIG+=$CONFIG"
make -j"$(sysctl -n hw.logicalcpu)"

echo
echo "构建完成：$(pwd)/WifiDebugHelper.app"
echo "运行：open WifiDebugHelper.app"
echo "打包（可选，需 Qt 自带的 macdeployqt）：macdeployqt WifiDebugHelper.app -dmg"