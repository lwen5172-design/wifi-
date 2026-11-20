#include "mainwindow.h"

#include "wifiinfo.h"

#include <QDateTime>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRandomGenerator>
#include <QScreen>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

#include "qrcodegen.hpp"

namespace {

// 把 qrcodegen 的模块矩阵画成 QImage，模块按整数倍像素放大，保证边缘锐利
QImage renderToImage(const qrcodegen::QrCode &qr, int targetPixels, int quietZone = 3)
{
    const int size = qr.getSize();
    const int totalModules = size + quietZone * 2;
    const int scale = qMax(1, targetPixels / totalModules);
    const int imagePixels = totalModules * scale;

    QImage image(imagePixels, imagePixels, QImage::Format_ARGB32);
    image.fill(Qt::white);

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            if (!qr.getModule(x, y))
                continue;
            for (int dy = 0; dy < scale; ++dy) {
                auto *line = reinterpret_cast<QRgb *>(image.scanLine((y + quietZone) * scale + dy));
                for (int dx = 0; dx < scale; ++dx)
                    line[(x + quietZone) * scale + dx] = qRgb(0, 0, 0);
            }
        }
    }
    return image;
}

const char *kPairingService = "_adb-tls-pairing";
const char *kConnectService = "_adb-tls-connect";

int kPairScanTimeoutMs = 120 * 1000;
int kConnectWaitMs = 30 * 1000;

QString randomToken(int length)
{
    static const QString alphabet =
        QStringLiteral("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
    QString token;
    token.reserve(length);
    auto *rng = QRandomGenerator::system();
    for (int i = 0; i < length; ++i)
        token += alphabet.at(rng->bounded(alphabet.size()));
    return token;
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_adb(new AdbManager(this))
{
    setWindowTitle(QStringLiteral("WiFi 无线调试助手"));
    resize(1180, 700);
    setMinimumSize(980, 620);

    if (const QScreen *screen = QGuiApplication::primaryScreen()) {
        const QRect avail = screen->availableGeometry();
        move(avail.center() - QPoint(width() / 2, height() / 2));
    }

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(buildLeftPanel());
    splitter->addWidget(buildRightPanel());
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({420, 760});
    setCentralWidget(splitter);

    connect(m_adb, &AdbManager::logMessage, this, &MainWindow::appendLog);
    connect(m_adb, &AdbManager::pairFinished, this, &MainWindow::onPairFinished);
    connect(m_adb, &AdbManager::connectFinished, this, &MainWindow::onConnectFinished);
    connect(m_adb, &AdbManager::devicesListed, this, &MainWindow::onDevicesListed);
    connect(m_adb, &AdbManager::mdnsListed, this, &MainWindow::onMdnsListed);

    // 设备列表每 4 秒静默刷新
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(4000);
    connect(m_pollTimer, &QTimer::timeout, this, &MainWindow::refreshDeviceList);
    m_pollTimer->start();

    // 二维码扫描期间每秒轮询一次 mDNS
    m_pairWatchTimer = new QTimer(this);
    m_pairWatchTimer->setInterval(1000);
    connect(m_pairWatchTimer, &QTimer::timeout, this, &MainWindow::onPairWatchTick);

    refreshWifi();
    regeneratePairQr();

    if (!m_adb->isAvailable())
        appendLog(QStringLiteral("警告：未找到 adb，请安装 platform-tools 并加入 PATH"));
    else
        appendLog(QStringLiteral("adb: %1").arg(m_adb->adbPath()));

    refreshDeviceList();
}

MainWindow::~MainWindow() = default;

QWidget *MainWindow::buildLeftPanel()
{
    auto *panel = new QWidget;
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(12, 12, 12, 12);

    m_tabs = new QTabWidget;
    m_tabs->addTab(buildQrPairTab(), QStringLiteral("扫码配对"));
    m_tabs->addTab(buildWifiTab(), QStringLiteral("WiFi 入网"));
    layout->addWidget(m_tabs);
    return panel;
}

QWidget *MainWindow::buildQrPairTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setSpacing(10);

    auto *hint = new QLabel(
        QStringLiteral("手机上：设置 → 开发者选项 → 无线调试 → 使用二维码配对设备，"
                       "然后扫下面这个码。配对和连接一步完成，不需要输配对码。"));
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color: #555;"));

    m_adbQrLabel = new QLabel;
    m_adbQrLabel->setAlignment(Qt::AlignCenter);
    m_adbQrLabel->setMinimumSize(360, 360);
    m_adbQrLabel->setStyleSheet(QStringLiteral("QLabel { background: white; border: 1px solid #ddd; }"));

    m_adbQrStatus = new QLabel(QStringLiteral("等待扫码…"));
    m_adbQrStatus->setWordWrap(true);
    m_adbQrStatus->setAlignment(Qt::AlignCenter);
    m_adbQrStatus->setStyleSheet(QStringLiteral("color: #666;"));

    auto *refresh = new QPushButton(QStringLiteral("重新生成配对码"));
    connect(refresh, &QPushButton::clicked, this, &MainWindow::regeneratePairQr);

    layout->addWidget(hint);
    layout->addWidget(m_adbQrLabel, 1, Qt::AlignCenter);
    layout->addWidget(m_adbQrStatus);
    layout->addWidget(refresh);
    return page;
}

QWidget *MainWindow::buildWifiTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setSpacing(10);

    auto *hint = new QLabel(
        QStringLiteral("手机还没连上这个 WiFi 时先扫这个码；扫完入网后，再用左侧的「扫码配对」"
                       "或右侧的配对码完成调试授权。"));
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color: #555;"));

    m_wifiQrLabel = new QLabel;
    m_wifiQrLabel->setAlignment(Qt::AlignCenter);
    m_wifiQrLabel->setMinimumSize(360, 360);
    m_wifiQrLabel->setStyleSheet(QStringLiteral("QLabel { background: white; border: 1px solid #ddd; }"));

    m_ssidLabel = new QLabel;
    m_ssidLabel->setAlignment(Qt::AlignCenter);
    m_ssidLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont ssidFont = m_ssidLabel->font();
    ssidFont.setPointSize(ssidFont.pointSize() + 2);
    ssidFont.setBold(true);
    m_ssidLabel->setFont(ssidFont);

    m_passwordTip = new QLabel;
    m_passwordTip->setWordWrap(true);
    m_passwordTip->setAlignment(Qt::AlignCenter);
    m_passwordTip->setStyleSheet(QStringLiteral("color: #666;"));

    auto *refresh = new QPushButton(QStringLiteral("重新读取 WiFi"));
    connect(refresh, &QPushButton::clicked, this, &MainWindow::refreshWifi);

    layout->addWidget(hint);
    layout->addWidget(m_wifiQrLabel, 1, Qt::AlignCenter);
    layout->addWidget(m_ssidLabel);
    layout->addWidget(m_passwordTip);
    layout->addWidget(refresh);
    return page;
}

QWidget *MainWindow::buildRightPanel()
{
    auto *panel = new QWidget;
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(10);

    // ---- 设备列表 ----
    auto *deviceGroup = new QGroupBox(QStringLiteral("设备列表"));
    auto *deviceLayout = new QVBoxLayout(deviceGroup);

    m_deviceTable = new QTableWidget(0, 3);
    m_deviceTable->setHorizontalHeaderLabels(
        {QStringLiteral("地址"), QStringLiteral("状态"), QStringLiteral("型号")});
    m_deviceTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_deviceTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_deviceTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_deviceTable->verticalHeader()->setVisible(false);
    m_deviceTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_deviceTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_deviceTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_deviceTable->setMinimumHeight(140);
    connect(m_deviceTable, &QTableWidget::itemSelectionChanged,
            this, [this]() { m_connectButton->setEnabled(!selectedAddress().isEmpty()); });
    connect(m_deviceTable, &QTableWidget::cellDoubleClicked,
            this, [this](int, int) { connectSelectedDevice(); });

    auto *deviceButtons = new QHBoxLayout;
    auto *refreshButton = new QPushButton(QStringLiteral("刷新"));
    m_connectButton = new QPushButton(QStringLiteral("连接"));
    m_disconnectButton = new QPushButton(QStringLiteral("断开"));
    m_connectButton->setEnabled(false);
    m_disconnectButton->setEnabled(true);
    connect(refreshButton, &QPushButton::clicked, this, &MainWindow::refreshDeviceList);
    connect(m_connectButton, &QPushButton::clicked, this, &MainWindow::connectSelectedDevice);
    connect(m_disconnectButton, &QPushButton::clicked, this, &MainWindow::disconnectSelectedDevice);
    deviceButtons->addWidget(refreshButton);
    deviceButtons->addWidget(m_connectButton);
    deviceButtons->addWidget(m_disconnectButton);
    deviceButtons->addStretch();

    deviceLayout->addWidget(m_deviceTable);
    deviceLayout->addLayout(deviceButtons);

    // ---- 配对码配对 ----
    auto *codeGroup = new QGroupBox(QStringLiteral("配对码配对（备选）"));
    auto *codeLayout = new QVBoxLayout(codeGroup);
    auto *codeHint = new QLabel(
        QStringLiteral("如果扫码不行，在无线调试页点「使用配对码配对设备」，把手机显示的 "
                       "IP:端口 和 6 位配对码填进来。"));
    codeHint->setWordWrap(true);
    codeHint->setStyleSheet(QStringLiteral("color: #666;"));

    auto *form = new QHBoxLayout;
    m_pairEndpoint = new QLineEdit;
    m_pairEndpoint->setPlaceholderText(QStringLiteral("192.168.1.5:37123"));
    m_pairCode = new QLineEdit;
    m_pairCode->setPlaceholderText(QStringLiteral("配对码"));
    m_pairCode->setMaxLength(6);
    m_pairCode->setMaximumWidth(110);
    m_pairButton = new QPushButton(QStringLiteral("配对"));
    connect(m_pairButton, &QPushButton::clicked, this, &MainWindow::pairWithCode);
    form->addWidget(new QLabel(QStringLiteral("地址")));
    form->addWidget(m_pairEndpoint, 1);
    form->addWidget(new QLabel(QStringLiteral("配对码")));
    form->addWidget(m_pairCode);
    form->addWidget(m_pairButton);

    codeLayout->addWidget(codeHint);
    codeLayout->addLayout(form);

    // ---- 日志 ----
    m_logView = new QPlainTextEdit;
    m_logView->setReadOnly(true);
    m_logView->setMaximumBlockCount(3000);
    QFont mono(QStringLiteral("Consolas"));
    mono.setStyleHint(QFont::Monospace);
    m_logView->setFont(mono);

    layout->addWidget(deviceGroup);
    layout->addWidget(codeGroup);
    layout->addWidget(m_logView, 1);
    return panel;
}

// ---------------------------------------------------------------- 二维码

void MainWindow::renderQr(QLabel *label, const QString &text)
{
    if (!label)
        return;
    try {
        const QByteArray utf8 = text.toUtf8();
        const qrcodegen::QrCode qr =
            qrcodegen::QrCode::encodeText(utf8.constData(), qrcodegen::QrCode::Ecc::MEDIUM);

        QPixmap pixmap = QPixmap::fromImage(renderToImage(qr, 330));
        pixmap.setDevicePixelRatio(devicePixelRatioF());
        label->setText(QString());
        label->setPixmap(pixmap);
    } catch (const std::exception &e) {
        label->setPixmap(QPixmap());
        label->setText(QStringLiteral("二维码生成失败"));
        appendLog(QStringLiteral("二维码生成失败：%1").arg(QString::fromUtf8(e.what())));
    }
}

void MainWindow::refreshWifi()
{
    QString ssid;
    QString password;
    QString authType;

    if (!WifiInfo::currentProfile(&ssid, &password, &authType)) {
        m_ssid.clear();
        m_password.clear();
        m_authType.clear();
        m_ssidLabel->setText(QStringLiteral("未读取到当前 WiFi"));
        m_wifiQrLabel->setPixmap(QPixmap());
        m_wifiQrLabel->setText(QStringLiteral("无二维码"));
        appendLog(QStringLiteral("读取 WiFi 配置失败：请确认已连接 WiFi"));
        return;
    }

    m_ssid = ssid;
    m_password = password;
    m_authType = authType;

    m_ssidLabel->setText(QStringLiteral("网络：%1").arg(m_ssid));

    if (m_authType.compare(QStringLiteral("nopass"), Qt::CaseInsensitive) == 0)
        m_passwordTip->setText(QStringLiteral("开放网络，扫码后手机直接加入。"));
    else if (m_password.isEmpty())
        m_passwordTip->setText(QStringLiteral("警告：没能读取到 WiFi 密码，这个二维码只能让手机看到网络名。"));
    else
        m_passwordTip->setText(QStringLiteral("密码仅用于在本机内存中生成二维码，不显示、不写日志。"));

    renderQr(m_wifiQrLabel, WifiInfo::buildWifiQrText(m_ssid, m_password, m_authType));
    appendLog(QStringLiteral("已读取 WiFi「%1」").arg(m_ssid));
}

// -------------------------------------------------------- 二维码配对流程

void MainWindow::regeneratePairQr()
{
    startQrPairing();
}

void MainWindow::startQrPairing()
{
    if (!m_adb->isAvailable()) {
        setPairStatus(QStringLiteral("未找到 adb，无法进行二维码配对"));
        return;
    }

    if (m_phase != PairPhase::Idle)
        m_pairWatchTimer->stop();

    // 服务名要和 QR 里的 S 字段一致，手机扫码后会用这个名字广播 mDNS
    m_pairName = QStringLiteral("wifi-dbg-%1").arg(randomToken(8));
    m_pairPassword = randomToken(12);
    m_pairIp.clear();
    m_triedConnects.clear();
    m_phase = PairPhase::WaitingScan;
    m_pairDeadlineMs = QDateTime::currentMSecsSinceEpoch() + kPairScanTimeoutMs;

    const QString payload = QStringLiteral("WIFI:T:ADB;S:%1;P:%2;;")
                                .arg(m_pairName, m_pairPassword);
    renderQr(m_adbQrLabel, payload);

    setPairStatus(QStringLiteral("等待手机扫码…请打开「使用二维码配对设备」扫描左侧二维码"));
    appendLog(QStringLiteral("已生成配对二维码，等待扫码（%1 秒超时）")
                  .arg(kPairScanTimeoutMs / 1000));

    m_pairWatchTimer->start();
    // 立刻查一次，避免首轮要等 1 秒
    m_adb->mdnsServices(true);
}

void MainWindow::onPairWatchTick()
{
    if (m_phase == PairPhase::WaitingConnect) {
        if (QDateTime::currentMSecsSinceEpoch() > m_pairDeadlineMs) {
            stopQrPairing(QStringLiteral("已配对成功，但没等到自动连接。"
                                         "请在手机无线调试页查看 IP:端口，用「连接」按钮手动连。"));
            return;
        }
    } else if (m_phase == PairPhase::WaitingScan) {
        if (QDateTime::currentMSecsSinceEpoch() > m_pairDeadlineMs) {
            stopQrPairing(QStringLiteral("超时：手机没有出现在 adb 的服务列表里。"
                                         "请确认手机和电脑在同一个 WiFi 下。"));
            return;
        }
    }
    m_adb->mdnsServices(true);
}

void MainWindow::onMdnsListed(const QList<MdnsService> &services)
{
    m_lastServices = services;
    updateDeviceTable();
    handleQrPairing(services);
}

void MainWindow::handleQrPairing(const QList<MdnsService> &services)
{
    if (m_phase == PairPhase::Idle)
        return;

    if (m_phase == PairPhase::WaitingScan) {
        // 手机扫码后会广播 _adb-tls-pairing，实例名等于二维码里的 S 字段
        QList<MdnsService> pairing;
        for (const MdnsService &service : services) {
            if (service.type.startsWith(QLatin1String(kPairingService)))
                pairing << service;
        }
        if (pairing.isEmpty())
            return;

        MdnsService target;
        bool found = false;
        for (const MdnsService &service : pairing) {
            if (service.instance == m_pairName) {
                target = service;
                found = true;
                break;
            }
        }
        // 有些设备不回显服务名：只有唯一一个配对服务时才敢用
        if (!found && pairing.size() == 1) {
            target = pairing.first();
            found = true;
        }
        if (!found)
            return;

        m_phase = PairPhase::Pairing;
        m_pairIp = target.ip;
        setPairStatus(QStringLiteral("发现手机 %1，正在配对…").arg(target.endpoint()));
        appendLog(QStringLiteral("发现配对服务 %1，开始配对").arg(target.endpoint()));
        m_adb->pair(target.endpoint(), m_pairPassword);
        return;
    }

    if (m_phase == PairPhase::WaitingConnect) {
        if (m_pairIp.isEmpty())
            return;

        // 优先连 _adb-tls-connect 服务；adb 有时会自动连上，所以也看设备列表
        for (const MdnsService &service : services) {
            if (!service.type.startsWith(QLatin1String(kConnectService)))
                continue;
            if (service.ip != m_pairIp)
                continue;
            const QString endpoint = service.endpoint();
            if (m_triedConnects.contains(endpoint))
                continue;
            m_triedConnects.insert(endpoint);
            appendLog(QStringLiteral("发现连接端口 %1，尝试连接").arg(endpoint));
            m_adb->connectTo(endpoint);
        }
    }
}

void MainWindow::stopQrPairing(const QString &reason)
{
    m_pairWatchTimer->stop();
    m_phase = PairPhase::Idle;
    setPairStatus(reason);
    appendLog(reason);
}

void MainWindow::setPairStatus(const QString &text, bool ok)
{
    const QString color = ok ? QStringLiteral("#2e7d32") : QStringLiteral("#666");
    m_adbQrStatus->setStyleSheet(QStringLiteral("color: %1;").arg(color));
    m_adbQrStatus->setText(text);
}

void MainWindow::onPairFinished(bool ok, const QString &message)
{
    if (m_phase == PairPhase::Pairing) {
        if (!ok) {
            stopQrPairing(QStringLiteral("配对失败：%1").arg(message));
            return;
        }
        setPairStatus(QStringLiteral("配对成功，正在等待连接…"), true);
        appendLog(QStringLiteral("二维码配对成功"));
        m_phase = PairPhase::WaitingConnect;
        m_pairDeadlineMs = QDateTime::currentMSecsSinceEpoch() + kConnectWaitMs;
        m_adb->mdnsServices(true);
        m_adb->listDevices(true);
        return;
    }

    // 来自「配对码配对」面板
    m_pairButton->setEnabled(true);
    if (ok) {
        m_pairCode->clear();
        appendLog(QStringLiteral("配对码配对成功"));
        refreshDeviceList();
    } else {
        appendLog(QStringLiteral("配对码配对失败：%1").arg(message));
        QMessageBox::warning(this, QStringLiteral("配对失败"), message);
    }
}

void MainWindow::onConnectFinished(bool ok, const QString &message)
{
    m_connectButton->setEnabled(!selectedAddress().isEmpty());
    appendLog(message);

    if (m_phase == PairPhase::WaitingConnect && ok) {
        setPairStatus(QStringLiteral("已连接：%1").arg(m_pairIp), true);
        m_pairWatchTimer->stop();
        m_phase = PairPhase::Idle;
    }
    if (ok)
        refreshDeviceList();
}

// ---------------------------------------------------------------- 设备列表

void MainWindow::refreshDeviceList()
{
    m_adb->listDevices(true);
    m_adb->mdnsServices(true);
}

void MainWindow::onDevicesListed(const QList<AdbDevice> &devices)
{
    m_lastDevices = devices;
    updateDeviceTable();

    // 二维码配对流程里，设备出现即视为连接成功
    if (m_phase == PairPhase::WaitingConnect) {
        for (const AdbDevice &device : devices) {
            if (device.state == QLatin1String("device") && device.serial.contains(QLatin1Char(':'))) {
                setPairStatus(QStringLiteral("已连接：%1").arg(device.serial), true);
                m_pairWatchTimer->stop();
                m_phase = PairPhase::Idle;
                break;
            }
        }
    }
}

void MainWindow::updateDeviceTable()
{
    const QString keep = selectedAddress();

    struct Row
    {
        QString address;
        QString state;
        QString model;
        bool connected;
    };
    QList<Row> rows;
    QSet<QString> seen;

    for (const AdbDevice &device : m_lastDevices) {
        // adb 会为同一台无线设备同时给出 TCP 条目和 mDNS 传输别名，
        // 别名形如 adb-xxxx._adb-tls-connect._tcp，重复且不可点，直接跳过
        if (device.serial.contains(QStringLiteral("_adb-tls-")))
            continue;
        rows << Row{device.serial, device.state, device.model,
                    device.state == QLatin1String("device")};
        seen.insert(device.serial);
    }

    // 局域网里发现、但还没连接（也没配对）的设备
    for (const MdnsService &service : m_lastServices) {
        if (!service.type.startsWith(QLatin1String(kConnectService)))
            continue;
        const QString endpoint = service.endpoint();
        if (seen.contains(endpoint))
            continue;
        seen.insert(endpoint);
        rows << Row{endpoint, QStringLiteral("可连接"), QString(), false};
    }

    std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
        if (a.connected != b.connected)
            return a.connected;
        return a.address < b.address;
    });

    m_deviceTable->setRowCount(rows.size());
    for (int row = 0; row < rows.size(); ++row) {
        const Row &item = rows.at(row);
        auto *addressItem = new QTableWidgetItem(item.address);
        auto *stateItem = new QTableWidgetItem(item.state);
        auto *modelItem = new QTableWidgetItem(item.model);

        if (item.connected)
            stateItem->setForeground(QColor(QStringLiteral("#2e7d32")));
        else if (item.state == QStringLiteral("可连接"))
            stateItem->setForeground(QColor(QStringLiteral("#1565c0")));
        else
            stateItem->setForeground(QColor(QStringLiteral("#c62828")));

        m_deviceTable->setItem(row, 0, addressItem);
        m_deviceTable->setItem(row, 1, stateItem);
        m_deviceTable->setItem(row, 2, modelItem);

        if (!keep.isEmpty() && item.address == keep)
            m_deviceTable->selectRow(row);
    }

    m_connectButton->setEnabled(!selectedAddress().isEmpty());
}

QString MainWindow::selectedAddress() const
{
    const int row = m_deviceTable ? m_deviceTable->currentRow() : -1;
    if (row < 0 || !m_deviceTable->item(row, 0))
        return QString();
    return m_deviceTable->item(row, 0)->text();
}

void MainWindow::connectSelectedDevice()
{
    const QString address = selectedAddress();
    if (address.isEmpty())
        return;
    m_connectButton->setEnabled(false);
    m_adb->connectTo(address);
}

void MainWindow::disconnectSelectedDevice()
{
    const QString address = selectedAddress();
    if (address.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("提示"),
                                 QStringLiteral("先在设备列表里选中一台设备"));
        return;
    }
    m_adb->disconnectDevice(address);
}

// ------------------------------------------------------------ 配对码配对

void MainWindow::pairWithCode()
{
    const QString endpoint = m_pairEndpoint->text().trimmed();
    const QString code = m_pairCode->text().trimmed();
    if (endpoint.isEmpty() || code.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("提示"),
                                 QStringLiteral("请填写手机无线调试页面上显示的 IP:端口 和配对码"));
        return;
    }
    if (m_phase != PairPhase::Idle) {
        // 二维码配对进行中，配对结果不应被误当成二维码流程的结果
        m_phase = PairPhase::Idle;
        m_pairWatchTimer->stop();
        setPairStatus(QStringLiteral("已切换到配对码配对"));
    }
    m_pairButton->setEnabled(false);
    m_adb->pair(endpoint, code);
}

void MainWindow::appendLog(const QString &text)
{
    m_logView->appendPlainText(text);
}