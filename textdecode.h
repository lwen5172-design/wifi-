#ifndef TEXTDECODE_H
#define TEXTDECODE_H

#include <QByteArray>
#include <QString>
#include <QStringConverter>

// 子进程输出优先按 UTF-8 解码（adb / hdc 在 Windows 上都输出 UTF-8，
// 包括本地化的错误消息），遇到非法字节再退回系统本地编码。
inline QString decodeProcessOutput(const QByteArray &data)
{
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString text = decoder.decode(data);
    if (!decoder.hasError())
        return text;
    return QString::fromLocal8Bit(data);
}

#endif // TEXTDECODE_H