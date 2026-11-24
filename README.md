# WiFi 无线调试助手

Qt 6 Widgets 工具：**扫码配对**（和 Android Studio 一样的体验）+ **配对码配对**双通道，外加一个实时设备列表，支持多台设备。安卓走 `adb`，鸿蒙 NEXT 走 `hdc`，iPhone 走系统自带的 usbmuxd。

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

1. 手机上：设置 → 系统 → 开发者选项 → **无线调试**，打开即可
2. 把手机无线调试页显示的 `IP:端口` 填进右侧「鸿蒙无线调试（hdc）」面板，点「连接」（等价于 `hdc tconn IP:端口`）
3. 连上后设备会出现在列表里，**型号列显示设备名**（如 `HUAWEI Mate 60 Pro`，通过 `hdc shell param get const.product.name` 查询），下次启动自动重连
4. 如果程序自己能发现设备（UDP 单播探测有效时），会自动以「可连接（鸿蒙）」列出来，双击即可连接，不用手填

**为什么不用 `hdc discover`，也不用安卓那套**：

- 安卓的自动发现靠 mDNS（`adbd` 广播 `_adb-tls-connect._tcp`），`adb mdns services` 直接能列出来。**鸿蒙的 hdc 里没有任何 mDNS 实现**（官方源码全量搜 `mdns`/`bonjour`/`dns_sd`/`avahi` 零命中），所以安卓那套无法复用。
- hdc 自己的发现机制是往 UDP 8710 发 `OHOS HDC` 广播，daemon 回 `OHOS HDC-<端口>`。程序自己实现了这套协议，并改成**逐地址单播**（部分 hdc 版本发不出正确广播，路由器也常过滤广播和组播）。
- ⚠️ 部分固件上这条路也不通：实测 HarmonyOS 7（API 26）的 hdcd 虽然绑定了 UDP 8710，但**从不读取**（接收队列只涨不减，30 秒不消费），`hdc discover` 恒为 0，单播探测也无应答。这是设备侧的问题，程序无法绕过。

**端口变了怎么办**：手机每次重新打开「无线调试」，调试端口都会换一个。程序会记住上次连上的 `IP:端口`，下次启动自动重连；如果端口已经变了，在设备列表里**右键那一行 → 「修改端口…」**，填上手机无线调试页显示的新端口，会立刻按新地址重连并更新记忆。右键菜单还有「重新连接」和「复制地址」。

> 曾经试过自动扫描端口段来找回设备，但全段扫描要 60 秒，并发高了还会把手机的调试服务扫挂，而且端口本来就显示在手机屏幕上，直接改一下更快，所以没有保留这个功能。

**记住地址**：每次连上都会把 `IP:端口` 记到设置里（同一 IP 只保留最新端口），下次启动自动重连；列表里显示「可连接（上次连接过）」，直接用 TCP 探测该地址是否还在线，不需要 UDP 回包。点「断开」会同时忘掉这个地址。若记住的地址全部连不上，日志会提示「鸿蒙每次重开无线调试端口都会变，请在手机上看新端口」。

**hdc.exe 从哪来**：DevEco Studio 的 `sdk/default/openharmony/toolchains/hdc.exe`，或单独下载的 toolchains 包。程序会自动在 PATH、DevEco Studio 默认安装目录、`%LOCALAPPDATA%\Huawei\Sdk`、下载/桌面里找；找不到时点「选择 hdc.exe」指定路径（会记住）。

> `hdc tconn <key> -remove` 在 3.0.0b 上实测不生效，所以「断开」改用 `hdc kill -r` 重启 hdc 服务（会断开所有 hdc 连接），界面会先弹确认框。

## 设备列表

自动汇总四个来源：

- `adb devices -l` —— 已连接的安卓设备
- `adb mdns services` —— 局域网里发现但还没连接的安卓设备（状态显示「可连接」）
- `hdc list targets -v` —— 已连接的鸿蒙设备（USB / TCP，COM 串口会被过滤）
- 程序自带的 UDP 单播扫描 —— 局域网里的鸿蒙设备（状态显示「可连接（鸿蒙）」）
- 记住的鸿蒙地址 —— TCP 探测还在线的（状态显示「可连接（上次连接过）」）
- **usbmuxd** —— 通过 USB / Wi-Fi 连到本机的 iPhone、iPad

同一台设备在 adb 里可能出现两条（TCP 入口 + mDNS 传输别名），程序会合并去重；鸿蒙端口换过之后，同一 IP 的旧 `Offline` 条目也不会再显示。列表每 4 秒自动刷新，双击行可直接连接，也可以选中后点「连接」/「断开」。`来源` 列区分 adb / hdc / ios。鸿蒙设备的行可以**右键**：修改端口、重新连接、复制地址。

**iOS 设备**：不依赖 adb/hdc，直接说苹果驱动自带的 usbmuxd 协议（`127.0.0.1:27015`），再去 lockdownd 读 `DeviceName` / `ProductType` / `ProductVersion`。装了「Apple 设备」驱动就有，不需要 libimobiledevice。手机第一次连时需要点「信任此电脑」，否则只能显示 UDID 和「未信任」状态。当前只做设备识别，不提供安装、截图、日志等调试功能。

**设备名称列**：adb 设备显示「厂商 型号」（如 `OPPO PKB110`，由 `getprop ro.product.manufacturer` + `ro.product.model` 拼成）；鸿蒙设备显示 `const.product.name`（如 `HUAWEI Mate 60 Pro`）。名称是异步查询的，先显示已有信息，查到后自动刷新。

**连接方式列**：`TCP`（无线）/ `USB`。

## 限制

- **「无线调试」开关必须手动打开**，包括扫码配对也要先进到那个页面才扫得到码。系统安全边界，任何第三方程序都绕不过。
- 手机和电脑必须在同一个 WiFi 下，且路由器没有开启「AP 隔离」（客户端之间无法互访）。
- 安卓 / HarmonyOS 4 及以下走 adb；**HarmonyOS NEXT 只支持 IP:端口手动连接**，没有扫码或配对码（鸿蒙不提供这套协议）。
- adb 需要 platform-tools 31+（要支持 `adb mdns services`）。

## 构建

```bash
qmake WifiDebugHelper.pro -spec win32-g++
mingw32-make -j8
```

需要 Qt 6 + MinGW（本机为 Qt 6.12.0 / mingw1310_64；用到 Qt Network，`QT += ... network`），以及 `adb` 在 PATH 中（鸿蒙支持另需 `hdc.exe`）。

## 文件

| 文件 | 作用 |
| --- | --- |
| `main.cpp` | 程序入口 |
| `mainwindow.h/.cpp` | 界面：二维码、设备列表、配对面板、日志、扫码配对状态机 |
| `wifiinfo.h/.cpp` | 读取 Windows 当前 WiFi 的 SSID/密码/加密方式 |
| `adbmanager.h/.cpp` | 异步封装 `pair` / `connect` / `disconnect` / `mdns services` / `devices` |
| `hdcmanager.h/.cpp` | 异步封装 `tconn` / `list targets` / `kill -r`；UDP 单播发现、记住地址并自动重连、自动查找 hdc.exe |
| `iosmanager.h/.cpp` | 直接实现 usbmuxd + lockdownd 协议，枚举 iPhone/iPad 并读取名称与系统版本 |
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