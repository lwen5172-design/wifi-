#ifndef ADBMANAGER_H
#define ADBMANAGER_H

#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

// 一台通过 adb 看到的设备
struct AdbDevice
{
    QString serial;   // 192.168.1.5:37000 或 USB 序列号
    QString state;    // device / offline / unauthorized
    QString model;    // 解析自 model:xxx
};

// `adb mdns services` 里的一条记录
struct MdnsService
{
    QString instance; // 服务实例名
    QString type;     // _adb-tls-pairing._tcp / _adb-tls-connect._tcp
    QString ip;
    int port = 0;

    QString endpoint() const { return QStringLiteral("%1:%2").arg(ip).arg(port); }
};

Q_DECLARE_METATYPE(AdbDevice)
Q_DECLARE_METATYPE(MdnsService)

// 封装 adb 命令行调用。所有操作异步执行，结果通过信号返回，不阻塞界面。
class AdbManager : public QObject
{
    Q_OBJECT

public:
    explicit AdbManager(QObject *parent = nullptr);

    QString adbPath() const;
    bool isAvailable() const;

    // adb pair <host:port> <code>
    void pair(const QString &hostPort, const QString &code);
    // adb connect <host:port>
    void connectTo(const QString &hostPort);
    // adb disconnect <serial>
    void disconnectDevice(const QString &serial);
    // adb mdns services
    void mdnsServices(bool quiet = false);
    // adb devices -l
    void listDevices(bool quiet = false);
    // adb mdns check，确认 adb 的 mDNS 后端可用
    void checkMdns();

signals:
    void logMessage(const QString &text);
    void pairFinished(bool ok, const QString &message);
    void connectFinished(bool ok, const QString &message);
    void disconnected(const QString &serial);
    void mdnsListed(const QList<MdnsService> &services);
    void devicesListed(const QList<AdbDevice> &devices);
    void mdnsChecked(bool available, const QString &detail);

private:
    void run(const QStringList &args,
             std::function<void(int, const QString &)> done,
             bool quiet = false);

    QString m_adbPath;
};

#endif // ADBMANAGER_H