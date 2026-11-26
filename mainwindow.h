#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QHash>
#include <QSet>
#include <QStringList>

#include "adbmanager.h"
#include "hdcmanager.h"
#include "iosmanager.h"

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QTabWidget;
class QTimer;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    void refreshWifi();
    void regeneratePairQr();
    void pairWithCode();
    void connectSelectedDevice();
    void disconnectSelectedDevice();
    void refreshDeviceList();

    void onPairFinished(bool ok, const QString &message);
    void onConnectFinished(bool ok, const QString &message);
    void onDevicesListed(const QList<AdbDevice> &devices);
    void onMdnsListed(const QList<MdnsService> &services);
    void onPairWatchTick();
    void appendLog(const QString &text);

    // 鸿蒙 / hdc
    void onHdcTargetsListed(const QList<HdcDevice> &devices);
    void onHdcDiscovered(const QList<HdcDiscovered> &devices);
    void onHdcKnownEndpoints(const QList<HdcDiscovered> &devices);
    void onHdcConnectFinished(bool ok, const QString &message, const QString &key, bool quiet);
    void onHdcServerRestarted(const QString &detail);
    void onHdcLanProbeFinished(int probed, int found);
    void chooseHdcPath();
    void autoReconnectKnownHdc();
    // 设备列表右键菜单：修改端口 / 重连 / 断开
    void showDeviceContextMenu(const QPoint &pos);

private:
    QWidget *buildLeftPanel();
    QWidget *buildQrPairTab();
    QWidget *buildWifiTab();
    QWidget *buildRightPanel();
    QWidget *buildHdcGroup();

    void renderQr(QLabel *label, const QString &text);
    void applyWifiProfile(bool ok, const QString &ssid, const QString &password,
                          const QString &authType);
    void updateDeviceTable();
    QString selectedAddress() const;
    QString selectedSource() const;
    void connectHdcManually();
    void updateHdcPathLabel();
    // 弹窗修改选中设备的端口，成功后按新地址重连
    void editSelectedDevicePort();

    void startQrPairing();
    void stopQrPairing(const QString &reason);
    void handleQrPairing(const QList<MdnsService> &services);
    void setPairStatus(const QString &text, bool ok = false);

    // 左侧：二维码
    QTabWidget *m_tabs = nullptr;
    QLabel *m_adbQrLabel = nullptr;      // 配对二维码
    QLabel *m_adbQrStatus = nullptr;     // 扫码/配对状态
    QLabel *m_wifiQrLabel = nullptr;     // WiFi 二维码
    QLabel *m_ssidLabel = nullptr;
    QLabel *m_passwordTip = nullptr;

    // 右侧：设备列表
    QTableWidget *m_deviceTable = nullptr;
    QPushButton *m_connectButton = nullptr;
    QPushButton *m_disconnectButton = nullptr;

    // 右侧：配对码配对
    QLineEdit *m_pairEndpoint = nullptr;
    QLineEdit *m_pairCode = nullptr;
    QPushButton *m_pairButton = nullptr;

    // 右侧：鸿蒙（hdc）
    QLabel *m_hdcPathLabel = nullptr;
    QLineEdit *m_hdcEndpoint = nullptr;
    QPushButton *m_hdcConnectButton = nullptr;

    // 右侧：日志
    QPlainTextEdit *m_logView = nullptr;

    AdbManager *m_adb = nullptr;
    HdcManager *m_hdc = nullptr;
    IosManager *m_ios = nullptr;
    QTimer *m_pollTimer = nullptr;
    QTimer *m_pairWatchTimer = nullptr;

    QList<AdbDevice> m_lastDevices;
    QList<MdnsService> m_lastServices;
    QList<HdcDevice> m_hdcDevices;
    QList<HdcDiscovered> m_hdcDiscovered;   // UDP 单播复扫到的
    QList<HdcDiscovered> m_hdcKnownOnline;  // 记住的地址里 TCP 还连得上的
    QHash<QString, QString> m_hdcDeviceNames; // connectKey -> 设备名
    QHash<QString, QString> m_adbDeviceNames; // serial -> 设备名
    QList<IosDevice> m_iosDevices;

    // 二维码配对状态机
    enum class PairPhase { Idle, WaitingScan, Pairing, WaitingConnect };
    PairPhase m_phase = PairPhase::Idle;
    QString m_pairName;
    QString m_pairPassword;
    QString m_pairIp;
    qint64 m_pairDeadlineMs = 0;
    QSet<QString> m_triedConnects;
    int m_scanTick = 0; // 用来降低 hdc discover（耗时约 2 秒）的调用频率

    QString m_ssid;
    QString m_password;
    QString m_authType;
    bool m_wifiLoading = false;

    // 自动重连记住的地址时别刷日志
    QSet<QString> m_autoReconnectTried;
    // 上次探测结束时的结果数，用来决定是否写日志
    int m_lastLanProbeFound = -1;
    // 记住的地址全部重连失败时提示一次“端口可能变了”
    int m_knownReconnectFailures = 0;
    bool m_portHintShown = false;
};

#endif // MAINWINDOW_H