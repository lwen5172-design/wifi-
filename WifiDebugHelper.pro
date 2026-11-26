QT += core gui widgets network

CONFIG += c++17

TEMPLATE = app
TARGET = WifiDebugHelper

# 源码里有中文字符串，显式指定源文件/执行字符集为 UTF-8
# （这些是 GCC 的选项，macOS 默认的 clang 不需要也不接受，只在 Windows 上加）
win32-g++|win32-clang-g++: QMAKE_CXXFLAGS += -finput-charset=UTF-8 -fexec-charset=UTF-8

INCLUDEPATH += qrcodegen

SOURCES += \
    main.cpp \
    mainwindow.cpp \
    wifiinfo.cpp \
    adbmanager.cpp \
    hdcmanager.cpp \
    iosmanager.cpp \
    qrcodegen/qrcodegen.cpp

HEADERS += \
    mainwindow.h \
    wifiinfo.h \
    macwifiparse.h \
    adbmanager.h \
    hdcmanager.h \
    iosmanager.h \
    textdecode.h \
    qrcodegen/qrcodegen.hpp

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

# ---------------------------------------------------------------- macOS
# 生成 .app 包；Info.plist 里补上读取 WiFi 名称、访问局域网需要的权限说明
macx {
    QMAKE_INFO_PLIST = mac/Info.plist
    DISTFILES += mac/build.sh
}