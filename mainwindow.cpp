#include "mainwindow.h"

#include "wifiinfo.h"

#include <QDateTime>
#include <QClipboard>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QInputDialog>
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
    , m_hdc(new HdcManager(this))
    , m_ios(new IosManager(this))
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
    connect(m_adb, &AdbManager::deviceNameResolved, this,
            [this](const QString &serial, const QString &name) {
        m_adbDeviceNames.insert(serial, name);
        updateDeviceTable();
    });

    connect(m_hdc, &HdcManager::logMessage, this, &MainWindow::appendLog);
    connect(m_hdc, &HdcManager::targetsListed, this, &MainWindow::onHdcTargetsListed);
    connect(m_hdc, &HdcManager::discoveredListed, this, &MainWindow::onHdcDiscovered);
    connect(m_hdc, &HdcManager::knownEndpointsListed,
            this, &MainWindow::onHdcKnownEndpoints);
    connect(m_hdc, &HdcManager::connectFinished, this, &MainWindow::onHdcConnectFinished);
    connect(m_hdc, &HdcManager::serverRestarted, this, &MainWindow::onHdcServerRestarted);
    connect(m_hdc, &HdcManager::lanProbeFinished,
            this, &MainWindow::onHdcLanProbeFinished);
    connect(m_ios, &IosManager::devicesListed, this,
            [this](const QList<IosDevice> &devices) {
        m_iosDevices = devices;
        updateDeviceTable();
    });
    connect(m_ios, &IosManager::logMessage, this, &MainWindow::appendLog);
    connect(m_hdc, &HdcManager::deviceNameResolved, this,
            [this](const QString &key, const QString &name) {
        m_hdcDeviceNames.insert(key, name);
        updateDeviceTable();
    });
    connect(m_hdc, &HdcManager::versionChecked, this,
            [this](bool ok, const QString &detail) {
        updateHdcPathLabel();
        if (ok)
            appendLog(QStringLiteral("hdc: %1").arg(detail));
        else
            appendLog(QStringLiteral("hdc 不可用：%1").arg(detail));
    });

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

    updateHdcPathLabel();
    if (!m_hdc->isAvailable())
        appendLog(QStringLiteral("未找到 hdc。鸿蒙设备支持需要 DevEco Studio 的 hdc.exe，"
                                 "点「选择 hdc.exe」指定路径。"));
    else
        m_hdc->checkVersion();

    refreshDeviceList();
    if (m_hdc->isAvailable())
        QTimer::singleShot(600, this, [this]() { m_hdc->checkKnownEndpoints(); });
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

    m_deviceTable = new QTableWidget(0, 5);
    m_deviceTable->setHorizontalHeaderLabels(
        {QStringLiteral("地址"), QStringLiteral("状态"), QStringLiteral("设备名称"),
         QStringLiteral("连接方式"), QStringLiteral("来源")});
    m_deviceTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_deviceTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_deviceTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_deviceTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_deviceTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_deviceTable->verticalHeader()->setVisible(false);
    m_deviceTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_deviceTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_deviceTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_deviceTable->setMinimumHeight(140);
    m_deviceTable->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_deviceTable, &QTableWidget::itemSelectionChanged,
            this, [this]() { m_connectButton->setEnabled(!selectedAddress().isEmpty()); });
    connect(m_deviceTable, &QTableWidget::cellDoubleClicked,
            this, [this](int, int) { connectSelectedDevice(); });
    connect(m_deviceTable, &QWidget::customContextMenuRequested,
            this, &MainWindow::showDeviceContextMenu);

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
    layout->addWidget(buildHdcGroup());
    layout->addWidget(m_logView, 1);
    return panel;
}

QWidget *MainWindow::buildHdcGroup()
{
    auto *group = new QGroupBox(QStringLiteral("鸿蒙无线调试（hdc）"));
    auto *layout = new QVBoxLayout(group);

    auto *hint = new QLabel(
        QStringLiteral("鸿蒙没有安卓那种二维码/配对码。手机上：设置 → 系统 → 开发者选项 → "
                       "无线调试，把显示的 IP:端口 填到下面点「连接」。"
                       "连过一次的地址会被记住，下次启动自动重连。"
                       "每次重开无线调试端口会变，此时在设备列表里右键那一行，"
                       "可以改端口号后直接重连。"));
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color: #666;"));

    auto *form = new QHBoxLayout;
    m_hdcEndpoint = new QLineEdit;
    m_hdcEndpoint->setPlaceholderText(QStringLiteral("192.168.3.24:37991"));
    m_hdcConnectButton = new QPushButton(QStringLiteral("连接"));
    connect(m_hdcConnectButton, &QPushButton::clicked, this, &MainWindow::connectHdcManually);
    connect(m_hdcEndpoint, &QLineEdit::returnPressed, this, &MainWindow::connectHdcManually);
    form->addWidget(new QLabel(QStringLiteral("地址")));
    form->addWidget(m_hdcEndpoint, 1);
    form->addWidget(m_hdcConnectButton);

    auto *pathRow = new QHBoxLayout;
    m_hdcPathLabel = new QLabel;
    m_hdcPathLabel->setStyleSheet(QStringLiteral("color: #666;"));
    m_hdcPathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *browse = new QPushButton(QStringLiteral("选择 hdc.exe"));
    connect(browse, &QPushButton::clicked, this, &MainWindow::chooseHdcPath);
    pathRow->addWidget(m_hdcPathLabel, 1);
    pathRow->addWidget(browse);

    layout->addWidget(hint);
    layout->addLayout(form);
    layout->addLayout(pathRow);
    return group;
}

void MainWindow::updateHdcPathLabel()
{
    if (!m_hdcPathLabel)
        return;
    if (m_hdc->isAvailable())
        m_hdcPathLabel->setText(QStringLiteral("hdc: %1").arg(m_hdc->hdcPath()));
    else
        m_hdcPathLabel->setText(QStringLiteral("未找到 hdc.exe，请点右侧按钮指定"));
}

void MainWindow::chooseHdcPath()
{
    const QString start = m_hdc->isAvailable()
        ? QFileInfo(m_hdc->hdcPath()).absolutePath()
        : QStringLiteral("C:/Users/%1/Downloads").arg(qEnvironmentVariable("USERNAME"));
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择 hdc.exe"), start,
        QStringLiteral("hdc (hdc.exe);;可执行文件 (*.exe);;所有文件 (*)"));
    if (path.isEmpty())
        return;

    m_hdc->setHdcPath(path);
    updateHdcPathLabel();
    appendLog(QStringLiteral("使用 hdc: %1").arg(path));
    m_hdc->checkVersion();
    m_hdc->listTargets(true);
    m_hdc->probeLan();
    m_hdc->checkKnownEndpoints();
}

void MainWindow::autoReconnectKnownHdc()
{
    const QStringList known = m_hdc->knownEndpoints();
    if (known.isEmpty())
        return;

    QSet<QString> online;
    for (const HdcDevice &device : m_hdcDevices) {
        const bool isTcp = device.type.compare(QStringLiteral("TCP"), Qt::CaseInsensitive) == 0
                           || device.connectKey.contains(QLatin1Char(':'));
        const bool connected =
            device.state.compare(QStringLiteral("Connected"), Qt::CaseInsensitive) == 0
            || device.state.compare(QStringLiteral("Ready"), Qt::CaseInsensitive) == 0;
        // hdc 里 Offline 只表示“记得这个地址”，不算在线，仍然尝试重连
        if (isTcp && connected)
            online.insert(device.connectKey);
    }

    for (const QString &key : known) {
        if (online.contains(key) || m_autoReconnectTried.contains(key))
            continue;
        m_autoReconnectTried.insert(key);
        // 静默重连：失败不刷日志 —— 这个地址可能只是暂时不在线
        m_hdc->connectTo(key, /*quiet=*/true);
    }
}

void MainWindow::connectHdcManually()
{
    const QString endpoint = m_hdcEndpoint->text().trimmed();
    if (endpoint.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("提示"),
                                 QStringLiteral("请填写手机无线调试页显示的 IP:端口"));
        return;
    }
    if (!endpoint.contains(QLatin1Char(':'))) {
        QMessageBox::warning(this, QStringLiteral("地址格式"),
                             QStringLiteral("地址要写成 IP:端口，例如 192.168.3.24:37991\n"
                                            "端口请用手机无线调试页上显示的那个。"));
        return;
    }
    m_hdcConnectButton->setEnabled(false);
    m_hdc->connectTo(endpoint);
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

// ---------------------------------------------------------------- 鸿蒙 hdc

void MainWindow::onHdcTargetsListed(const QList<HdcDevice> &devices)
{
    m_hdcDevices = devices;
    updateDeviceTable();
}

void MainWindow::onHdcDiscovered(const QList<HdcDiscovered> &devices)
{
    m_hdcDiscovered = devices;
    updateDeviceTable();
}

void MainWindow::onHdcKnownEndpoints(const QList<HdcDiscovered> &devices)
{
    const bool appeared = devices.size() > m_hdcKnownOnline.size();
    m_hdcKnownOnline = devices;

    // 掉线的地址允许下次重新尝试自动重连
    QSet<QString> online;
    for (const HdcDiscovered &device : devices)
        online.insert(device.endpoint());
    for (auto it = m_autoReconnectTried.begin(); it != m_autoReconnectTried.end();) {
        if (online.contains(*it))
            ++it;
        else
            it = m_autoReconnectTried.erase(it);
    }

    updateDeviceTable();
    // 记住的地址重新上线了就自动连，不然用户得自己再点一次
    if (appeared)
        autoReconnectKnownHdc();
}

void MainWindow::onHdcLanProbeFinished(int probed, int found)
{
    if (found != m_lastLanProbeFound) {
        m_lastLanProbeFound = found;
        if (found > 0) {
            appendLog(QStringLiteral("局域网发现：探测 %1 个地址，发现 %2 台鸿蒙设备")
                          .arg(probed)
                          .arg(found));
        }
    }
}

void MainWindow::onHdcConnectFinished(bool ok, const QString &message, const QString &key,
                                      bool quiet)
{
    if (m_hdcConnectButton)
        m_hdcConnectButton->setEnabled(true);
    if (!quiet || ok)
        appendLog(message);
    if (ok) {
        if (m_hdcEndpoint)
            m_hdcEndpoint->clear();
        m_autoReconnectTried.insert(key);
        m_knownReconnectFailures = 0;
        m_portHintShown = false;
        m_hdc->listTargets(true);
        return;
    }

    // 静默重连失败：全部记住的地址都连不上时提示一次，多半是无线调试重开过、端口变了
    if (quiet) {
        ++m_knownReconnectFailures;
        const int known = m_hdc->knownEndpoints().size();
        if (!m_portHintShown && known > 0 && m_knownReconnectFailures >= known) {
            m_portHintShown = true;
            appendLog(QStringLiteral("上次连接的鸿蒙设备现在连不上了。鸿蒙每次重新打开"
                                     "「无线调试」端口都会变，请在手机无线调试页确认新的 "
                                     "IP:端口，填到「鸿蒙无线调试」面板里点「连接」。"));
        }
    }
}

void MainWindow::onHdcServerRestarted(const QString &detail)
{
    appendLog(detail);
    m_hdcDevices.clear();
    m_hdcDiscovered.clear();
    updateDeviceTable();
    m_hdc->probeLan();
}

// ---------------------------------------------------------------- 设备列表

void MainWindow::refreshDeviceList()
{
    if (m_adb->isAvailable()) {
        m_adb->listDevices(true);
        m_adb->mdnsServices(true);
    }
    if (m_hdc->isAvailable()) {
        m_hdc->listTargets(true);
        // UDP 单播扫描一轮约 1.5 秒：已经有设备连着时降频，空闲时勤一点
        const bool hasConnected = std::any_of(
            m_hdcDevices.cbegin(), m_hdcDevices.cend(), [](const HdcDevice &device) {
                const bool isTcp =
                    device.type.compare(QStringLiteral("TCP"), Qt::CaseInsensitive) == 0;
                return isTcp
                       && (device.state.compare(QStringLiteral("Connected"),
                                                Qt::CaseInsensitive) == 0
                           || device.state.compare(QStringLiteral("Ready"),
                                                   Qt::CaseInsensitive) == 0);
            });
        const int scanEveryTicks = hasConnected ? 30 : 4; // 2 分钟 / 16 秒
        if (m_scanTick++ % scanEveryTicks == 0) {
            m_hdc->probeLan();
            m_hdc->checkKnownEndpoints();
        }
    }
    // iOS 走本机 usbmuxd，查询很快
    m_ios->listDevices();
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
        QString name;       // 设备名称 / 型号
        QString connection; // TCP / USB / UART
        QString source;     // adb / hdc
        bool connected;
    };
    QList<Row> rows;
    QSet<QString> seen;
    const auto keyOf = [](const QString &source, const QString &address) {
        return source + QLatin1Char('|') + address;
    };

    // ---- adb：已连接的设备 ----
    for (const AdbDevice &device : m_lastDevices) {
        // adb 会为同一台无线设备同时给出 TCP 条目和 mDNS 传输别名，
        // 别名形如 adb-xxxx._adb-tls-connect._tcp，重复且不可点，直接跳过
        if (device.serial.contains(QStringLiteral("_adb-tls-")))
            continue;
        // 带冒号是 TCP 无线连接，否则是 USB
        const QString conn = device.serial.contains(QLatin1Char(':'))
                                 ? QStringLiteral("TCP")
                                 : QStringLiteral("USB");
        // 优先用查到的“厂商 型号”（如 OPPO PKB110），还没查到时先显示 adb 给的型号
        QString name = m_adbDeviceNames.value(device.serial);
        if (name.isEmpty()) {
            if (device.state == QLatin1String("device"))
                m_adb->requestDeviceName(device.serial);
            name = device.model;
        }
        rows << Row{device.serial, device.state, name, conn,
                    QStringLiteral("adb"), device.state == QLatin1String("device")};
        seen.insert(keyOf(QStringLiteral("adb"), device.serial));
    }

    // ---- adb：局域网里发现、还没连接的设备 ----
    for (const MdnsService &service : m_lastServices) {
        if (!service.type.startsWith(QLatin1String(kConnectService)))
            continue;
        const QString endpoint = service.endpoint();
        if (seen.contains(keyOf(QStringLiteral("adb"), endpoint)))
            continue;
        seen.insert(keyOf(QStringLiteral("adb"), endpoint));
        rows << Row{endpoint, QStringLiteral("可连接"), QString(),
                    QStringLiteral("TCP"), QStringLiteral("adb"), false};
    }

    // ---- hdc：已连接的鸿蒙设备 ----
    // hdc 服务端会把连过的地址都留在内存里，端口变了之后旧的那条会标成 Offline。
    // 同一个 IP 只保留在线的那条，旧端口的死条目不再显示（否则同一台手机出现两行）。
    QSet<QString> hdcOnlineIps;
    for (const HdcDevice &device : m_hdcDevices) {
        const bool online =
            device.state.compare(QStringLiteral("Connected"), Qt::CaseInsensitive) == 0
            || device.state.compare(QStringLiteral("Ready"), Qt::CaseInsensitive) == 0;
        if (!online)
            continue;
        const int colon = device.connectKey.lastIndexOf(QLatin1Char(':'));
        hdcOnlineIps.insert(colon > 0 ? device.connectKey.left(colon) : device.connectKey);
    }

    for (const HdcDevice &device : m_hdcDevices) {
        const bool online =
            device.state.compare(QStringLiteral("Connected"), Qt::CaseInsensitive) == 0
            || device.state.compare(QStringLiteral("Ready"), Qt::CaseInsensitive) == 0;

        const int colon = device.connectKey.lastIndexOf(QLatin1Char(':'));
        const bool isTcp = colon > 0
                           && !device.connectKey.startsWith(QStringLiteral("COM"),
                                                            Qt::CaseInsensitive);
        // 这个 IP 已经有在线端口了，旧端口的离线条目直接跳过
        if (!online && isTcp && hdcOnlineIps.contains(device.connectKey.left(colon)))
            continue;

        // 有设备名就显示设备名（HUAWEI Mate 60 Pro），没有就退回类型（TCP / USB）
        QString name = m_hdcDeviceNames.value(device.connectKey);
        if (name.isEmpty() && isTcp) {
            // 同一 IP 换过端口的旧条目也尽量显示已知名称
            const QString prefix = device.connectKey.left(colon + 1);
            for (auto it = m_hdcDeviceNames.cbegin(); it != m_hdcDeviceNames.cend(); ++it) {
                if (it.key().startsWith(prefix)) {
                    name = it.value();
                    break;
                }
            }
        }
        rows << Row{device.connectKey, device.state, name, device.type,
                    QStringLiteral("hdc"), online};
        seen.insert(keyOf(QStringLiteral("hdc"), device.connectKey));
        // 已连接的设备去问一次名称，查到后会刷新列表
        if (online && name.isEmpty() && isTcp)
            m_hdc->requestDeviceName(device.connectKey);
    }

    // ---- hdc：局域网广播发现、还没连接的鸿蒙设备 ----
    for (const HdcDiscovered &device : m_hdcDiscovered) {
        const QString endpoint = device.endpoint();
        if (seen.contains(keyOf(QStringLiteral("hdc"), endpoint)))
            continue;
        seen.insert(keyOf(QStringLiteral("hdc"), endpoint));
        rows << Row{endpoint, QStringLiteral("可连接（鸿蒙）"), QString(),
                    QStringLiteral("TCP"), QStringLiteral("hdc"), false};
    }

    // ---- hdc：连过、但当前没在列表里的地址（TCP 还连得上） ----
    for (const HdcDiscovered &device : m_hdcKnownOnline) {
        const QString endpoint = device.endpoint();
        if (seen.contains(keyOf(QStringLiteral("hdc"), endpoint)))
            continue;
        seen.insert(keyOf(QStringLiteral("hdc"), endpoint));
        rows << Row{endpoint, QStringLiteral("可连接（上次连接过）"),
                    m_hdcDeviceNames.value(endpoint), QStringLiteral("TCP"),
                    QStringLiteral("hdc"), false};
    }

    // ---- iOS：通过 usbmuxd 找到的设备 ----
    for (const IosDevice &device : m_iosDevices) {
        // 有设备名就显示名称，否则先用型号标识
        QString name = device.name;
        if (name.isEmpty())
            name = device.productType;
        if (name.isEmpty())
            name = device.udid;

        QString state;
        if (!device.paired)
            state = QStringLiteral("未信任"); // 手机没点“信任此电脑”，读不到名称
        else if (!device.version.isEmpty())
            state = QStringLiteral("已连接 · iOS %1").arg(device.version);
        else
            state = QStringLiteral("已连接");

        rows << Row{device.udid, state, name, device.connection,
                    QStringLiteral("ios"), true};
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
        auto *nameItem = new QTableWidgetItem(item.name);
        auto *connItem = new QTableWidgetItem(item.connection);
        auto *sourceItem = new QTableWidgetItem(item.source);

        if (item.connected)
            stateItem->setForeground(QColor(QStringLiteral("#2e7d32")));
        else if (item.state.startsWith(QStringLiteral("可连接")))
            stateItem->setForeground(QColor(QStringLiteral("#1565c0")));
        else
            stateItem->setForeground(QColor(QStringLiteral("#c62828")));

        // 连接方式用浅色，突出设备名
        connItem->setForeground(QColor(QStringLiteral("#666")));
        sourceItem->setForeground(QColor(QStringLiteral("#666")));

        m_deviceTable->setItem(row, 0, addressItem);
        m_deviceTable->setItem(row, 1, stateItem);
        m_deviceTable->setItem(row, 2, nameItem);
        m_deviceTable->setItem(row, 3, connItem);
        m_deviceTable->setItem(row, 4, sourceItem);

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

QString MainWindow::selectedSource() const
{
    const int row = m_deviceTable ? m_deviceTable->currentRow() : -1;
    if (row < 0 || !m_deviceTable->item(row, 4))
        return QString();
    return m_deviceTable->item(row, 4)->text();
}

void MainWindow::connectSelectedDevice()
{
    const QString address = selectedAddress();
    if (address.isEmpty())
        return;
    const QString source = selectedSource();
    // iOS 设备不需要 adb/hdc 那种“连接”动作，插上就是连好的
    if (source == QLatin1String("ios")) {
        QMessageBox::information(
            this, QStringLiteral("iOS 设备"),
            QStringLiteral("iOS 设备通过 USB/网络直接可见，不需要手动连接。\n\n"
                           "当前只做设备识别，不提供调试功能。"));
        return;
    }
    m_connectButton->setEnabled(false);
    if (source == QLatin1String("hdc"))
        m_hdc->connectTo(address);
    else
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
    if (selectedSource() == QLatin1String("ios")) {
        QMessageBox::information(this, QStringLiteral("iOS 设备"),
                                 QStringLiteral("iOS 设备请在手机端断开，或在系统里拔掉数据线。"));
        return;
    }
    if (selectedSource() == QLatin1String("hdc")) {
        // 部分 hdc 版本的 -remove 不生效，用 kill 重启 server 真正断开所有 TCP 连接
        const auto answer = QMessageBox::question(
            this, QStringLiteral("断开鸿蒙设备"),
            QStringLiteral("将重启 hdc 服务进程（hdc kill -r）以断开 %1。\n"
                           "这会断开所有 hdc 连接，USB 设备会在重连后自动恢复。")
                .arg(address),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (answer == QMessageBox::Yes) {
            // 同时忘掉这个地址，否则重启后会自动重连回去
            m_hdc->forgetEndpoint(address);
            m_autoReconnectTried.remove(address);
            m_hdc->restartServer();
        }
        return;
    }
    m_adb->disconnectDevice(address);
}

void MainWindow::showDeviceContextMenu(const QPoint &pos)
{
    QTableWidgetItem *item = m_deviceTable->itemAt(pos);
    if (!item)
        return;
    // 右键顺便把该行选中，后续动作都基于选中行
    m_deviceTable->selectRow(item->row());

    const QString address = selectedAddress();
    const QString source = selectedSource();
    const bool isHdc = source == QLatin1String("hdc");

    QMenu menu(this);
    QAction *editPort = nullptr;
    // 只有 hdc 的 IP:端口 条目才能改端口（USB 序列号、COM 串口不行）
    if (isHdc && address.contains(QLatin1Char(':'))
        && !address.startsWith(QStringLiteral("COM"), Qt::CaseInsensitive)) {
        editPort = menu.addAction(QStringLiteral("修改端口…"));
    }
    QAction *reconnect = menu.addAction(QStringLiteral("重新连接"));
    QAction *copyAddr = menu.addAction(QStringLiteral("复制地址"));

    QAction *chosen = menu.exec(m_deviceTable->viewport()->mapToGlobal(pos));
    if (!chosen)
        return;

    if (chosen == editPort) {
        editSelectedDevicePort();
    } else if (chosen == reconnect) {
        connectSelectedDevice();
    } else if (chosen == copyAddr) {
        QGuiApplication::clipboard()->setText(address);
        appendLog(QStringLiteral("已复制地址：%1").arg(address));
    }
}

void MainWindow::editSelectedDevicePort()
{
    const QString address = selectedAddress();
    const int colon = address.lastIndexOf(QLatin1Char(':'));
    if (colon <= 0) {
        QMessageBox::information(this, QStringLiteral("提示"),
                                 QStringLiteral("这个条目不是 IP:端口 形式，无法修改端口。"));
        return;
    }
    const QString ip = address.left(colon);
    const QString oldPort = address.mid(colon + 1);

    bool ok = false;
    const QString portText = QInputDialog::getText(
        this, QStringLiteral("修改端口"),
        QStringLiteral("手机「无线调试」页上显示的端口：\n%1").arg(ip),
        QLineEdit::Normal, oldPort, &ok);
    if (!ok)
        return;

    const QString trimmed = portText.trimmed();
    bool portOk = false;
    const int port = trimmed.toInt(&portOk);
    if (!portOk || port <= 0 || port > 65535) {
        QMessageBox::warning(this, QStringLiteral("端口无效"),
                             QStringLiteral("端口要是 1-65535 之间的数字。"));
        return;
    }

    const QString endpoint = QStringLiteral("%1:%2").arg(ip).arg(port);
    if (endpoint == address) {
        connectSelectedDevice();
        return;
    }

    appendLog(QStringLiteral("修改端口：%1 → %2").arg(address, endpoint));
    // 旧端口作废，换上新地址后直接连
    m_hdc->forgetEndpoint(address);
    m_autoReconnectTried.remove(address);
    m_hdc->connectTo(endpoint);
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