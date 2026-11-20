# WiFi 无线调试助手

Qt 6 Widgets 工具：**扫码配对**（和 Android Studio 一样的体验）+ **配对码配对**双通道，外加一个实时设备列表，支持多台设备。安卓走 `adb`，鸿蒙 NEXT 走 `hdc`。

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

## 鸿蒙 NEXT（hdc）

HarmonyOS NEXT 不跑 adb，也没有安卓那套二维码/配对码协议。程序用 `hdc` 作为第二个后端：

1. 手机上：设置 → 系统 → 开发者选项 → **无线调试**，打开后记下显示的 `IP:端口`
2. 在右侧「鸿蒙无线调试（hdc）」面板填入 `IP:端口`，点「连接」（等价于 `hdc tconn IP:端口`）
3. 设备出现在列表里（来源列显示 `hdc`），之后「连接 / 断开」都走 hdc

设备列表还会显示 `hdc discover`（局域网 UDP 广播）发现的鸿蒙设备，状态为「可连接（鸿蒙）」，双击即可连接。`hdc discover` 单次约 2 秒，程序每 16 秒才跑一次。

**hdc.exe 从哪来**：DevEco Studio 的 `sdk/default/openharmony/toolchains/hdc.exe`，或单独下载的 toolchains 包。程序会自动在 PATH、DevEco Studio 默认安装目录、`%LOCALAPPDATA%\Huawei\Sdk`、下载/桌面里找；找不到时点「选择 hdc.exe」指定路径（会记住）。

> `hdc tconn <key> -remove` 在 3.0.0b 上实测不生效，所以「断开」改用 `hdc kill -r` 重启 hdc 服务（会断开所有 hdc 连接），界面会先弹确认框。

## 设备列表

自动汇总四个来源：

- `adb devices -l` —— 已连接的安卓设备
- `adb mdns services` —— 局域网里发现但还没连接的安卓设备（状态显示「可连接」）
- `hdc list targets -v` —— 已连接的鸿蒙设备（USB / TCP，COM 串口会被过滤）
- `hdc discover` —— 局域网广播发现的鸿蒙设备（状态显示「可连接（鸿蒙）」）

同一台设备在 adb 里可能出现两条（TCP 入口 + mDNS 传输别名），程序会合并去重。列表每 4 秒自动刷新，双击行可直接连接，也可以选中后点「连接」/「断开」。支持同时管理多台设备，`来源` 列区分 adb / hdc。

## 限制

- **「无线调试」开关必须手动打开**，包括扫码配对也要先进到那个页面才扫得到码。系统安全边界，任何第三方程序都绕不过。
- 手机和电脑必须在同一个 WiFi 下。
- 安卓 / HarmonyOS 4 及以下走 adb；**HarmonyOS NEXT 只支持 IP:端口手动连接**，没有扫码或配对码（鸿蒙不提供这套协议）。
- adb 需要 platform-tools 31+（要支持 `adb mdns services`）。

## 构建

```bash
qmake WifiDebugHelper.pro -spec win32-g++
mingw32-make -j8
```

需要 Qt 6 + MinGW（本机为 Qt 6.12.0 / mingw1310_64），以及 `adb` 在 PATH 中（鸿蒙支持另需 `hdc.exe`）。

## 文件

| 文件 | 作用 |
| --- | --- |
| `main.cpp` | 程序入口 |
| `mainwindow.h/.cpp` | 界面：二维码、设备列表、配对面板、日志、扫码配对状态机 |
| `wifiinfo.h/.cpp` | 读取 Windows 当前 WiFi 的 SSID/密码/加密方式 |
| `adbmanager.h/.cpp` | 异步封装 `pair` / `connect` / `disconnect` / `mdns services` / `devices` |
| `hdcmanager.h/.cpp` | 异步封装 `tconn` / `list targets` / `discover` / `kill -r`，自动查找 hdc.exe |
| `textdecode.h` | 子进程输出解码：优先 UTF-8，失败回退系统本地编码 |
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