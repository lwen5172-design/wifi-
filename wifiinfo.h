#ifndef WIFIINFO_H
#define WIFIINFO_H

#include <QString>

// 读取当前连接的 WiFi 信息，并生成手机可识别的 WiFi 二维码内容。
// Windows 走 netsh；macOS 走 ipconfig / networksetup + 钥匙串（security）。
// 注意：密码只在进程内存中流转，不写日志、不显示到界面。
class WifiInfo
{
public:
    // 当前连接的 SSID，读不到返回空
    static QString currentSsid();

    // 读取当前 WiFi 的 SSID / 密码 / 加密方式。成功返回 true。
    static bool currentProfile(QString *ssid, QString *password, QString *authType);

    // 生成 WIFI:T:WPA;S:ssid;P:password;; 形式的内容
    static QString buildWifiQrText(const QString &ssid,
                                   const QString &password,
                                   const QString &authType);

    // 按 WiFi 二维码规范转义 \ ; , : "
    static QString escapeField(const QString &value);
};

#endif // WIFIINFO_H