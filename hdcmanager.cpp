#include "hdcmanager.h"

#include "textdecode.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QNetworkProxy>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>

#include <algorithm>
#include <memory>

namespace {
using OutputPtr = std::shared_ptr<QString>;
using OncePtr = std::shared_ptr<bool>;

const char *kSettingsOrg = "WifiDebugHelper";
const char *kSettingsApp = "WifiDebugHelper";
const char *kHdcPathKey = "hdc/path";
const char *kKnownEndpointsKey = "hdc/knownEndpoints";

// 鸿蒙 hdc daemon 的局域网发现端口（源码 define.h 里的 DEFAULT_PORT）
constexpr quint16 kDiscoveryPort = 8710;
// 发现请求的载荷，与 hdc 源码里的 HANDSHAKE_MESSAGE 一致
const QByteArray kDiscoveryPayload = QByteArrayLiteral("OHOS HDC");
// 一次探测最多覆盖多少个地址，避免在 /16 这种大网段里发几万个包
constexpr int kMaxProbeTargets = 2048;
// 等待 daemon 应答的时间
constexpr int kProbeTimeoutMs = 1500;

QSettings settings()
{
    return QSettings(QSettings::IniFormat, QSettings::UserScope,
                     QString::fromLatin1(kSettingsOrg), QString::fromLatin1(kSettingsApp));
}

// IPv4-mapped IPv6（::ffff:192.168.3.35）统一转成普通 IPv4 字符串
QString normalizedAddress(const QHostAddress &address)
{
    if (address.protocol() == QAbstractSocket::IPv6Protocol) {
        bool isV4 = false;
        const quint32 v4 = address.toIPv4Address(&isV4);
        if (isV4)
            return QHostAddress(v4).toString();
    }
    return address.toString();
}

// `hdc shell param get xxx` 的返回值：正常是一行值，失败时是 [Fail] 或 "Get parameter ... fail!"
QString cleanParamValue(const QString &output)
{
    const QStringList lines = output.split(QLatin1Char('\n'));
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty())
            continue;
        if (line.startsWith(QLatin1Char('[')) || line.contains(QStringLiteral("[Fail]"))
            || line.contains(QStringLiteral("fail!"))
            || line.contains(QStringLiteral("need connect-key")))
            continue;
        return line;
    }
    return QString();
}

// 在指定目录里递归找 hdc.exe，限制深度，避免扫描整个磁盘。
// onlyToolchainDirs 为 true 时只进入名字像 SDK / 工具链的目录，
// 防止在 Downloads、桌面这种大目录里漫无目的地遍历。
QString scanForHdc(const QString &root, int depth, bool onlyToolchainDirs = false)
{
    if (depth < 0)
        return QString();
    QDir dir(root);
    if (!dir.exists())
        return QString();

    static const QRegularExpression toolchainNameRe(
        QStringLiteral("hdc|toolchain|sdk|deveco|harmony"),
        QRegularExpression::CaseInsensitiveOption);

    const QFileInfoList entries =
        dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &entry : entries) {
        if (entry.isFile() && entry.fileName().compare(QStringLiteral("hdc.exe"),
                                                       Qt::CaseInsensitive) == 0)
            return entry.absoluteFilePath();
    }
    for (const QFileInfo &entry : entries) {
        if (!entry.isDir())
            continue;
        if (onlyToolchainDirs && !toolchainNameRe.match(entry.fileName()).hasMatch())
            continue;
        const QString found = scanForHdc(entry.absoluteFilePath(), depth - 1, onlyToolchainDirs);
        if (!found.isEmpty())
            return found;
    }
    return QString();
}

QString findHdcExecutable()
{
    // 1. 用户在程序里手动指定的路径
    const QString saved = settings().value(QString::fromLatin1(kHdcPathKey)).toString();
    if (!saved.isEmpty() && QFileInfo::exists(saved))
        return saved;

    // 2. 环境变量
    const QString fromEnv = qEnvironmentVariable("HDC_PATH");
    if (!fromEnv.isEmpty() && QFileInfo::exists(fromEnv))
        return fromEnv;

    // 3. PATH
    const QString inPath = QStandardPaths::findExecutable(QStringLiteral("hdc"));
    if (!inPath.isEmpty())
        return inPath;

    // 4. DevEco Studio / HarmonyOS SDK 的常见安装位置
    const QString user = qEnvironmentVariable("USERNAME");
    const QStringList candidates = {
        QStringLiteral("C:/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/toolchains/hdc.exe"),
        QStringLiteral("C:/Program Files/Huawei/DevEco Studio/sdk/default/hms/toolchains/hdc.exe"),
        QStringLiteral("C:/Program Files/Huawei/DevEco Studio/tools/hdc/hdc.exe"),
        QStringLiteral("D:/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/toolchains/hdc.exe"),
        QStringLiteral("D:/Huawei/DevEco Studio/sdk/default/openharmony/toolchains/hdc.exe"),
        QStringLiteral("C:/Users/%1/AppData/Local/Huawei/Sdk/openharmony/toolchains/hdc.exe").arg(user),
        QStringLiteral("C:/Users/%1/AppData/Local/OpenHarmony/Sdk/openharmony/toolchains/hdc.exe").arg(user),
    };
    for (const QString &candidate : candidates) {
        if (QFileInfo::exists(candidate))
            return candidate;
    }

    // SDK 下面通常还有版本号这一层目录（AppData/Local/Huawei/Sdk/12/openharmony/...）
    const QStringList sdkRoots = {
        QStringLiteral("C:/Users/%1/AppData/Local/Huawei/Sdk").arg(user),
        QStringLiteral("C:/Users/%1/AppData/Local/OpenHarmony/Sdk").arg(user),
        QStringLiteral("C:/Users/%1/AppData/Local/Huawei/sdk").arg(user),
    };
    for (const QString &root : sdkRoots) {
        const QString found = scanForHdc(root, 3);
        if (!found.isEmpty())
            return found;
    }

    // 5. 用户手工下载/解压的 toolchains（常见于 Downloads、桌面）
    const QStringList userDirs = {
        QStringLiteral("C:/Users/%1/Downloads").arg(user),
        QStringLiteral("C:/Users/%1/Desktop").arg(user),
    };
    for (const QString &root : userDirs) {
        const QString found = scanForHdc(root, 3, /*onlyToolchainDirs=*/true);
        if (!found.isEmpty())
            return found;
    }
    return QString();
}

QList<HdcDevice> parseTargets(const QString &output)
{
    QList<HdcDevice> devices;
    const QStringList lines = output.split(QLatin1Char('\n'));
    for (const QString &rawLine : lines) {
        const QString line = rawLine.trimmed();
        // [Empty] / [Info] / [Fail] 等状态行
        if (line.isEmpty() || line.startsWith(QLatin1Char('[')))
            continue;

        // -v 输出形如：COM1\t\tUART\tReady\tunknown...\thdc
        QStringList parts = line.split(QLatin1Char('\t'), Qt::SkipEmptyParts);
        if (parts.size() < 2)
            parts = line.split(QRegularExpression(QStringLiteral("\\s+")),
                               Qt::SkipEmptyParts);
        if (parts.isEmpty())
            continue;

        HdcDevice device;
        device.connectKey = parts.at(0).trimmed();
        if (device.connectKey.isEmpty())
            continue;
        if (parts.size() >= 3) {
            device.type = parts.at(1).trimmed();
            device.state = parts.at(2).trimmed();
        } else if (parts.size() == 2) {
            device.type = parts.at(1).trimmed();
            device.state = QStringLiteral("Unknown");
        } else {
            device.state = QStringLiteral("Unknown");
        }

        // hdc 会把本机的 COM 串口也列出来（COM1 UART Ready），不是手机，跳过
        if (device.type.compare(QStringLiteral("UART"), Qt::CaseInsensitive) == 0)
            continue;

        devices << device;
    }
    return devices;
}

// 每个本机 IPv4 网卡一条探测计划
struct ProbePlan
{
    QHostAddress localAddress;
    QHostAddress broadcastAddress; // 子网定向广播地址
    QList<QHostAddress> targets;   // 需要单播探测的地址（跳过自己）
};

QList<ProbePlan> buildProbePlans()
{
    QList<ProbePlan> plans;
    const QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : interfaces) {
        const auto flags = iface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp)
            || !flags.testFlag(QNetworkInterface::IsRunning)
            || flags.testFlag(QNetworkInterface::IsLoopBack))
            continue;

        for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
            const QHostAddress local = entry.ip();
            if (local.protocol() != QAbstractSocket::IPv4Protocol)
                continue;
            const int prefix = entry.prefixLength();
            if (prefix < 16)
                continue; // 网段太大，逐个探测没有意义

            const quint32 base = local.toIPv4Address();
            const quint32 mask =
                prefix >= 32 ? 0xffffffffu : ~((1u << (32 - prefix)) - 1u);
            const quint32 network = base & mask;
            const quint32 broadcast = network | ~mask;

            ProbePlan plan;
            plan.localAddress = local;
            plan.broadcastAddress = QHostAddress(broadcast);
            for (quint32 addr = network + 1; addr < broadcast; ++addr) {
                if (addr == base)
                    continue; // 跳过自己
                plan.targets << QHostAddress(addr);
                if (plan.targets.size() >= kMaxProbeTargets)
                    break;
            }
            plans << plan;
        }
    }
    return plans;
}

} // namespace

HdcManager::HdcManager(QObject *parent)
    : QObject(parent)
    , m_hdcPath(findHdcExecutable())
{
}

QString HdcManager::hdcPath() const
{
    return m_hdcPath;
}

bool HdcManager::isAvailable() const
{
    return !m_hdcPath.isEmpty();
}

void HdcManager::setHdcPath(const QString &path)
{
    m_hdcPath = path;
    settings().setValue(QString::fromLatin1(kHdcPathKey), path);
}

void HdcManager::run(const QStringList &args,
                     std::function<void(int, const QString &)> done,
                     bool quiet,
                     int timeoutMs)
{
    if (m_hdcPath.isEmpty()) {
        if (done)
            done(-1, QStringLiteral("未找到 hdc，请使用 DevEco Studio 自带的 hdc.exe，"
                                    "点「选择 hdc.exe」指定路径"));
        return;
    }

    auto *process = new QProcess(this);
    process->setProcessChannelMode(QProcess::MergedChannels);

    const OutputPtr collected = std::make_shared<QString>();
    const OncePtr finishedOnce = std::make_shared<bool>(false);
    const auto pending = std::make_shared<QByteArray>(); // 未成行的原始字节

    // 某些 hdc 命令（例如 tconn 到一个已关闭的端口）会卡住不返回，加个兜底超时
    QTimer *killTimer = nullptr;
    if (timeoutMs > 0) {
        killTimer = new QTimer(process);
        killTimer->setSingleShot(true);
        connect(killTimer, &QTimer::timeout, process, [process]() {
            if (process->state() != QProcess::NotRunning)
                process->kill();
        });
        killTimer->start(timeoutMs);
    }

    connect(process, &QProcess::readyReadStandardOutput, this,
            [this, process, collected, pending, quiet]() {
        pending->append(process->readAllStandardOutput());
        // hdc 输出以 \r\r\n 结尾，按完整行解码可避免 UTF-8 多字节字符被切断
        int newline = 0;
        while ((newline = int(pending->indexOf('\n'))) >= 0) {
            const QString text = decodeProcessOutput(pending->left(newline + 1));
            pending->remove(0, newline + 1);
            collected->append(text);
            const QString trimmed = text.trimmed();
            if (!quiet && !trimmed.isEmpty())
                emit logMessage(trimmed);
        }
    });

    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, process, collected, finishedOnce, pending, done, quiet, killTimer](
                      int exitCode, QProcess::ExitStatus status) {
        if (*finishedOnce)
            return;
        *finishedOnce = true;
        if (killTimer)
            killTimer->stop();

        pending->append(process->readAllStandardOutput());
        if (!pending->isEmpty()) {
            const QString text = decodeProcessOutput(*pending);
            pending->clear();
            collected->append(text);
            if (!quiet && !text.trimmed().isEmpty())
                emit logMessage(text.trimmed());
        }

        process->deleteLater();

        if (status == QProcess::CrashExit) {
            if (done)
                done(-1, QStringLiteral("hdc 进程异常退出"));
            return;
        }
        if (done)
            done(exitCode, collected->trimmed());
    });

    connect(process, &QProcess::errorOccurred, this,
            [this, process, finishedOnce, done](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || *finishedOnce)
            return;
        *finishedOnce = true;
        process->deleteLater();
        emit logMessage(QStringLiteral("hdc 启动失败，请检查 hdc.exe 路径"));
        if (done)
            done(-1, QStringLiteral("hdc 启动失败"));
    });

    if (!quiet)
        emit logMessage(QStringLiteral("> hdc ") + args.join(' '));
    process->start(m_hdcPath, args);
}

void HdcManager::listTargets(bool quiet)
{
    run({QStringLiteral("list"), QStringLiteral("targets"), QStringLiteral("-v")},
        [this](int, const QString &output) {
        const QList<HdcDevice> devices = parseTargets(output);
        // 已连上的 TCP 设备顺手记下来，下次启动可以直接自动重连
        for (const HdcDevice &device : devices) {
            const bool isTcp =
                device.type.compare(QStringLiteral("TCP"), Qt::CaseInsensitive) == 0;
            if (!isTcp)
                continue;
            if (device.state.compare(QStringLiteral("Connected"), Qt::CaseInsensitive) == 0)
                rememberEndpoint(device.connectKey);
        }
        emit targetsListed(devices);
    }, quiet);
}

void HdcManager::connectTo(const QString &key, bool quiet)
{
    run({QStringLiteral("tconn"), key}, [this, key, quiet](int, const QString &output) {
        const QString text = output.trimmed();
        // 成功输出：Connect OK；重复连接：Target is connected, repeat opration
        const bool ok = text.contains(QStringLiteral("Connect OK"), Qt::CaseInsensitive)
                        || text.contains(QStringLiteral("Target is connected"),
                                         Qt::CaseInsensitive);
        if (ok) {
            rememberEndpoint(key);
            emit connectFinished(true, QStringLiteral("已连接 %1").arg(key), key, quiet);
        } else {
            emit connectFinished(false,
                                 text.isEmpty() ? QStringLiteral("hdc 连接失败") : text,
                                 key, quiet);
        }
    }, quiet);
}

void HdcManager::restartServer()
{
    run({QStringLiteral("kill"), QStringLiteral("-r")}, [this](int, const QString &) {
        emit serverRestarted(QStringLiteral("已重启 hdc 服务"));
        listTargets(true);
    }, true);
}

void HdcManager::probeLan()
{
    if (m_probing)
        return;

    // 鸿蒙 daemon 收到 UDP 8710 上的 "OHOS HDC" 后会回同样端口：
    // "OHOS HDC-<它监听的 TCP 端口>"。这里自己做单播扫描：
    //  - 不依赖 hdc discover（部分 hdc 版本发不出正确广播）
    //  - 单个单播包不受路由器过滤广播/组播影响
    const QList<ProbePlan> plans = buildProbePlans();
    int targetCount = 0;
    for (const ProbePlan &plan : plans)
        targetCount += plan.targets.size();
    if (targetCount == 0) {
        emit lanProbeFinished(0, 0);
        return;
    }
    m_probing = true;

    auto *socket = new QUdpSocket(this);
    socket->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 1 << 20);
    // 绑定到固定端口 8710：daemon 的应答固定回到发送方的 8710 端口
    if (!socket->bind(QHostAddress::AnyIPv4, kDiscoveryPort,
                      QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        emit logMessage(QStringLiteral("局域网探测失败：UDP %1 端口被占用（%2）")
                            .arg(kDiscoveryPort)
                            .arg(socket->errorString()));
        socket->deleteLater();
        m_probing = false;
        emit lanProbeFinished(0, 0);
        return;
    }

    auto found = std::make_shared<QList<HdcDiscovered>>();
    auto seen = std::make_shared<QSet<QString>>();

    connect(socket, &QUdpSocket::readyRead, this, [this, socket, found, seen]() {
        while (socket->hasPendingDatagrams()) {
            const QNetworkDatagram datagram = socket->receiveDatagram();
            const QByteArray data = datagram.data();
            if (!data.startsWith(kDiscoveryPayload))
                continue;
            // "OHOS HDC-<tcpPort>"，端口在 '-' 之后
            const int dash = data.indexOf('-');
            if (dash < 0)
                continue;
            bool ok = false;
            const int port = data.mid(dash + 1).trimmed().toInt(&ok);
            if (!ok || port <= 0 || port > 65535)
                continue;

            HdcDiscovered device;
            device.ip = normalizedAddress(datagram.senderAddress());
            device.port = port;
            if (device.ip.isEmpty() || seen->contains(device.endpoint()))
                continue;
            seen->insert(device.endpoint());
            found->append(device);
        }
    });

    // 逐个地址单播探测（按网卡子网），最后再补一个子网广播
    for (const ProbePlan &plan : plans) {
        for (const QHostAddress &target : plan.targets) {
            socket->writeDatagram(kDiscoveryPayload, target, kDiscoveryPort);
        }
        // 子网定向广播（很多路由器会过滤广播，发一下不亏）
        socket->writeDatagram(kDiscoveryPayload, plan.broadcastAddress, kDiscoveryPort);
    }
    // 全局广播
    socket->writeDatagram(kDiscoveryPayload, QHostAddress(QHostAddress::Broadcast),
                          kDiscoveryPort);

    QTimer::singleShot(kProbeTimeoutMs, this, [this, socket, found, targetCount]() {
        socket->deleteLater();
        m_probing = false;
        emit discoveredListed(*found);
        emit lanProbeFinished(targetCount, found->size());
    });
}

QStringList HdcManager::knownEndpoints() const
{
    return settings().value(QString::fromLatin1(kKnownEndpointsKey)).toStringList();
}

void HdcManager::rememberEndpoint(const QString &key)
{
    const QString trimmed = key.trimmed();
    const int colon = trimmed.lastIndexOf(QLatin1Char(':'));
    if (trimmed.isEmpty() || colon <= 0)
        return;
    const QString ip = trimmed.left(colon);

    QStringList list = knownEndpoints();
    const QStringList before = list;

    // 同 IP 不同端口的旧记录直接淘汰：鸿蒙每次重开无线调试都会换端口，
    // 留着旧端口只会让自动重连白白失败
    for (int i = list.size() - 1; i >= 0; --i) {
        if (list.at(i).left(list.at(i).lastIndexOf(QLatin1Char(':'))) == ip)
            list.removeAt(i);
    }
    list.removeAll(trimmed);
    list.prepend(trimmed);
    while (list.size() > 16)
        list.removeLast();
    if (list != before) // 没变化就别每 4 秒重写一次配置文件
        settings().setValue(QString::fromLatin1(kKnownEndpointsKey), list);
}

void HdcManager::forgetEndpoint(const QString &key)
{
    QStringList list = knownEndpoints();
    if (list.removeAll(key.trimmed()) > 0)
        settings().setValue(QString::fromLatin1(kKnownEndpointsKey), list);
}

void HdcManager::checkKnownEndpoints(int timeoutMs)
{
    // 鸿蒙 daemon 只监听随机 TCP 端口，UDP 发现又不一定回包，
    // 所以“上次连过的地址”单独用 TCP 连一下确认还在线。
    const QStringList list = knownEndpoints();
    if (list.isEmpty()) {
        emit knownEndpointsListed({});
        return;
    }

    auto found = std::make_shared<QList<HdcDiscovered>>();
    auto remaining = std::make_shared<int>(list.size());

    for (const QString &key : list) {
        const int colon = key.lastIndexOf(QLatin1Char(':'));
        bool ok = false;
        const int port = colon > 0 ? key.mid(colon + 1).toInt(&ok) : 0;
        if (!ok || port <= 0 || port > 65535) {
            if (--*remaining == 0)
                emit knownEndpointsListed(*found);
            continue;
        }

        auto *socket = new QTcpSocket(this);
        // 必须绕过系统代理：否则 Qt 会用 HTTP 代理去连裸 TCP，直接失败
        socket->setProxy(QNetworkProxy::NoProxy);
        auto *timer = new QTimer(socket);
        timer->setSingleShot(true);
        auto done = std::make_shared<bool>(false);

        const auto finish = [this, socket, timer, done, found, remaining, key, port](
                                bool alive) {
            if (*done)
                return;
            *done = true;
            timer->stop();
            socket->deleteLater();
            timer->deleteLater();
            if (alive) {
                HdcDiscovered device;
                const int c = key.lastIndexOf(QLatin1Char(':'));
                device.ip = key.left(c);
                device.port = port;
                found->append(device);
            }
            if (--*remaining == 0)
                emit knownEndpointsListed(*found);
        };

        connect(socket, &QTcpSocket::connected, this, [finish]() { finish(true); });
        connect(socket, &QAbstractSocket::errorOccurred, this,
                [finish](QAbstractSocket::SocketError) { finish(false); });
        connect(timer, &QTimer::timeout, this, [socket, finish]() {
            socket->abort();
            finish(false);
        });

        timer->start(timeoutMs);
        socket->connectToHost(key.left(colon), static_cast<quint16>(port));
    }
}

void HdcManager::requestDeviceName(const QString &key)
{
    const QString trimmed = key.trimmed();
    if (trimmed.isEmpty())
        return;
    if (m_deviceNames.contains(trimmed) || m_nameRequests.contains(trimmed))
        return; // 已经有了，或者正在查
    // 刚查失败过就别每 4 秒重试一次，隔一分钟再试
    const qint64 lastFailed = m_nameFailedAt.value(trimmed, 0);
    if (lastFailed > 0 && QDateTime::currentMSecsSinceEpoch() - lastFailed < 60 * 1000)
        return;

    m_nameRequests.insert(trimmed);

    // 依次尝试几个能拿到人话名称的参数，第一个有效值就用
    const QStringList params = {
        QStringLiteral("const.product.name"),
        QStringLiteral("const.product.marketname"),
        QStringLiteral("const.product.brand"),
        QStringLiteral("const.product.model"),
    };
    auto index = std::make_shared<int>(0);
    auto tryNext = std::make_shared<std::function<void()>>();

    *tryNext = [this, trimmed, params, index, tryNext]() {
        if (*index >= params.size()) {
            m_nameRequests.remove(trimmed);
            m_nameFailedAt.insert(trimmed, QDateTime::currentMSecsSinceEpoch());
            return;
        }
        const QString param = params.at(*index);
        ++*index;

        run({QStringLiteral("-t"), trimmed, QStringLiteral("shell"),
             QStringLiteral("param get %1").arg(param)},
            [this, trimmed, param, tryNext](int, const QString &output) {
            const QString value = cleanParamValue(output);
            if (value.isEmpty() || value.compare(QStringLiteral("default"),
                                                 Qt::CaseInsensitive) == 0) {
                (*tryNext)();
                return;
            }
            m_deviceNames.insert(trimmed, value);
            m_nameRequests.remove(trimmed);
            emit deviceNameResolved(trimmed, value);
        }, /*quiet=*/true, /*timeoutMs=*/4000);
    };
    (*tryNext)();
}

QString HdcManager::cachedDeviceName(const QString &key) const
{
    return m_deviceNames.value(key.trimmed());
}

void HdcManager::checkVersion()
{
    run({QStringLiteral("-v")}, [this](int exitCode, const QString &output) {
        const bool ok = exitCode == 0 && !output.trimmed().isEmpty()
                        && !output.contains(QStringLiteral("[Fail]"));
        emit versionChecked(ok, output.trimmed());
    }, true);
}
