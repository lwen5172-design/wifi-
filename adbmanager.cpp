#include "adbmanager.h"

#include "textdecode.h"

#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>

#include <memory>

namespace {
using OutputPtr = std::shared_ptr<QString>;
using OncePtr = std::shared_ptr<bool>;


// 1. 优先走系统 PATH，用 `findExecutable` 找 adb；
// 2. PATH 找不到的话，去 Windows 上 Android SDK 几个最常见默认安装位置挨个尝试；
// 3. 找到就返回 adb 绝对路径，全都找不到返回空串。
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
    //挨个找
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
    process->setProcessChannelMode(QProcess::MergedChannels); //stdout 和 stderr 合并在一起,不用分开处理两个管道

    // 老版本 platform-tools 需要这个变量才启用内置 mDNS；新版默认启用，设置无害
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("ADB_MDNS_OPENSCREEN"), QStringLiteral("1"));
    process->setProcessEnvironment(env);

    const OutputPtr collected = std::make_shared<QString>();//汇总全部的adb输出，给done回调
    const OncePtr finishedOnce = std::make_shared<bool>(false);//`finished` 和 `errorOccurred` 信号都可能触发收尾，保证收尾逻辑只跑一次
    const auto pending = std::make_shared<QByteArray>(); // 未成行的原始字节

    //流式读
    connect(process, &QProcess::readyReadStandardOutput, this,
            [this, process, collected, pending, quiet]() {
        pending->append(process->readAllStandardOutput());
        // 只解码完整行，避免一个 UTF-8 多字节字符被读取边界切成两半
        int newline = 0;
        while ((newline = int(pending->indexOf('\n'))) >= 0) {
            const QString text = decodeProcessOutput(pending->left(newline + 1)); //取出从开头到 `\n` 这一段（包含换行符）.解码函数，把这一整行的字节转成 QString
            pending->remove(0, newline + 1);
            collected->append(text);
            const QString trimmed = text.trimmed();
            if (!quiet && !trimmed.isEmpty())
                emit logMessage(trimmed);
        }
    });
    //进程正常结束 / 崩溃
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, process, collected, finishedOnce, pending, done, quiet](
                      int exitCode, QProcess::ExitStatus status) {
        if (*finishedOnce) //保证一次退出
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

        process->deleteLater();//延迟删除 QProcess 对象
        //进程是崩溃退出（CrashExit）
        if (status == QProcess::CrashExit) {
            if (done)
                done(-1, QStringLiteral("adb 进程异常退出"));
            return;
        }
        if (done)
            done(exitCode, collected->trimmed());
    });
    //进程启动失败
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

//调用无线配对
void AdbManager::pair(const QString &hostPort, const QString &code)
{
    // 配对码不进日志
    emit logMessage(QStringLiteral("> adb pair %1 ******").arg(hostPort));
    run({QStringLiteral("pair"), hostPort, code}, [this](int exitCode, const QString &output) {
        const bool ok = exitCode == 0
                        && output.contains(QStringLiteral("Successfully paired"), Qt::CaseInsensitive);//忽略大小写
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

void AdbManager::requestDeviceName(const QString &serial)
{
    const QString key = serial.trimmed();
    if (key.isEmpty())
        return;
    if (m_deviceNames.contains(key) || m_nameRequests.contains(key))
        return;
    m_nameRequests.insert(key);

    // 一次调用把厂商和型号都拿回来，拼成 “OPPO PKB110” 这种
    run({QStringLiteral("-s"), key, QStringLiteral("shell"),
         QStringLiteral("getprop ro.product.manufacturer; getprop ro.product.model")},
        [this, key](int, const QString &output) {
        m_nameRequests.remove(key);
        QStringList lines;
        const QStringList raw = output.split(QLatin1Char('\n'));
        for (const QString &line : raw) {
            const QString trimmed = line.trimmed();
            if (!trimmed.isEmpty())
                lines << trimmed;
        }
        // output: 第 1 行厂商，第 2 行型号
        QString name;
        if (lines.size() >= 2)
            name = QStringLiteral("%1 %2").arg(lines.at(0), lines.at(1));
        else if (lines.size() == 1)
            name = lines.first();
        if (name.isEmpty())
            return;
        m_deviceNames.insert(key, name);
        emit deviceNameResolved(key, name);
    }, true);
}

QString AdbManager::cachedDeviceName(const QString &serial) const
{
    return m_deviceNames.value(serial.trimmed());
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