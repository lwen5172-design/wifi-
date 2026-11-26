#ifndef HDCMANAGER_H
#define HDCMANAGER_H

#include <QHash>
#include <QMetaType>
#include <QObject>
#include <QSet>
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

// 局域网里发现或记住的鸿蒙 hdc 地址（尚未连接）
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
    // 手动指定 hdc 可执行文件路径（会持久化到 QSettings）
    void setHdcPath(const QString &path);

    // hdc list targets -v
    void listTargets(bool quiet = false);
    // hdc tconn <ip:port>；quiet 时不把命令和失败信息写进日志（自动重连用）
    void connectTo(const QString &key, bool quiet = false);
    // hdc kill -r：重启 hdc 服务进程，断开所有连接
    void restartServer();
    // 自己实现局域网发现：向本机各网段的每个地址单播 "OHOS HDC"，
    // 鸿蒙 daemon 会应答 "OHOS HDC-<端口>"。不依赖 hdc discover，
    // 也不受路由器过滤广播的影响。
    void probeLan();
    bool isProbing() const { return m_probing; }

    // 记住连过的 IP:端口，下次自动重连
    // （鸿蒙的 daemon 只监听随机 TCP 端口，UDP 发现又不一定回包，靠记住地址兜底）
    QStringList knownEndpoints() const;
    void rememberEndpoint(const QString &key);
    void forgetEndpoint(const QString &key);
    // 单独用 TCP 探测记住的地址是否还在线，不依赖 UDP 发现
    void checkKnownEndpoints(int timeoutMs = 700);

    // 查询已连接鸿蒙设备的名称（hdc 的 list targets 不带型号，只能问设备本身）
    void requestDeviceName(const QString &key);
    QString cachedDeviceName(const QString &key) const;

    // hdc -v
    void checkVersion();

signals:
    void logMessage(const QString &text);
    void targetsListed(const QList<HdcDevice> &devices);
    void discoveredListed(const QList<HdcDiscovered> &devices);
    void connectFinished(bool ok, const QString &message, const QString &key, bool quiet);
    void serverRestarted(const QString &detail);
    void versionChecked(bool available, const QString &detail);
    // 一轮局域网探测结束：probed 探测了多少个地址，found 发现了几台
    void lanProbeFinished(int probed, int found);
    // 记住的地址里，TCP 还连得上的那些
    void knownEndpointsListed(const QList<HdcDiscovered> &devices);
    // 设备名查到了（例如 "HUAWEI Mate 60 Pro"）
    void deviceNameResolved(const QString &key, const QString &name);

private:
    void run(const QStringList &args,
             std::function<void(int, const QString &)> done,
             bool quiet = false,
             int timeoutMs = 0);

    QString m_hdcPath;
    bool m_probing = false;
    QHash<QString, QString> m_deviceNames; // connectKey -> 设备名
    QSet<QString> m_nameRequests;          // 正在查的 key，避免重复起进程
    QHash<QString, qint64> m_nameFailedAt; // 查失败的 key -> 时间戳，一段时间内不再重试
};

#endif // HDCMANAGER_H