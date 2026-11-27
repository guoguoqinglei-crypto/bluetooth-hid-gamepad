#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QWidget>
#include <QByteArray>
#include <QList>
#include <QVector>
#include <QTimer>
#include <QElapsedTimer>
#include <QColor>
#include <QStringList>
#include "hiddevice.h"
#include "protocol.h"

class QLabel; class QPushButton; class QComboBox; class QSlider; class QSpinBox;
class QCheckBox; class QPlainTextEdit; class QGroupBox;
class PerformanceMonitor;

/* 一块自绘的摇杆面板：方形底盘 + 十字线 + 当前位置的小圆点。
 * 原始值是 0~65535 的 ADC 直采，中位 32768，这里直接按比例画。 */
class StickPad : public QWidget
{
public:
    explicit StickPad(const QString &title, QWidget *parent = nullptr);
    void setStick(quint16 x, quint16 y);
    void reset();
protected:
    void paintEvent(QPaintEvent *event) override;
private:
    QString m_title;
    int m_x = 0;          /* -100 ~ +100 */
    int m_y = 0;
    quint16 m_rawX = 0x8000;
    quint16 m_rawY = 0x8000;
};

/* 一个按键指示灯：上面画灯，下面写键名，按下时亮起并计数 */
class KeyIndicator : public QWidget
{
public:
    KeyIndicator(const QString &name, const QString &func,
                 const QColor &color, QWidget *parent = nullptr);
    void setDown(bool down);
    void setPressCount(int n);
    void resetCount();
    bool isDown() const { return m_down; }
    int pressCount() const { return m_count; }
protected:
    void paintEvent(QPaintEvent *event) override;
private:
    QString m_name;
    QString m_func;
    QColor  m_color;
    bool    m_down = false;
    int     m_count = 0;
};

/* RGB 预览色块 */
class ColorSwatch : public QWidget
{
public:
    explicit ColorSwatch(QWidget *parent = nullptr);
    void setColor(int r, int g, int b);
    QColor color() const { return m_color; }
protected:
    void paintEvent(QPaintEvent *event) override;
private:
    QColor m_color = QColor(0, 0, 0);
    int    m_pct[3] = { 0, 0, 0 };
};

namespace Dashboard { class MetricCard; class AssessmentBar; }

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;
private slots:
    void onRefreshDevices();
    void onConnectClicked();
    void onReport(const QByteArray &raw);
    void onDeviceLost(const QString &reason);
    void onLog(const QString &text);
    void onSendRgb();
    void onPresetClicked();
    void onSliderChanged(int value);
    void onRainbowTick();
    void onAutoReconnectTick();
    void onClearLog();
    void onSaveLog();
    void onClearCounters();
    void onShowAssessment();
    void onExportReport();
    void onStartRgbTest();
    void onRgbTestTick();
    /* 三个节流定时器：高频操作合并到低频执行 */
    void onSendTick();
    void onUiTick();
    void onStatTick();
    void onFlushLog();
    void onConfigRead();
    void onConfigWrite();
    void onConfigDefault();
    void onConfigPoll();
    void onFeatureResult(bool success, bool write, const QByteArray &data, const QString &error);
private:
    void buildUi();
    void setConnected(bool on);
    void populateDeviceCombo();
    void appendLog(const QString &tag, const QString &text, const QColor &color = QColor());
    void updateStats();
    void updateAssessment();
    QString assessmentReport() const;
    void refreshUi();
    void applyColor(int r, int g, int b, bool send);
    void queueRgb(int r, int g, int b);
    void sendRgbNow(int r, int g, int b);
    void loadSettings();
    void saveSettings();

    HidDevice m_hid;
    PerformanceMonitor *m_performance = nullptr;
    QList<HidDeviceInfo> m_devices;

    QComboBox *m_deviceCombo = nullptr;
    QPushButton *m_refreshBtn = nullptr;
    QPushButton *m_connectBtn = nullptr;
    QLabel *m_connLabel = nullptr;
    QCheckBox *m_autoReconnect = nullptr;
    QCheckBox *m_liveSend = nullptr;
    QCheckBox *m_rainbow = nullptr;
    QCheckBox *m_pauseLog = nullptr;
    QCheckBox *m_logEveryFrame = nullptr;
    ColorSwatch *m_colorPreview = nullptr;
    QLabel *m_heartbeatLabel = nullptr;
    QPlainTextEdit *m_logView = nullptr;
    QGroupBox *m_rgbBox = nullptr;
    Dashboard::MetricCard *m_framesCard = nullptr;
    Dashboard::MetricCard *m_rateCard = nullptr;
    Dashboard::MetricCard *m_keysCard = nullptr;
    Dashboard::MetricCard *m_rgbCard = nullptr;
    Dashboard::AssessmentBar *m_assessmentBar = nullptr;

    StickPad *m_padL = nullptr;
    StickPad *m_padR = nullptr;
    QLabel *m_padLValue = nullptr;
    QLabel *m_padRValue = nullptr;

    QVector<KeyIndicator *> m_keys;
    QVector<QSlider *> m_sliders;
    QVector<QSpinBox *> m_spinBoxes;
    QLabel *m_btnLabel = nullptr;
    QLabel *m_frameLabel = nullptr;
    QLabel *m_rateLabel = nullptr;
    QLabel *m_ageLabel = nullptr;
    QLabel *m_rawLabel = nullptr;
    QLabel *m_eventLabel = nullptr;
    QLabel *m_statusLabel = nullptr;
    QLabel *m_durationLabel = nullptr;
    QLabel *m_outLabel = nullptr;
    QLabel *m_pathLabel = nullptr;
    QGroupBox *m_configBox = nullptr;
    QSpinBox *m_sens[2] = { nullptr, nullptr };
    QSpinBox *m_deadzone[2] = { nullptr, nullptr };
    QLabel *m_configStatus = nullptr;

    QTimer m_reconnectTimer;
    QTimer m_rainbowTimer;
    QTimer m_rgbTestTimer;
    QTimer m_sendTimer;      /* 合并 RGB 下发，40ms */
    QTimer m_uiTimer;        /* 摇杆/按键面板刷新，50ms */
    QTimer m_statTimer;      /* 统计数字，500ms */
    QTimer m_logTimer;       /* 日志批量 flush，80ms */
    QTimer m_configPollTimer;
    QElapsedTimer m_connTime;
    bool m_configWaitingSave = false;
    int m_configPolls = 0;

    quint16 m_lastButtons = 0;
    quint16 m_testedButtons = 0;
    quint8  m_leftDirs = 0;
    quint8  m_rightDirs = 0;
    quint64 m_frames = 0;
    quint64 m_outFrames = 0;
    quint64 m_rgbBaseline = 0;
    quint64 m_lastFrameMs = 0;
    qint64  m_lastReportMs = 0;
    int     m_rainbowHue = 0;
    int     m_rgbTestStep = 0;
    QStringList m_events;
    QStringList m_logBuf;
    int     m_curRgb[3] = { 0, 0, 0 };
    int     m_pendingRgb[3] = { 0, 0, 0 };
    Proto::Report m_latest;
    QByteArray   m_lastRaw;
    qint64  m_lastOutLogMs = 0;
    bool    m_rgbDirty = false;
    bool    m_uiDirty = false;
    bool    m_connectWanted = false;
    bool    m_connected = false;

    /* 速率统计：最近 1 秒收到的帧数 */
    qint64  m_rateWindowStart = 0;
    quint64 m_rateWindowFrames = 0;
    double  m_rateHz = 0.0;
};

#endif /* MAINWINDOW_H */
