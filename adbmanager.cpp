#include "adbmanager.h"

#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringConverter>

#include <memory>

namespace {
using OutputPtr = std::shared_ptr<QString>;
using OncePtr = std::shared_ptr<bool>;

// adb 在 Windows 上输出 UTF-8（包括中文系统下本地化的 Winsock 错误消息，
// 如“由于目标计算机积极拒绝，无法连接。”）。若拿到非 UTF-8 数据
// （旧版构建或异常输出），再退回系统本地编码。
QString decodeAdbOutput(const QByteArray &data)
{
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString text = decoder.decode(data);
    if (!decoder.hasError())
        return text;
    return QString::fromLocal8Bit(data);
}

QString adbExecutable()
{
    QString path = QStandardPaths::findExecutable(QStringLiteral("adb"));
    if (!path.isEmpty())
        return path;

    const QString user = qEnvironmentVariable("USERNAME");
    const QStringList candidates = {
        QStringLiteral("C:/Users/%1/Android/platform-tools/adb.exe").arg(user),
        QStringLiteral("C:/Android/platform-tools/adb.exe"),
        QStringLiteral("C:/Users/%1/AppData/Local/Android/Sdk/platform-tools/adb.exe").arg(user),
    };
    for (const QString &candidate : candidates) {
        if (QFileInfo::exists(candidate))
            return candidate;
    }
    return QString();
}
} // namespace

AdbManager::AdbManager(QObject *parent)
    : QObject(parent)
    , m_adbPath(adbExecutable())
{
}

QString AdbManager::adbPath() const
{
    return m_adbPath;
}

bool AdbManager::isAvailable() const
{
    return !m_adbPath.isEmpty();
}

void AdbManager::run(const QStringList &args, std::function<void(int, const QString &)> done, bool quiet)
{
    if (m_adbPath.isEmpty()) {
        if (done)
            done(-1, QStringLiteral("未找到 adb，请确认 platform-tools 已加入 PATH"));
        return;
    }

    auto *process = new QProcess(this);
    process->setProcessChannelMode(QProcess::MergedChannels);

    // 老版本 platform-tools 需要这个变量才启用内置 mDNS；新版默认启用，设置无害
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("ADB_MDNS_OPENSCREEN"), QStringLiteral("1"));
    process->setProcessEnvironment(env);

    const OutputPtr collected = std::make_shared<QString>();
    const OncePtr finishedOnce = std::make_shared<bool>(false);
    const auto pending = std::make_shared<QByteArray>(); // 未成行的原始字节

    connect(process, &QProcess::readyReadStandardOutput, this,
            [this, process, collected, pending, quiet]() {
        pending->append(process->readAllStandardOutput());
        // 只解码完整行，避免一个 UTF-8 多字节字符被读取边界切成两半
        int newline = 0;
        while ((newline = int(pending->indexOf('\n'))) >= 0) {
            const QString text = decodeAdbOutput(pending->left(newline + 1));
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
            const QString text = decodeAdbOutput(*pending);
            pending->clear();
            collected->append(text);
            if (!quiet && !text.trimmed().isEmpty())
                emit logMessage(text.trimmed());
        }

        process->deleteLater();

        if (status == QProcess::CrashExit) {
            if (done)
                done(-1, QStringLiteral("adb 进程异常退出"));
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
        emit logMessage(QStringLiteral("adb 启动失败，可能是路径有误"));
        if (done)
            done(-1, QStringLiteral("adb 启动失败"));
    });

    if (!quiet)
        emit logMessage(QStringLiteral("> adb ") + args.join(' '));
    process->start(m_adbPath, args);
}

void AdbManager::pair(const QString &hostPort, const QString &code)
{
    // 配对码不进日志
    emit logMessage(QStringLiteral("> adb pair %1 ******").arg(hostPort));
    run({QStringLiteral("pair"), hostPort, code}, [this](int exitCode, const QString &output) {
        const bool ok = exitCode == 0
                        && output.contains(QStringLiteral("Successfully paired"), Qt::CaseInsensitive);
        if (ok)
            emit pairFinished(true, QStringLiteral("配对成功"));
        else
            emit pairFinished(false, output.isEmpty() ? QStringLiteral("配对失败") : output);
    }, /*quiet=*/true);
}

void AdbManager::connectTo(const QString &hostPort)
{
    run({QStringLiteral("connect"), hostPort}, [this, hostPort](int exitCode, const QString &output) {
        const QString lower = output.toLower();
        // adb connect 失败时 exit code 仍可能是 0，必须看输出
        const bool ok = exitCode == 0
                        && !lower.contains(QStringLiteral("unable"))
                        && !lower.contains(QStringLiteral("cannot"))
                        && !lower.contains(QStringLiteral("failed"));
        if (ok)
            emit connectFinished(true, QStringLiteral("已连接 %1").arg(hostPort));
        else
            emit connectFinished(false, output.isEmpty() ? QStringLiteral("连接失败") : output);
    });
}

void AdbManager::disconnectDevice(const QString &serial)
{
    run({QStringLiteral("disconnect"), serial}, [this, serial](int, const QString &output) {
        emit disconnected(serial);
        if (!output.trimmed().isEmpty())
            emit logMessage(output.trimmed());
        listDevices();
    });
}

void AdbManager::mdnsServices(bool quiet)
{
    run({QStringLiteral("mdns"), QStringLiteral("services")},
        [this](int exitCode, const QString &output) {
        QList<MdnsService> services;
        if (exitCode == 0) {
            // 输出形如：adbqr-1a2b3c4d	_adb-tls-pairing._tcp	192.168.1.5:37123
            const QStringList lines = output.split(QLatin1Char('\n'));
            for (const QString &line : lines) {
                const QString trimmed = line.trimmed();
                if (!trimmed.contains(QStringLiteral("_adb-tls")))
                    continue;

                QStringList parts;
                if (trimmed.contains(QLatin1Char('\t')))
                    parts = trimmed.split(QLatin1Char('\t'), Qt::SkipEmptyParts);
                else
                    parts = trimmed.split(QRegularExpression(QStringLiteral("\\s+")),
                                          Qt::SkipEmptyParts);
                if (parts.size() < 3)
                    continue;

                MdnsService service;
                service.instance = parts.at(0).trimmed();
                service.type = parts.at(1).trimmed();
                const QString endpoint = parts.at(2).trimmed();

                const int colon = endpoint.lastIndexOf(QLatin1Char(':'));
                if (colon <= 0)
                    continue;
                service.ip = endpoint.left(colon);
                bool ok = false;
                service.port = endpoint.mid(colon + 1).toInt(&ok);
                if (!ok)
                    continue;

                if (service.type.startsWith(QStringLiteral("_adb-tls")))
                    services << service;
            }
        }
        emit mdnsListed(services);
    }, quiet);
}

void AdbManager::listDevices(bool quiet)
{
    run({QStringLiteral("devices"), QStringLiteral("-l")},
        [this](int exitCode, const QString &output) {
        QList<AdbDevice> devices;
        if (exitCode == 0) {
            const QStringList lines = output.split(QLatin1Char('\n'));
            for (const QString &line : lines) {
                const QString trimmed = line.trimmed();
                if (trimmed.isEmpty() || trimmed.startsWith(QStringLiteral("List of devices")))
                    continue;

                const QStringList parts = trimmed.split(QRegularExpression(QStringLiteral("\\s+")),
                                                        Qt::SkipEmptyParts);
                if (parts.size() < 2)
                    continue;

                AdbDevice device;
                device.serial = parts.at(0);
                device.state = parts.at(1);
                for (int i = 2; i < parts.size(); ++i) {
                    if (parts.at(i).startsWith(QStringLiteral("model:"))) {
                        device.model = parts.at(i).mid(6);
                        break;
                    }
                }
                devices << device;
            }
        }
        emit devicesListed(devices);
    }, quiet);
}

void AdbManager::checkMdns()
{
    run({QStringLiteral("mdns"), QStringLiteral("check")},
        [this](int exitCode, const QString &output) {
        const bool available = exitCode == 0
                               && output.contains(QStringLiteral("daemon version"));
        emit mdnsChecked(available, output.trimmed());
    }, true);
}