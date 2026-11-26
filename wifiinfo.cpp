#include "wifiinfo.h"

#include "macwifiparse.h"

#include <QProcess>
#include <QStringList>

namespace {

#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
// 命令行输出优先按 UTF-8 解码：netsh 在无控制台的 GUI 进程里输出 UTF-8，
// macOS 的 networksetup / security 也输出 UTF-8；出现替换字符再回退到本地编码。
QString decodeConsoleOutput(const QByteArray &raw)
{
    const QString utf8 = QString::fromUtf8(raw);
    if (!utf8.contains(QChar::ReplacementCharacter))
        return utf8;
    return QString::fromLocal8Bit(raw);
}

struct ProcessResult
{
    int exitCode = -1;
    QString output;
    QString error;
};

ProcessResult runProcess(const QString &program, const QStringList &args, int timeoutMs = 8000)
{
    ProcessResult result;
    QProcess process;
    process.start(program, args);
    if (!process.waitForStarted(3000))
        return result;
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(1000);
        return result;
    }
    result.exitCode = process.exitCode();
    result.output = decodeConsoleOutput(process.readAllStandardOutput());
    result.error = decodeConsoleOutput(process.readAllStandardError());
    return result;
}
#endif

#ifdef Q_OS_WIN
// netsh 输出形如 "    SSID                   : test-zhuanyong"，
// 按 "标签 : 值" 拆开，标签做去空格 + 小写比较
bool splitLine(const QString &line, QString *label, QString *value)
{
    const int pos = line.indexOf(QLatin1Char(':'));
    if (pos <= 0)
        return false;
    *label = line.left(pos).trimmed().toLower();
    *value = line.mid(pos + 1).trimmed();
    return true;
}

QString runNetsh(const QStringList &args)
{
    return runProcess(QStringLiteral("netsh"), args).output;
}
#elif defined(Q_OS_MACOS)
// macOS 的 Wi-Fi 接口名（绝大多数是 en0，少数机型 / 外接网卡可能不同）。
// networksetup 的输出固定是英文，不受系统语言影响。
QString macWifiDevice()
{
    const ProcessResult result = runProcess(QStringLiteral("/usr/sbin/networksetup"),
                                            {QStringLiteral("-listallhardwareports")});
    if (result.exitCode == 0) {
        QString currentPort;
        const QStringList lines = result.output.split(QLatin1Char('\n'));
        for (const QString &rawLine : lines) {
            const QString line = rawLine.trimmed();
            if (line.startsWith(QStringLiteral("Hardware Port:")))
                currentPort = line.mid(14).trimmed();
            else if (line.startsWith(QStringLiteral("Device:"))
                     && (currentPort == QStringLiteral("Wi-Fi")
                         || currentPort == QStringLiteral("AirPort")))
                return line.mid(7).trimmed();
        }
    }
    return QStringLiteral("en0");
}

// security -w 的输出末尾带换行
QString chomp(QString text)
{
    while (text.endsWith(QLatin1Char('\n')) || text.endsWith(QLatin1Char('\r')))
        text.chop(1);
    return text;
}
#endif

} // namespace

QString WifiInfo::currentSsid()
{
#ifdef Q_OS_WIN
    const QString output = runNetsh({QStringLiteral("wlan"),
                                     QStringLiteral("show"),
                                     QStringLiteral("interfaces")});
    const QStringList lines = output.split(QLatin1Char('\n'));
    for (const QString &rawLine : lines) {
        QString label;
        QString value;
        if (!splitLine(rawLine, &label, &value))
            continue;
        // 标签必须正好是 "SSID"，避免把 "SSID 数量" 之类的行也算进来
        if (label == QStringLiteral("ssid") && !value.isEmpty())
            return value;
    }
    return QString();
#elif defined(Q_OS_MACOS)
    const QString device = macWifiDevice();

    // 优先用 ipconfig getsummary：它不涉及定位权限，任何进程都能读到 SSID。
    // 输出里有一行形如 "  SSID : MyWifi"。
    {
        const ProcessResult summary = runProcess(
            QStringLiteral("/usr/sbin/ipconfig"),
            {QStringLiteral("getsummary"), device});
        const QString ssid = MacWifiParse::ipconfigSummary(summary.output);
        if (!ssid.isEmpty())
            return ssid;
    }

    // 回退：networksetup。较新的系统上没给定位权限时这里可能读不到 SSID，
    // 但老系统和给了权限的终端里依然可用。
    const ProcessResult result = runProcess(
        QStringLiteral("/usr/sbin/networksetup"),
        {QStringLiteral("-getairportnetwork"), device});
    if (result.exitCode == 0) {
        const QString ssid = MacWifiParse::networksetupAirport(result.output);
        if (!ssid.isEmpty())
            return ssid;
    }

    // 最后兜底：system_profiler（比较慢，但一般不需要任何权限）
    const ProcessResult profiler = runProcess(
        QStringLiteral("/usr/sbin/system_profiler"),
        {QStringLiteral("SPAirPortDataType")},
        20000);
    return MacWifiParse::systemProfiler(profiler.output);
#else
    return QString();
#endif
}

bool WifiInfo::currentProfile(QString *ssid, QString *password, QString *authType)
{
#ifdef Q_OS_WIN
    const QString currentSsid = WifiInfo::currentSsid();
    if (currentSsid.isEmpty())
        return false;

    // 注意：不能传 name="xxx"，QProcess 会把内嵌引号转义掉导致 netsh 报错；
    // 不带引号时 SSID 里有空格也能正常识别。
    const QString output = runNetsh({QStringLiteral("wlan"),
                                     QStringLiteral("show"),
                                     QStringLiteral("profile"),
                                     QStringLiteral("name=%1").arg(currentSsid),
                                     QStringLiteral("key=clear")});
    if (output.isEmpty())
        return false;

    QString foundKey;
    QString foundAuth;

    const QStringList lines = output.split(QLatin1Char('\n'));
    for (const QString &rawLine : lines) {
        QString label;
        QString value;
        if (!splitLine(rawLine, &label, &value))
            continue;

        // profile 输出里有 "SSID 数量 : 1" 和 "SSID 名称 : xxx" 两行，
        // SSID 直接用 currentSsid() 的结果，这里不再解析，避免取错。
        // 中文 "关键内容" / "密钥内容"，英文 "key content"
        if (foundKey.isEmpty()
                 && (label.contains(QStringLiteral("关键内容"))
                     || label.contains(QStringLiteral("密钥内容"))
                     || label.contains(QStringLiteral("key content"))))
            foundKey = value;
        // 中文 "身份验证"，英文 "authentication"
        else if (foundAuth.isEmpty()
                 && (label.contains(QStringLiteral("身份验证"))
                     || label.contains(QStringLiteral("authentication"))))
            foundAuth = value;
    }

    const bool open = foundKey.isEmpty() && foundAuth.contains(QStringLiteral("开放"), Qt::CaseInsensitive);

    if (ssid)
        *ssid = currentSsid;
    if (password)
        *password = foundKey;
    if (authType) {
        if (open)
            *authType = QStringLiteral("nopass");
        else if (foundAuth.contains(QStringLiteral("WEP"), Qt::CaseInsensitive))
            *authType = QStringLiteral("WEP");
        else
            *authType = QStringLiteral("WPA");
    }
    return true;
#elif defined(Q_OS_MACOS)
    const QString currentSsid = WifiInfo::currentSsid();
    if (currentSsid.isEmpty())
        return false;

    // macOS 把 WiFi 密码存在登录钥匙串里，用 security 命令读取。
    // 第一次读取时系统会弹一次授权框（「security 想访问钥匙串」），点「允许」即可；
    // 用户拒绝的话密码就当没读到，界面会提示二维码只能包含网络名。
    // 超时给得比较宽：授权框要等用户点，而这段代码在后台线程跑，不卡界面。
    const QString security = QStringLiteral("/usr/bin/security");
    ProcessResult result = runProcess(
        security,
        {QStringLiteral("find-generic-password"),
         QStringLiteral("-D"), QStringLiteral("AirPort network password"),
         QStringLiteral("-a"), currentSsid,
         QStringLiteral("-w")},
        120000);

    // 较新的系统里描述可能不是 "AirPort network password"，
    // 换用服务名 "AirPort" + 账号（= SSID）再查一次
    if (result.exitCode != 0 && !currentSsid.isEmpty()) {
        result = runProcess(
            security,
            {QStringLiteral("find-generic-password"),
             QStringLiteral("-s"), QStringLiteral("AirPort"),
             QStringLiteral("-a"), currentSsid,
             QStringLiteral("-w")},
            120000);
    }

    QString foundKey;
    QString foundAuth = QStringLiteral("WPA");
    if (result.exitCode == 0) {
        foundKey = chomp(result.output);
    } else if (result.error.contains(QStringLiteral("could not be found"))
               || result.error.contains(QStringLiteral("not be found"))) {
        // 钥匙串里没有这个网络的密码条目，多半是开放网络
        foundAuth = QStringLiteral("nopass");
    }

    if (ssid)
        *ssid = currentSsid;
    if (password)
        *password = foundKey;
    if (authType)
        *authType = foundAuth;
    return true;
#else
    Q_UNUSED(ssid);
    Q_UNUSED(password);
    Q_UNUSED(authType);
    return false;
#endif
}

QString WifiInfo::buildWifiQrText(const QString &ssid, const QString &password, const QString &authType)
{
    const QString type = authType.isEmpty() ? QStringLiteral("WPA") : authType;
    QString text = QStringLiteral("WIFI:T:%1;S:%2;")
                       .arg(type, escapeField(ssid));
    if (type.compare(QStringLiteral("nopass"), Qt::CaseInsensitive) != 0)
        text += QStringLiteral("P:%1;").arg(escapeField(password));
    text += QStringLiteral(";");
    return text;
}

QString WifiInfo::escapeField(const QString &value)
{
    QString escaped;
    escaped.reserve(value.size() + 8);
    for (const QChar ch : value) {
        switch (ch.unicode()) {
        case '\\':
        case ';':
        case ',':
        case ':':
        case '"':
            escaped += QLatin1Char('\\');
            break;
        default:
            break;
        }
        escaped += ch;
    }
    return escaped;
}