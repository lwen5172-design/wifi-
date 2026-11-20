#include "hdcmanager.h"

#include "textdecode.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>

#include <memory>

namespace {
using OutputPtr = std::shared_ptr<QString>;
using OncePtr = std::shared_ptr<bool>;

const char *kSettingsOrg = "WifiDebugHelper";
const char *kSettingsApp = "WifiDebugHelper";
const char *kHdcPathKey = "hdc/path";

QSettings settings()
{
    return QSettings(QSettings::IniFormat, QSettings::UserScope,
                     QString::fromLatin1(kSettingsOrg), QString::fromLatin1(kSettingsApp));
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

QList<HdcDiscovered> parseDiscovery(const QString &output)
{
    QList<HdcDiscovered> found;
    QSet<QString> seen;
    static const QRegularExpression endpointRe(
        QStringLiteral("(\\d{1,3}(?:\\.\\d{1,3}){3}):(\\d{1,5})"));

    const QStringList lines = output.split(QLatin1Char('\n'));
    for (const QString &rawLine : lines) {
        const QString line = rawLine.trimmed();
        if (line.startsWith(QLatin1Char('[')))
            continue; // [Info]Broadcast find daemon, total:N

        const QRegularExpressionMatch match = endpointRe.match(line);
        if (!match.hasMatch())
            continue;

        HdcDiscovered device;
        device.ip = match.captured(1);
        bool ok = false;
        device.port = match.captured(2).toInt(&ok);
        if (!ok || device.port <= 0)
            continue;
        if (seen.contains(device.endpoint()))
            continue;
        seen.insert(device.endpoint());
        found << device;
    }
    return found;
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
                     bool quiet)
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
            this, [this, process, collected, finishedOnce, pending, done, quiet](
                      int exitCode, QProcess::ExitStatus status) {
        if (*finishedOnce)
            return;
        *finishedOnce = true;

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
        emit targetsListed(parseTargets(output));
    }, quiet);
}

void HdcManager::connectTo(const QString &key)
{
    run({QStringLiteral("tconn"), key}, [this, key](int, const QString &output) {
        const QString text = output.trimmed();
        // 成功输出：Connect OK；重复连接：Target is connected, repeat opration
        const bool ok = text.contains(QStringLiteral("Connect OK"), Qt::CaseInsensitive)
                        || text.contains(QStringLiteral("Target is connected"),
                                         Qt::CaseInsensitive);
        if (ok)
            emit connectFinished(true, QStringLiteral("已连接 %1").arg(key));
        else
            emit connectFinished(false,
                                 text.isEmpty() ? QStringLiteral("hdc 连接失败") : text);
    });
}

void HdcManager::restartServer()
{
    run({QStringLiteral("kill"), QStringLiteral("-r")}, [this](int, const QString &) {
        emit serverRestarted(QStringLiteral("已重启 hdc 服务"));
        listTargets(true);
    }, true);
}

void HdcManager::discover()
{
    run({QStringLiteral("discover")}, [this](int, const QString &output) {
        emit discoveredListed(parseDiscovery(output));
    }, true);
}

void HdcManager::checkVersion()
{
    run({QStringLiteral("-v")}, [this](int exitCode, const QString &output) {
        const bool ok = exitCode == 0 && !output.trimmed().isEmpty()
                        && !output.contains(QStringLiteral("[Fail]"));
        emit versionChecked(ok, output.trimmed());
    }, true);
}