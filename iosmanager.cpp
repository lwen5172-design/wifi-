#include "iosmanager.h"

#include <QDeadlineTimer>
#include <QHash>
#include <QIODevice>
#include <QThread>
#include <QVariant>
#include <QXmlStreamReader>

#ifdef Q_OS_MACOS
#include <QLocalSocket>
#else
#include <QTcpSocket>
#endif

namespace {

constexpr quint16 kLockdownPort = 62078;

#ifdef Q_OS_MACOS
// macOS 的 usbmuxd 由系统自带，监听在 /var/run/usbmuxd 这个 Unix 域套接字上
const char *kUsbmuxdSocketPath = "/var/run/usbmuxd";
#else
constexpr quint16 kUsbmuxdPort = 27015;
#endif

// usbmuxd 的协议常量
constexpr quint32 kMuxVersion = 1;
constexpr quint32 kMuxTypePlist = 8;

// ---- plist 解析（只覆盖 usbmuxd / lockdownd 会返回的几种类型） ----

QVariant parsePlistValue(QXmlStreamReader &xml);

QVariant parsePlistValue(QXmlStreamReader &xml)
{
    const QString tag = xml.name().toString();
    if (tag == QLatin1String("dict")) {
        QVariantMap map;
        QString key;
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement()) {
                if (xml.name().toString() == QLatin1String("key")) {
                    key = xml.readElementText();
                } else {
                    map.insert(key, parsePlistValue(xml));
                }
            } else if (xml.isEndElement() && xml.name().toString() == QLatin1String("dict")) {
                break;
            }
        }
        return map;
    }
    if (tag == QLatin1String("array")) {
        QVariantList list;
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement())
                list << parsePlistValue(xml);
            else if (xml.isEndElement() && xml.name().toString() == QLatin1String("array"))
                break;
        }
        return list;
    }
    if (tag == QLatin1String("string"))
        return xml.readElementText();
    if (tag == QLatin1String("integer"))
        return xml.readElementText().toLongLong();
    if (tag == QLatin1String("true")) {
        xml.skipCurrentElement();
        return true;
    }
    if (tag == QLatin1String("false")) {
        xml.skipCurrentElement();
        return false;
    }
    if (tag == QLatin1String("data")) {
        const QString text = xml.readElementText();
        return QByteArray::fromBase64(text.toLatin1());
    }
    return QVariant();
}

QVariant parsePlist(const QByteArray &data)
{
    QXmlStreamReader xml(data);
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name().toString() == QLatin1String("plist")) {
            while (!xml.atEnd()) {
                xml.readNext();
                if (xml.isStartElement())
                    return parsePlistValue(xml);
                if (xml.isEndElement())
                    break;
            }
        }
    }
    return QVariant();
}

// ---- 简单的阻塞读写（都带超时，总耗时最多几秒） ----

// macOS 连的是 Unix 域套接字（QLocalSocket），Windows 连 127.0.0.1:27015（QTcpSocket）。
// 两者的 QIODevice 接口一致，下面的读写函数都能复用。
#ifdef Q_OS_MACOS
using MuxSocket = QLocalSocket;
#else
using MuxSocket = QTcpSocket;
#endif

// 连本机的 usbmuxd。macOS 走系统自带的 Unix 域套接字，Windows 走 TCP 27015。
bool connectUsbmuxd(MuxSocket &socket, int timeoutMs)
{
#ifdef Q_OS_MACOS
    socket.connectToServer(QString::fromLatin1(kUsbmuxdSocketPath));
#else
    socket.connectToHost(QHostAddress::LocalHost, kUsbmuxdPort);
#endif
    return socket.waitForConnected(timeoutMs);
}

bool readExact(MuxSocket &socket, char *buffer, qint64 size, int timeoutMs)
{
    QDeadlineTimer deadline(timeoutMs);
    qint64 got = 0;
    while (got < size) {
        if (socket.bytesAvailable() <= 0) {
            const int left = int(deadline.remainingTime());
            if (left <= 0 || !socket.waitForReadyRead(left))
                return false;
        }
        const qint64 n = socket.read(buffer + got, size - got);
        if (n <= 0)
            return false;
        got += n;
    }
    return true;
}

bool writeAll(MuxSocket &socket, const QByteArray &data, int timeoutMs)
{
    if (socket.write(data) != data.size())
        return false;
    return socket.waitForBytesWritten(timeoutMs);
}

QByteArray muxPlistRequest(const QVariantMap &map)
{
    QByteArray body;
    body += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
            "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
            "<plist version=\"1.0\"><dict>";
    for (auto it = map.cbegin(); it != map.cend(); ++it) {
        body += "<key>" + it.key().toUtf8() + "</key>";
        switch (it.value().typeId()) {
        case QMetaType::Int:
        case QMetaType::UInt:
        case QMetaType::LongLong:
        case QMetaType::ULongLong:
            body += "<integer>" + QByteArray::number(it.value().toLongLong()) + "</integer>";
            break;
        default:
            body += "<string>" + it.value().toString().toUtf8() + "</string>";
            break;
        }
    }
    body += "</dict></plist>";
    return body;
}

// usbmuxd: 4 字节小端长度(含头) + 版本 + 消息类型 + tag + plist
QByteArray muxFrame(const QVariantMap &request, quint32 tag = 1)
{
    const QByteArray plist = muxPlistRequest(request);
    QByteArray frame;
    frame.reserve(16 + plist.size());
    const quint32 length = quint32(16 + plist.size());
    for (quint32 value : {length, kMuxVersion, kMuxTypePlist, tag}) {
        frame.append(char(value & 0xff));
        frame.append(char((value >> 8) & 0xff));
        frame.append(char((value >> 16) & 0xff));
        frame.append(char((value >> 24) & 0xff));
    }
    frame += plist;
    return frame;
}

// lockdownd: 4 字节大端长度 + plist
QByteArray lockdownRequest(const QString &key)
{
    QByteArray plist;
    plist += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
             "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
             "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
             "<plist version=\"1.0\"><dict>"
             "<key>Label</key><string>WifiDebugHelper</string>"
             "<key>Request</key><string>GetValue</string>"
             "<key>Key</key><string>";
    plist += key.toUtf8();
    plist += "</string></dict></plist>";

    QByteArray out;
    const quint32 length = quint32(plist.size());
    out.append(char((length >> 24) & 0xff));
    out.append(char((length >> 16) & 0xff));
    out.append(char((length >> 8) & 0xff));
    out.append(char(length & 0xff));
    out += plist;
    return out;
}

bool readLockdownPlist(MuxSocket &socket, QVariant *value, int timeoutMs)
{
    char sizeBuf[4] = {};
    if (!readExact(socket, sizeBuf, 4, timeoutMs))
        return false;
    const quint32 length = (quint8(sizeBuf[0]) << 24) | (quint8(sizeBuf[1]) << 16)
                           | (quint8(sizeBuf[2]) << 8) | quint8(sizeBuf[3]);
    if (length == 0 || length > (16u << 20))
        return false;
    QByteArray payload(int(length), Qt::Uninitialized);
    if (!readExact(socket, payload.data(), qint64(length), timeoutMs))
        return false;
    *value = parsePlist(payload);
    return true;
}

} // namespace

IosManager::IosManager(QObject *parent)
    : QObject(parent)
{
}

bool IosManager::isAvailable() const
{
    MuxSocket probe;
    return connectUsbmuxd(probe, 500);
}

bool IosManager::fetchDeviceList(QList<IosManager::DeviceEntry> *out, QString *error) const
{
    MuxSocket socket;
    if (!connectUsbmuxd(socket, 800)) {
        if (error)
            *error = QStringLiteral("没有找到苹果移动设备服务（usbmuxd）");
        return false;
    }

    QVariantMap request;
    request.insert(QStringLiteral("MessageType"), QStringLiteral("ListDevices"));
    request.insert(QStringLiteral("ProgName"), QStringLiteral("WifiDebugHelper"));
    request.insert(QStringLiteral("ClientVersionString"), QStringLiteral("WifiDebugHelper"));
    request.insert(QStringLiteral("kLibUSBMuxVersion"), 3);
    if (!writeAll(socket, muxFrame(request), 1500)) {
        if (error)
            *error = QStringLiteral("写入 usbmuxd 失败");
        return false;
    }

    char header[16] = {};
    if (!readExact(socket, header, 16, 1500)) {
        if (error)
            *error = QStringLiteral("读取 usbmuxd 响应失败");
        return false;
    }
    const quint32 length = quint8(header[0]) | (quint8(header[1]) << 8)
                           | (quint8(header[2]) << 16) | (quint32(quint8(header[3])) << 24);
    if (length < 16 || length > (16u << 20)) {
        if (error)
            *error = QStringLiteral("usbmuxd 响应长度异常");
        return false;
    }
    QByteArray payload(int(length - 16), Qt::Uninitialized);
    if (!readExact(socket, payload.data(), qint64(length - 16), 1500)) {
        if (error)
            *error = QStringLiteral("读取 usbmuxd 设备列表失败");
        return false;
    }

    const QVariant parsed = parsePlist(payload);
    const QVariantList list =
        parsed.toMap().value(QStringLiteral("DeviceList")).toList();
    for (const QVariant &item : list) {
        const QVariantMap dev = item.toMap();
        const QVariantMap props = dev.value(QStringLiteral("Properties")).toMap();
        DeviceEntry entry;
        entry.deviceId = quint32(props.value(QStringLiteral("DeviceID")).toInt());
        entry.udid = props.value(QStringLiteral("SerialNumber")).toString();
        entry.connection = props.value(QStringLiteral("ConnectionType")).toString();
        if (entry.deviceId == 0 || entry.udid.isEmpty())
            continue;
        if (out)
            *out << entry;
    }
    return true;
}

IosDevice IosManager::enrichDevice(const DeviceEntry &entry) const
{
    IosDevice device;
    device.udid = entry.udid;
    device.connection = entry.connection;

    MuxSocket socket;
    if (!connectUsbmuxd(socket, 800))
        return device;

    // usbmuxd 的 Connect：要求连到设备的 lockdownd 端口，端口号要用网络字节序
    const quint16 networkPort = quint16((kLockdownPort >> 8) | (kLockdownPort << 8));
    QVariantMap request;
    request.insert(QStringLiteral("MessageType"), QStringLiteral("Connect"));
    request.insert(QStringLiteral("ProgName"), QStringLiteral("WifiDebugHelper"));
    request.insert(QStringLiteral("ClientVersionString"), QStringLiteral("WifiDebugHelper"));
    request.insert(QStringLiteral("DeviceID"), int(entry.deviceId));
    request.insert(QStringLiteral("PortNumber"), int(networkPort));
    if (!writeAll(socket, muxFrame(request, 2), 1500))
        return device;

    char header[16] = {};
    if (!readExact(socket, header, 16, 1500))
        return device;
    const quint32 length = quint8(header[0]) | (quint8(header[1]) << 8)
                           | (quint8(header[2]) << 16) | (quint32(quint8(header[3])) << 24);
    if (length < 16 || length > (1u << 20))
        return device;
    QByteArray payload(int(length - 16), Qt::Uninitialized);
    if (!readExact(socket, payload.data(), qint64(length - 16), 1500))
        return device;
    const QVariantMap result = parsePlist(payload).toMap();
    if (result.value(QStringLiteral("MessageType")).toString() != QLatin1String("Result")
        || result.value(QStringLiteral("Number")).toInt() != 0) {
        // 手机没点过“信任此电脑”时这里会失败，保留 UDID 就够了
        return device;
    }

    // 连上 lockdownd 了，逐个问需要的信息
    for (const QString &key :
         {QStringLiteral("DeviceName"), QStringLiteral("ProductType"),
          QStringLiteral("ProductVersion")}) {
        if (!writeAll(socket, lockdownRequest(key), 1200))
            break;
        QVariant reply;
        if (!readLockdownPlist(socket, &reply, 1200))
            break;
        const QVariantMap map = reply.toMap();
        if (map.contains(QStringLiteral("Error")))
            break;
        const QString value = map.value(QStringLiteral("Value")).toString();
        if (key == QLatin1String("DeviceName"))
            device.name = value;
        else if (key == QLatin1String("ProductType"))
            device.productType = value;
        else if (key == QLatin1String("ProductVersion"))
            device.version = value;
    }
    device.paired = !device.name.isEmpty();
    socket.abort();
    return device;
}

void IosManager::listDevices()
{
    // usbmuxd 查询很快，但握手可能被设备卡住，放后台线程跑，避免界面卡顿
    auto *worker = QThread::create([this]() {
        QList<DeviceEntry> entries;
        QString error;
        if (!fetchDeviceList(&entries, &error)) {
            emit devicesListed({});
            return;
        }

        // 同一台设备可能同时有 USB 和 Network 两条，优先显示 USB
        QHash<QString, DeviceEntry> unique;
        QStringList order;
        for (const DeviceEntry &entry : entries) {
            auto it = unique.find(entry.udid);
            if (it == unique.end()) {
                unique.insert(entry.udid, entry);
                order << entry.udid;
            } else if (it->connection != QLatin1String("USB")
                       && entry.connection == QLatin1String("USB")) {
                it->connection = entry.connection;
                it->deviceId = entry.deviceId;
            }
        }

        QList<IosDevice> devices;
        for (const QString &udid : order)
            devices << enrichDevice(unique.value(udid));
        emit devicesListed(devices);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}