#include "wifiinfo.h"

#include <QProcess>
#include <QStringList>
#include <QRegularExpression>

namespace {

// netsh 在无控制台的 GUI 进程里输出 UTF-8，在控制台里是系统 ANSI 编码。
// 先按 UTF-8 解码，出现替换字符再回退到本地编码。
QString decodeConsoleOutput(const QByteArray &raw)
{
    const QString utf8 = QString::fromUtf8(raw);
    if (!utf8.contains(QChar::ReplacementCharacter))
        return utf8;
    return QString::fromLocal8Bit(raw);
}

QString runNetsh(const QStringList &args)
{
    QProcess process;
    process.start(QStringLiteral("netsh"), args);
    if (!process.waitForStarted(3000))
        return QString();
    if (!process.waitForFinished(8000)) {
        process.kill();
        process.waitForFinished(1000);
        return QString();
    }
    return decodeConsoleOutput(process.readAllStandardOutput());
}

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
#else
    Q_UNUSED(runNetsh);
#endif
    return QString();
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