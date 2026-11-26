#ifndef IOSMANAGER_H
#define IOSMANAGER_H

#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>

// 一台通过 USB/网络连接到本机的 iOS 设备
struct IosDevice
{
    QString udid;              // 序列号 / UniqueDeviceID
    QString connection;        // USB / Network
    QString name;              // iPhone / 自定义设备名
    QString productType;       // iPhone13,2
    QString version;           // 18.1
    bool paired = false;       // 是否能从 lockdownd 读到信息（手机需信任此电脑）
};

Q_DECLARE_METATYPE(IosDevice)

// iOS 设备发现。
//
// iOS 没有 adb/hdc 那样的命令行工具，但系统里有 usbmuxd 服务：
// Windows 听 127.0.0.1:27015，macOS 走 /var/run/usbmuxd 这个 Unix 域套接字。
// 这里直接说它的简单二进制协议：
//   1. ListDevices：列出当前连接的设备（UDID、连接方式）
//   2. 对每台设备 Connect 到 lockdownd(62078)，用 plist 问 DeviceName / ProductType 等
// 全部异步，结果通过信号返回；找不到 usbmuxd（没装苹果驱动）时静默返回空列表。
class IosManager : public QObject
{
    Q_OBJECT

public:
    explicit IosManager(QObject *parent = nullptr);

    // usbmuxd 是否可用（装了苹果移动设备驱动就有）
    bool isAvailable() const;

    // 枚举设备并读取名称（每台设备一次 lockdownd 往返）
    void listDevices();

signals:
    void logMessage(const QString &text);
    void devicesListed(const QList<IosDevice> &devices);
    void availabilityChecked(bool available);

private:
    struct DeviceEntry
    {
        quint32 deviceId = 0;
        QString udid;
        QString connection;
    };

    // 走 usbmuxd 的 ListDevices，拿到设备列表
    bool fetchDeviceList(QList<DeviceEntry> *out, QString *error) const;
    // 连接到某台设备的 lockdownd 并取设备信息；失败时只保留 UDID
    IosDevice enrichDevice(const DeviceEntry &entry) const;

    bool m_available = true;
};

#endif // IOSMANAGER_H
