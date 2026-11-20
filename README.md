# WiFi 无线调试助手

Qt 6 Widgets 工具：**扫码配对**（和 Android Studio 一样的体验）+ **配对码配对**双通道，外加一个实时设备列表，支持多台设备。

## 两种配对方式

### 1. 扫码配对（推荐）

手机 `设置 → 开发者选项 → 无线调试 → 使用二维码配对设备`，扫程序里「扫码配对」页的二维码。**配对和连接一步完成，不用输配对码、不用看端口。**

原理（不是自己实现加密协议，所有握手都交给 adb）：

1. 程序生成随机服务名和密码，拼成 `WIFI:T:ADB;S:<名字>;P:<密码>;;` 显示成二维码
2. 手机扫码后，在局域网广播 `_adb-tls-pairing._tcp` 服务，实例名就是那个 `<名字>`，密码作为配对凭据
3. 程序每秒轮询 `adb mdns services`，发现该服务后执行 `adb pair <ip:port> <密码>`
4. 配对成功后继续监听 `_adb-tls-connect._tcp`，自动 `adb connect` 完成连接

SPAKE2 + TLS 的加密握手由 `adb pair` 内部完成，程序只负责生成码、发现服务、调用 adb。

### 2. 配对码配对（备选）

在无线调试页点「使用配对码配对设备」，把手机显示的 `IP:端口` 和 6 位配对码填进右侧面板，点「配对」。

### 3. WiFi 入网（手机不在同一网络时）

「WiFi 入网」页显示当前 WiFi 的二维码，手机扫码自动入网。这是扫码配对的前提。

## 设备列表

自动汇总两个来源：

- `adb devices -l` —— 已连接的设备
- `adb mdns services` —— 局域网里发现但还没连接的设备（状态显示「可连接」）

同一台设备在 adb 里可能出现两条（TCP 入口 + mDNS 传输别名），程序会合并去重。列表每 4 秒自动刷新，双击行可直接连接，也可以选中后点「连接」/「断开」。支持同时管理多台设备。

## 限制

- **「无线调试」开关必须手动打开**，包括扫码配对也要先进到那个页面才扫得到码。系统安全边界，任何第三方程序都绕不过。
- 手机和电脑必须在同一个 WiFi 下。
- 只支持 **adb（Android / HarmonyOS 4 及以下）**。HarmonyOS NEXT 用 hdc（`hdc tconn <ip>:<port>`），没有对应的二维码配对协议。
- 需要 platform-tools 31+（要支持 `adb mdns services`）。

## 构建

```bash
qmake WifiDebugHelper.pro -spec win32-g++
mingw32-make -j8
```

需要 Qt 6 + MinGW（本机为 Qt 6.12.0 / mingw1310_64），以及 `adb` 在 PATH 中。

## 文件

| 文件 | 作用 |
| --- | --- |
| `main.cpp` | 程序入口 |
| `mainwindow.h/.cpp` | 界面：二维码、设备列表、配对面板、日志、扫码配对状态机 |
| `wifiinfo.h/.cpp` | 读取 Windows 当前 WiFi 的 SSID/密码/加密方式 |
| `adbmanager.h/.cpp` | 异步封装 `pair` / `connect` / `disconnect` / `mdns services` / `devices` |
| `qrcodegen/` | [Nayuki 的 QR Code 生成库](https://www.nayuki.io/page/qr-code-generator-library)（MIT） |

## 安全说明

- WiFi 密码只在进程内存中用于生成二维码，不显示、不落盘。
- 配对码和配对密码不进日志。
- 二维码里的配对密码是一次性的，每次点「重新生成配对码」都会换新的。

## 两个环境坑（已处理）

- `netsh` 在无控制台的 GUI 进程里输出 UTF-8，在控制台里输出系统 ANSI 编码 —— `wifiinfo.cpp` 做了兼容解码。
- `QProcess` 传 `name="xxx"` 时内嵌引号会被转义坏导致 netsh 报错 —— 改用不带引号的 `name=xxx`。

## 致谢

扫码配对的思路来自 [aleixrodriala/adb-qr](https://github.com/aleixrodriala/adb-qr)（MIT），关键洞察是：**用 `adb mdns services` 做发现，不用自己写 mDNS 监听器**。