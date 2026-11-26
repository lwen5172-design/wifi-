#ifndef MACWIFIPARSE_H
#define MACWIFIPARSE_H

#include <QString>
#include <QStringList>

// macOS 上读取 WiFi 信息的几个命令输出解析器。
// 单独抽出来是因为它们只是纯文本处理：不依赖具体平台宏，
// 可以在任何平台上单独验证（见 wifiinfo.cpp 里的使用）。
namespace MacWifiParse {

// ipconfig getsummary <iface> 的输出里有一行形如 "  SSID : MyWifi"。
// 没给定位权限时值可能是 <redacted>，这种情况返回空，好让调用方继续兜底。
inline QString ipconfigSummary(const QString &output)
{
    const QStringList lines = output.split(QLatin1Char('\n'));
    for (const QString &rawLine : lines) {
        const QString line = rawLine.trimmed();
        if (!line.startsWith(QStringLiteral("SSID")))
            continue;
        const int sep = line.indexOf(QLatin1Char(':'));
        if (sep < 0)
            continue;
        const QString ssid = line.mid(sep + 1).trimmed();
        if (ssid.isEmpty() || ssid.contains(QStringLiteral("redacted"), Qt::CaseInsensitive))
            continue;
        return ssid;
    }
    return QString();
}

// networksetup -getairportnetwork 的输出形如 "Current Wi-Fi Network: MySSID"；
// 没连上时是 "You are not associated with an AirPort network."，自然匹配不上。
inline QString networksetupAirport(const QString &output)
{
    const QString prefix = QStringLiteral("Current Wi-Fi Network:");
    const int pos = output.indexOf(prefix);
    if (pos < 0)
        return QString();
    QString ssid = output.mid(pos + prefix.size());
    const int newline = ssid.indexOf(QLatin1Char('\n'));
    if (newline >= 0)
        ssid.truncate(newline);
    if (ssid.endsWith(QLatin1Char('\r')))
        ssid.chop(1);
    // 去掉冒号后的那一个格式空格，但保留 SSID 自身的首尾空格
    if (ssid.startsWith(QLatin1Char(' ')))
        ssid.remove(0, 1);
    return ssid;
}

inline int leadingIndent(const QString &line)
{
    int indent = 0;
    for (const QChar ch : line) {
        if (ch == QLatin1Char(' '))
            ++indent;
        else if (ch == QLatin1Char('\t'))
            indent += 4;
        else
            break;
    }
    return indent;
}

// system_profiler SPAirPortDataType 的输出是英文的（不受系统语言影响），结构大致是
//     Current Network Information:
//       MySSID:
//         PHY Mode: 802.11ax
//         Security: WPA2 Personal
// 取 "Current Network Information:" 下面缩进更深的第一行作为 SSID。
inline QString systemProfiler(const QString &output)
{
    const QStringList lines = output.split(QLatin1Char('\n'));
    int current = -1;
    for (int i = 0; i < lines.size(); ++i) {
        if (lines.at(i).trimmed() == QStringLiteral("Current Network Information:")) {
            current = i;
            break;
        }
    }
    if (current < 0)
        return QString();

    const int baseIndent = leadingIndent(lines.at(current));
    int first = current + 1;
    while (first < lines.size() && lines.at(first).trimmed().isEmpty())
        ++first;
    if (first >= lines.size() || leadingIndent(lines.at(first)) <= baseIndent)
        return QString();

    QString ssid = lines.at(first).trimmed();
    if (ssid.endsWith(QLatin1Char(':')))
        ssid.chop(1);
    if (ssid.contains(QStringLiteral("redacted"), Qt::CaseInsensitive))
        return QString();
    return ssid;
}

} // namespace MacWifiParse

#endif // MACWIFIPARSE_H
