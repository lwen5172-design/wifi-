QT += core gui widgets network

CONFIG += c++17

TEMPLATE = app
TARGET = WifiDebugHelper

# 源码里有中文字符串，显式指定源文件/执行字符集为 UTF-8
QMAKE_CXXFLAGS += -finput-charset=UTF-8 -fexec-charset=UTF-8

INCLUDEPATH += qrcodegen

SOURCES += \
    main.cpp \
    mainwindow.cpp \
    wifiinfo.cpp \
    adbmanager.cpp \
    hdcmanager.cpp \
    qrcodegen/qrcodegen.cpp

HEADERS += \
    mainwindow.h \
    wifiinfo.h \
    adbmanager.h \
    hdcmanager.h \
    textdecode.h \
    qrcodegen/qrcodegen.hpp

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target