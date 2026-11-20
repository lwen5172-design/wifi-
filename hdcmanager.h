#ifndef HDCMANAGER_H
#define HDCMANAGER_H

#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

// 一台通过 hdc 看到的鸿蒙设备
struct HdcDevice
{
    QString connectKey; // USB SN / 192.168.1.5:8710 / COM1
    QString type;       // USB / TCP / UART
    QString state;      // Connected / Offline / Ready / Unknown
};

// 局域网里通过 `hdc discover` 发现、尚未连接的设备
struct HdcDiscovered
{
    QString ip;
    int port = 0;

    QString endpoint() const { return QStringLiteral("%1:%2").arg(ip).arg(port); }
};

Q_DECLARE_METATYPE(HdcDevice)
Q_DECLARE_METATYPE(HdcDiscovered)

// 封装 hdc（HarmonyOS Device Connector）命令行调用。
// 与 AdbManager 一样全部异步，结果通过信号返回。
class HdcManager : public QObject
{
    Q_OBJECT

public:
    explicit HdcManager(QObject *parent = nullptr);

    QString hdcPath() const;
    bool isAvailable() const;
    // 手动指定 hdc.exe 路径（会持久化到 QSettings）
    void setHdcPath(const QString &path);

    // hdc list targets -v
    void listTargets(bool quiet = false);
    // hdc tconn <ip:port>
    void connectTo(const QString &key);
    // hdc kill -r：重启 hdc 服务进程，断开所有连接
    void restartServer();
    // hdc discover（局域网广播发现）
    void discover();
    // hdc -v
    void checkVersion();

signals:
    void logMessage(const QString &text);
    void targetsListed(const QList<HdcDevice> &devices);
    void discoveredListed(const QList<HdcDiscovered> &devices);
    void connectFinished(bool ok, const QString &message);
    void serverRestarted(const QString &detail);
    void versionChecked(bool available, const QString &detail);

private:
    void run(const QStringList &args,
             std::function<void(int, const QString &)> done,
             bool quiet = false);

    QString m_hdcPath;
};

#endif // HDCMANAGER_H