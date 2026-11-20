#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QSet>
#include <QStringList>

#include "adbmanager.h"

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

private:
    QWidget *buildLeftPanel();
    QWidget *buildQrPairTab();
    QWidget *buildWifiTab();
    QWidget *buildRightPanel();

    void renderQr(QLabel *label, const QString &text);
    void updateDeviceTable();
    QString selectedAddress() const;

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

    // 右侧：日志
    QPlainTextEdit *m_logView = nullptr;

    AdbManager *m_adb = nullptr;
    QTimer *m_pollTimer = nullptr;
    QTimer *m_pairWatchTimer = nullptr;

    QList<AdbDevice> m_lastDevices;
    QList<MdnsService> m_lastServices;

    // 二维码配对状态机
    enum class PairPhase { Idle, WaitingScan, Pairing, WaitingConnect };
    PairPhase m_phase = PairPhase::Idle;
    QString m_pairName;
    QString m_pairPassword;
    QString m_pairIp;
    qint64 m_pairDeadlineMs = 0;
    QSet<QString> m_triedConnects;

    QString m_ssid;
    QString m_password;
    QString m_authType;
};

#endif // MAINWINDOW_H