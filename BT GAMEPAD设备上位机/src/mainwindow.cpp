/* ============================================================================
 * 蓝牙手柄上位机主窗口
 *   - 左侧：左右摇杆的实时位置 + 10 个按键的灯与次数（考核第 3 项）
 *   - 右上：板载 RGB 三通道滑块 / 预设色 / 彩虹动画（考核第 5 项）
 *   - 右下：链路统计（帧数、报告速率、最近一帧时间、原始报文）
 *   - 底部：通信日志、自动重连、错误提示
 *
 * 数据流：HidDevice 的读线程 -> reportReceived -> onReport（界面线程）
 *         界面只存"最新一帧 + 脏标志"，真正的重绘落在 50ms 的 onUiTick，
 *         这样蓝牙 50Hz 的报告不会直接变成 50 次/秒的控件重绘。
 * ========================================================================== */

#include "mainwindow.h"
#include "protocol.h"
#include "dashboard.h"
#include "performance_monitor.h"

#include <QApplication>
#include <QComboBox>
#include <QPushButton>
#include <QCheckBox>
#include <QSlider>
#include <QSpinBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QGroupBox>
#include <QTabWidget>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QStatusBar>
#include <QMessageBox>
#include <QFileDialog>
#include <QFile>
#include <QTextStream>
#include <QStringConverter>
#include <QDateTime>
#include <QSettings>
#include <QPainter>
#include <QFont>
#include <QColor>
#include <QSizePolicy>
#include <QScrollBar>
#include <QScrollArea>
#include <QFrame>

/* 所有功能页使用相同的纵向滚动策略，低高度窗口不再挤压内容。 */
static QScrollArea *makeScrollPage(QWidget *content, int minimumHeight)
{
    content->setMinimumHeight(minimumHeight);
    auto *area = new QScrollArea;
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    area->setWidget(content);
    return area;
}

namespace {

const QColor kAccent(0x0F, 0x8F, 0x83);
const QColor kDim(0x62, 0x7D, 0x98);
const QColor kOk(0x15, 0x80, 0x3D);
const QColor kWarn(0xC2, 0x41, 0x3B);

} /* namespace */

/* ===========================================================================
 * StickPad：摇杆面板
 * ======================================================================== */
StickPad::StickPad(const QString &title, QWidget *parent)
    : QWidget(parent), m_title(title)
{
    setMinimumSize(150, 150);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void StickPad::setStick(quint16 x, quint16 y)
{
    /* X：值大 = 右。Y：Windows HID 约定值大 = 下，界面向上为正，所以取反 */
    const int nx = Proto::stickPercent(x);
    const int ny = -Proto::stickPercent(y);

    if (nx == m_x && ny == m_y && x == m_rawX && y == m_rawY) return;
    m_x = nx;
    m_y = ny;
    m_rawX = x;
    m_rawY = y;
    update();
}

void StickPad::reset()
{
    m_x = 0;
    m_y = 0;
    m_rawX = 0x8000;
    m_rawY = 0x8000;
    update();
}

void StickPad::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    /* 底盘 */
    const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(QPen(QColor(0xE1, 0xE5, 0xEA), 1));
    p.setBrush(QColor(0xFA, 0xFB, 0xFD));
    p.drawRoundedRect(box, 8, 8);

    /* 标题 */
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() - 1.0);
    p.setFont(f);
    p.setPen(kDim);
    p.drawText(QRectF(10, 6, box.width() - 20, 18), Qt::AlignLeft | Qt::AlignVCenter, m_title);

    /* 十字与活动范围 */
    const qreal cx = box.center().x();
    const qreal cy = box.center().y() + 8;
    const qreal radius = qMin(box.width(), box.height() - 28) / 2.0 - 8;

    p.setPen(QPen(QColor(0xE7, 0xEA, 0xEF), 1, Qt::DashLine));
    p.drawLine(QPointF(cx - radius, cy), QPointF(cx + radius, cy));
    p.drawLine(QPointF(cx, cy - radius), QPointF(cx, cy + radius));
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0xDD, 0xE2, 0xE8), 1));
    p.drawEllipse(QPointF(cx, cy), radius, radius);

    /* 当前位置：Y 轴向上为正，所以屏幕坐标要取负 */
    const qreal px = cx + radius * (m_x / 100.0);
    const qreal py = cy - radius * (m_y / 100.0);

    p.setPen(QPen(QColor(kAccent.red(), kAccent.green(), kAccent.blue(), 120), 1, Qt::DotLine));
    p.drawLine(QPointF(cx, cy), QPointF(px, py));

    /* 圆点：外圈半透明光晕 + 实心点，离开中位越远越明显 */
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x2F, 0x6F, 0xB5, 50));
    p.drawEllipse(QPointF(px, py), 10, 10);
    p.setBrush(kAccent);
    p.drawEllipse(QPointF(px, py), 5, 5);

    /* 右下角标出归一化后的百分比，方便判断中位是否偏移 */
    QFont f2 = font();
    f2.setPointSizeF(f2.pointSizeF() - 1.5);
    p.setFont(f2);
    p.setPen(kDim);
    p.drawText(QRectF(box.left(), box.bottom() - 20, box.width() - 10, 16),
               Qt::AlignRight | Qt::AlignVCenter,
               QStringLiteral("%1 / %2").arg(m_x).arg(m_y));
}

/* ===========================================================================
 * KeyIndicator：按键指示灯
 * ======================================================================== */
KeyIndicator::KeyIndicator(const QString &name, const QString &func,
                           const QColor &color, QWidget *parent)
    : QWidget(parent), m_name(name), m_func(func), m_color(color)
{
    setMinimumSize(86, 58);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void KeyIndicator::setDown(bool down)
{
    if (m_down == down) return;
    m_down = down;
    update();
}

void KeyIndicator::setPressCount(int n)
{
    if (m_count == n) return;
    m_count = n;
    update();
}

void KeyIndicator::resetCount()
{
    m_count = 0;
    update();
}

void KeyIndicator::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QColor bg = m_down ? QColor(m_color.red(), m_color.green(), m_color.blue(), 28)
                       : QColor(0xFA, 0xFB, 0xFD);
    QColor border = m_down ? m_color : QColor(0xE1, 0xE5, 0xEA);

    p.setPen(QPen(border, m_down ? 2 : 1));
    p.setBrush(bg);
    p.drawRoundedRect(box, 7, 7);

    /* 左上：圆灯 */
    const qreal lampR = 8.0;
    const QPointF lamp(box.left() + 18, box.top() + 20);
    if (m_down) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(m_color.red(), m_color.green(), m_color.blue(), 70));
        p.drawEllipse(lamp, lampR + 4, lampR + 4);
        p.setBrush(m_color);
    } else {
        p.setPen(QPen(QColor(0xD5, 0xDA, 0xE1), 1));
        p.setBrush(QColor(0xE9, 0xEC, 0xF1));
    }
    p.drawEllipse(lamp, lampR, lampR);

    /* 键名 */
    QFont f = font();
    f.setBold(true);
    f.setPointSizeF(f.pointSizeF() + 1.0);
    p.setFont(f);
    p.setPen(m_down ? QColor(0x22, 0x33, 0x44) : QColor(0x55, 0x5F, 0x6B));
    p.drawText(QRectF(box.left() + 32, box.top() + 8, box.width() - 60, 24),
               Qt::AlignLeft | Qt::AlignVCenter, m_name);

    /* 次数 */
    f.setBold(false);
    f.setPointSizeF(f.pointSizeF() - 2.0);
    p.setFont(f);
    p.setPen(m_count > 0 ? kAccent : kDim);
    p.drawText(QRectF(box.right() - 40, box.top() + 8, 32, 24),
               Qt::AlignRight | Qt::AlignVCenter,
               m_count > 0 ? QStringLiteral("×%1").arg(m_count) : QStringLiteral("×0"));

    /* 功能说明 */
    p.setPen(kDim);
    p.drawText(QRectF(box.left() + 32, box.top() + 32, box.width() - 42, 18),
               Qt::AlignLeft | Qt::AlignVCenter, m_func);
}

/* ===========================================================================
 * ColorSwatch：自绘色块
 *   QLabel + setStyleSheet 改背景时 Qt 每次都要重新解析样式表并 polish 控件，
 *   滑动/彩虹时一秒几十次会把 GUI 线程拖住；纯 paintEvent 只有一次 update()。
 * ======================================================================== */
ColorSwatch::ColorSwatch(QWidget *parent) : QWidget(parent)
{
    setMinimumHeight(52);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void ColorSwatch::setColor(int r, int g, int b)
{
    if (m_pct[0] == r && m_pct[1] == g && m_pct[2] == b) return;
    m_pct[0] = r; m_pct[1] = g; m_pct[2] = b;
    QColor c;
    c.setRgbF(r / 100.0, g / 100.0, b / 100.0, 1.0);
    m_color = c;
    update();
}

void ColorSwatch::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);

    p.setPen(QPen(QColor(0xD0, 0xD4, 0xD9), 1));
    p.setBrush(m_color);
    p.drawRoundedRect(r, 5, 5);

    /* 按感知亮度决定文字颜色：纯蓝的 HSV 明度是 255，
     * 但肉眼看上去是深色，用 value() 判断会把浅色文字判成深色，字就糊在底色里。 */
    const int luma = (m_color.red() * 299 + m_color.green() * 587 + m_color.blue() * 114) / 1000;
    const QColor text = (luma > 150) ? QColor(40, 44, 52) : QColor(245, 247, 250);
    QFont f = font();
    f.setBold(true);
    p.setFont(f);
    p.setPen(text);
    p.drawText(rect(), Qt::AlignCenter,
               QStringLiteral("R%1%   G%2%   B%3%   %4")
                   .arg(m_pct[0]).arg(m_pct[1]).arg(m_pct[2])
                   .arg(m_color.name().toUpper()));
}

/* ===========================================================================
 * MainWindow
 * ======================================================================== */
MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    buildUi();

    connect(&m_hid, &HidDevice::reportReceived, this, &MainWindow::onReport);
    connect(&m_hid, &HidDevice::disconnected, this, &MainWindow::onDeviceLost);
    connect(&m_hid, &HidDevice::logLine, this, &MainWindow::onLog);
    connect(&m_hid, &HidDevice::featureResult,
            this, &MainWindow::onFeatureResult, Qt::QueuedConnection);
    m_configPollTimer.setSingleShot(true);
    m_configPollTimer.setInterval(100);
    connect(&m_configPollTimer, &QTimer::timeout, this, &MainWindow::onConfigPoll);

    /* 三个节流定时器：高频操作合并到低频执行，界面永远不会被拖住 */
    m_sendTimer.setSingleShot(true);
    m_sendTimer.setInterval(40);          /* 最多 25 帧/秒 */
    connect(&m_sendTimer, &QTimer::timeout, this, &MainWindow::onSendTick);

    m_uiTimer.setInterval(50);            /* 20Hz 刷新摇杆/按键 */
    connect(&m_uiTimer, &QTimer::timeout, this, &MainWindow::onUiTick);
    m_uiTimer.start();

    m_statTimer.setInterval(500);         /* 2Hz 刷新统计文字 */
    connect(&m_statTimer, &QTimer::timeout, this, &MainWindow::onStatTick);
    m_statTimer.start();

    m_logTimer.setInterval(80);           /* 12Hz 批量写日志 */
    connect(&m_logTimer, &QTimer::timeout, this, &MainWindow::onFlushLog);
    m_logTimer.start();

    m_rainbowTimer.setInterval(60);       /* 彩虹动画 16 帧/秒 */
    connect(&m_rainbowTimer, &QTimer::timeout, this, &MainWindow::onRainbowTick);

    m_rgbTestTimer.setInterval(550);
    connect(&m_rgbTestTimer, &QTimer::timeout, this, &MainWindow::onRgbTestTick);

    m_reconnectTimer.setInterval(2000);   /* 2 秒扫一次，等手柄重新出现 */
    connect(&m_reconnectTimer, &QTimer::timeout, this, &MainWindow::onAutoReconnectTick);

    m_connTime.start();
    loadSettings();
    onRefreshDevices();
}

MainWindow::~MainWindow()
{
    saveSettings();
    m_hid.close();
}

/* ---------------------------------------------------------------------------
 * 界面搭建
 * ------------------------------------------------------------------------- */
void MainWindow::buildUi()
{
    auto *central = new QWidget(this);
    central->setObjectName(QStringLiteral("root"));
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(14, 12, 14, 10);
    root->setSpacing(9);

    m_connLabel = new QLabel(QStringLiteral("未连接"), central);
    root->addWidget(Dashboard::makeHero(
        QStringLiteral("BT GAMEPAD 控制台"),
        QStringLiteral("MP GAMEPAD  ·  蓝牙 HID  ·  双摇杆 / 10 键 / RGB 配置"),
        m_connLabel, central));

    /* ---- 设备选择：保留原 Windows HID 枚举与共享连接方式 ---- */
    auto *top = new QHBoxLayout;
    top->setSpacing(8);
    auto *devLabel = new QLabel(QStringLiteral("游戏控制器"), central);
    devLabel->setStyleSheet(QStringLiteral("color:#486581;font-weight:600;"));
    m_deviceCombo = new QComboBox(central);
    m_deviceCombo->setMinimumWidth(460);
    m_deviceCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_deviceCombo->setMinimumContentsLength(22);
    m_refreshBtn = new QPushButton(QStringLiteral("刷新设备"), central);
    m_connectBtn = new QPushButton(QStringLiteral("连接设备"), central);
    m_connectBtn->setProperty("primary", true);
    m_connectBtn->setMinimumWidth(96);
    m_autoReconnect = new QCheckBox(QStringLiteral("掉线自动重连"), central);
    m_autoReconnect->setChecked(true);
    top->addWidget(devLabel);
    top->addWidget(m_deviceCombo, 1);
    top->addWidget(m_refreshBtn);
    top->addWidget(m_connectBtn);
    top->addWidget(m_autoReconnect);
    root->addLayout(top);

    auto *metrics = new QHBoxLayout;
    metrics->setSpacing(9);
    m_framesCard = new Dashboard::MetricCard(QStringLiteral("输入报告"), QStringLiteral("HID INPUT · 帧"), central);
    m_rateCard = new Dashboard::MetricCard(QStringLiteral("实时速率"), QStringLiteral("目标 50 Hz"), central);
    m_keysCard = new Dashboard::MetricCard(QStringLiteral("按键覆盖"), QStringLiteral("至少按下过一次"), central);
    m_rgbCard = new Dashboard::MetricCard(QStringLiteral("RGB 成功下发"), QStringLiteral("写线程实际确认"), central);
    for (QWidget *card : {m_framesCard, m_rateCard, m_keysCard, m_rgbCard}) metrics->addWidget(card, 1);
    root->addLayout(metrics);

    connect(m_refreshBtn, &QPushButton::clicked, this, &MainWindow::onRefreshDevices);
    connect(m_connectBtn, &QPushButton::clicked, this, &MainWindow::onConnectClicked);
    connect(m_deviceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { if (!m_connected) m_connLabel->setText(QStringLiteral("未连接")); });

    /* ---- 主体：左（摇杆 + 按键） / 右（RGB + 统计） ---- */
    auto *body = new QHBoxLayout;
    body->setSpacing(8);

    /* 左：摇杆 */
    auto *stickBox = new QGroupBox(QStringLiteral("摇杆（左 X/Y，右 Z/Rz）"), central);
    auto *stickLay = new QVBoxLayout(stickBox);
    stickLay->setSpacing(6);

    auto *pads = new QHBoxLayout;
    pads->setSpacing(8);
    m_padL = new StickPad(QStringLiteral("左摇杆  X / Y"), stickBox);
    m_padR = new StickPad(QStringLiteral("右摇杆  Z / Rz"), stickBox);
    pads->addWidget(m_padL, 1);
    pads->addWidget(m_padR, 1);
    stickLay->addLayout(pads, 1);

    auto *padValues = new QHBoxLayout;
    padValues->setSpacing(8);
    m_padLValue = new QLabel(QStringLiteral("X 32768   Y 32768"), stickBox);
    m_padRValue = new QLabel(QStringLiteral("Z 32768   Rz 32768"), stickBox);
    const QString mono = QStringLiteral("font-family:Consolas,'Courier New',monospace;color:#555F6B;");
    m_padLValue->setStyleSheet(mono);
    m_padRValue->setStyleSheet(mono);
    m_padLValue->setAlignment(Qt::AlignCenter);
    m_padRValue->setAlignment(Qt::AlignCenter);
    padValues->addWidget(m_padLValue, 1);
    padValues->addWidget(m_padRValue, 1);
    stickLay->addLayout(padValues);

    /* 左列：上面摇杆、下面按键。摇杆吃掉多余高度，按键保持自身高度，
     * 这样窗口拉高时不会在按键下面留一大块空白。 */
    auto *leftCol = new QVBoxLayout;
    leftCol->setSpacing(8);
    leftCol->addWidget(stickBox, 1);

    /* 左中：按键 */
    auto *keyBox = new QGroupBox(QStringLiteral("按键（报告描述符 Button 1~10）"), central);
    auto *keyLay = new QVBoxLayout(keyBox);
    keyLay->setSpacing(6);

    auto *keyGrid = new QGridLayout;
    keyGrid->setSpacing(6);
    static const QColor keyColors[10] = {
        QColor(0xE4, 0x3B, 0x44), QColor(0xE0, 0x7B, 0x2A), QColor(0x2E, 0x9E, 0x5B),
        QColor(0x2F, 0x6F, 0xB5), QColor(0x7A, 0x5C, 0xC6), QColor(0x16, 0xA0, 0xA8),
        QColor(0xC2, 0x46, 0x8C), QColor(0x8A, 0x8F, 0x3A), QColor(0x55, 0x6B, 0x8A),
        QColor(0x8A, 0x6A, 0x4A),
    };
    for (int i = 0; i < 10; ++i) {
        auto *k = new KeyIndicator(QString::fromUtf8(Proto::kKeys[i].name),
                                   QString::fromUtf8(Proto::kKeys[i].function),
                                   keyColors[i], keyBox);
        m_keys.append(k);
        keyGrid->addWidget(k, i / 5, i % 5);
    }
    keyLay->addLayout(keyGrid);

    m_btnLabel = new QLabel(QStringLiteral("当前按下：（无）"), keyBox);
    m_btnLabel->setStyleSheet(QStringLiteral(
        "background:#F6F7F9;border:1px solid #E3E6EA;border-radius:4px;padding:5px;color:#444;"));
    keyLay->addWidget(m_btnLabel);

    auto *clearCnt = new QPushButton(QStringLiteral("清零计数"), keyBox);
    connect(clearCnt, &QPushButton::clicked, this, &MainWindow::onClearCounters);
    keyLay->addWidget(clearCnt, 0, Qt::AlignRight);

    keyBox->setMaximumWidth(620);
    leftCol->addWidget(keyBox, 0);
    body->addLayout(leftCol, 5);

    /* 右：RGB + 统计 */
    auto *rightCol = new QVBoxLayout;
    rightCol->setSpacing(8);

    m_rgbBox = new QGroupBox(QStringLiteral("板载 RGB 灯（HID 输出报告 Report ID 3）"), central);
    auto *rgbLay = new QVBoxLayout(m_rgbBox);
    rgbLay->setSpacing(6);

    auto *sliderForm = new QGridLayout;
    sliderForm->setSpacing(8);
    const char *channelNames[3] = { "R（红）", "G（绿）", "B（蓝）" };
    const char *channelColor[3] = { "#E43B44", "#4CAF50", "#3F7CE0" };
    for (int i = 0; i < 3; ++i) {
        auto *lab = new QLabel(QString::fromUtf8(channelNames[i]), m_rgbBox);
        lab->setStyleSheet(QString("color:%1;font-weight:bold;").arg(channelColor[i]));
        auto *sl = new QSlider(Qt::Horizontal, m_rgbBox);
        sl->setRange(0, 100);
        sl->setValue(0);
        auto *sp = new QSpinBox(m_rgbBox);
        sp->setRange(0, 100);
        sp->setValue(0);
        sp->setSuffix(QStringLiteral(" %"));
        sliderForm->addWidget(lab, i, 0);
        sliderForm->addWidget(sl, i, 1);
        sliderForm->addWidget(sp, i, 2);
        m_sliders.append(sl);
        m_spinBoxes.append(sp);
        connect(sl, &QSlider::valueChanged, sp, &QSpinBox::setValue);
        connect(sp, QOverload<int>::of(&QSpinBox::valueChanged), sl, &QSlider::setValue);
        connect(sl, &QSlider::valueChanged, this, &MainWindow::onSliderChanged);
    }
    rgbLay->addLayout(sliderForm);

    m_colorPreview = new ColorSwatch(m_rgbBox);
    rgbLay->addWidget(m_colorPreview);

    auto *presetLay = new QGridLayout;
    presetLay->setSpacing(5);
    struct Preset { const char *name; int r, g, b; };
    static const Preset presets[] = {
        { "红", 100,   0,   0 }, { "绿",   0, 100,   0 }, { "蓝",   0,   0, 100 },
        { "白", 100, 100, 100 }, { "黄", 100, 100,   0 }, { "青",   0, 100, 100 },
        { "紫", 100,   0, 100 }, { "熄灭", 0,   0,   0 },
    };
    for (int i = 0; i < 8; ++i) {
        auto *b = new QPushButton(QString::fromUtf8(presets[i].name), m_rgbBox);
        b->setMinimumHeight(26);
        b->setProperty("pr", i);
        connect(b, &QPushButton::clicked, this, &MainWindow::onPresetClicked);
        presetLay->addWidget(b, i / 4, i % 4);
    }
    rgbLay->addLayout(presetLay);

    auto *sendRow = new QHBoxLayout;
    m_liveSend = new QCheckBox(QStringLiteral("拖动即下发"), m_rgbBox);
    m_liveSend->setChecked(true);
    m_rainbow = new QCheckBox(QStringLiteral("彩虹动画"), m_rgbBox);
    auto *sendBtn = new QPushButton(QStringLiteral("立即下发"), m_rgbBox);
    sendRow->addWidget(m_liveSend);
    sendRow->addWidget(m_rainbow);
    sendRow->addStretch(1);
    sendRow->addWidget(sendBtn);
    rgbLay->addLayout(sendRow);

    connect(m_rainbow, &QCheckBox::toggled, this, [this](bool on) {
        if (on) m_rainbowTimer.start(); else m_rainbowTimer.stop();
    });
    connect(sendBtn, &QPushButton::clicked, this, &MainWindow::onSendRgb);

    rightCol->addWidget(m_rgbBox, 0);

    auto *statBox = new QGroupBox(QStringLiteral("链路状态"), central);
    auto *statLay = new QVBoxLayout(statBox);
    statLay->setSpacing(4);

    m_frameLabel = new QLabel(QStringLiteral("收到报告：0 帧"), statBox);
    m_rateLabel  = new QLabel(QStringLiteral("报告速率：0.0 帧/秒（固件 50Hz）"), statBox);
    m_ageLabel   = new QLabel(QStringLiteral("最近一帧：—"), statBox);
    m_heartbeatLabel = new QLabel(QStringLiteral("链路：未连接"), statBox);
    m_outLabel   = new QLabel(QStringLiteral("已下发：0 帧 RGB"), statBox);
    m_durationLabel = new QLabel(QStringLiteral("连接时长：—"), statBox);
    m_rawLabel   = new QLabel(QStringLiteral("原始：—"), statBox);
    m_eventLabel = new QLabel(QStringLiteral("事件：—"), statBox);
    m_pathLabel  = new QLabel(QStringLiteral("设备路径：—"), statBox);

    m_frameLabel->setStyleSheet(QStringLiteral("color:#444;"));
    m_rateLabel->setStyleSheet(QStringLiteral("color:#444;"));
    m_ageLabel->setStyleSheet(QStringLiteral("color:#444;"));
    m_heartbeatLabel->setStyleSheet(QStringLiteral("color:#9AA3AE;font-weight:bold;"));
    m_outLabel->setStyleSheet(QStringLiteral("color:#444;"));
    m_durationLabel->setStyleSheet(QStringLiteral("color:#444;"));
    m_rawLabel->setStyleSheet(QStringLiteral(
        "font-family:Consolas,'Courier New',monospace;color:#666;"));
    m_rawLabel->setWordWrap(true);
    m_eventLabel->setStyleSheet(QStringLiteral("color:#666;"));
    m_eventLabel->setWordWrap(true);
    m_pathLabel->setStyleSheet(QStringLiteral(
        "font-family:Consolas,'Courier New',monospace;color:#98A2AD;font-size:10px;"));
    m_pathLabel->setWordWrap(true);

    statLay->addWidget(m_frameLabel);
    statLay->addWidget(m_rateLabel);
    statLay->addWidget(m_ageLabel);
    statLay->addWidget(m_heartbeatLabel);
    statLay->addWidget(m_outLabel);
    statLay->addWidget(m_durationLabel);
    statLay->addWidget(m_rawLabel);
    statLay->addWidget(m_eventLabel);
    statLay->addSpacing(4);
    statLay->addWidget(m_pathLabel);
    statLay->addStretch(1);

    rightCol->addStretch(1);
    body->addLayout(rightCol, 4);

    auto *workspace = new QTabWidget(central);
    auto *monitorPage = new QWidget(workspace);
    monitorPage->setLayout(body);
    workspace->addTab(makeScrollPage(monitorPage, 560), QStringLiteral("实时监测"));
    workspace->addTab(makeScrollPage(statBox, 500), QStringLiteral("链路详情"));
    root->addWidget(workspace, 1);

    m_assessmentBar = new Dashboard::AssessmentBar(central);
    root->addWidget(m_assessmentBar);
    connect(m_assessmentBar->detailsButton(), &QPushButton::clicked,
            this, &MainWindow::onShowAssessment);
    connect(m_assessmentBar->testButton(), &QPushButton::clicked,
            this, &MainWindow::onStartRgbTest);
    connect(m_assessmentBar->exportButton(), &QPushButton::clicked,
            this, &MainWindow::onExportReport);

    /* ---- 底部：日志 ---- */
    auto *logBox = new QGroupBox(QStringLiteral("通信日志"), central);
    auto *logLay = new QVBoxLayout(logBox);
    logLay->setSpacing(6);

    m_logView = new QPlainTextEdit(logBox);
    m_logView->setReadOnly(true);
    m_logView->setMaximumBlockCount(1200);
    m_logView->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_logView->setUndoRedoEnabled(false);
    logLay->addWidget(m_logView, 1);

    auto *logRow = new QHBoxLayout;
    m_logEveryFrame = new QCheckBox(QStringLiteral("记录每一帧"), logBox);
    m_pauseLog = new QCheckBox(QStringLiteral("暂停滚动"), logBox);
    auto *clearLog = new QPushButton(QStringLiteral("清空"), logBox);
    auto *saveLog = new QPushButton(QStringLiteral("保存…"), logBox);
    logRow->addWidget(m_logEveryFrame);
    logRow->addWidget(m_pauseLog);
    logRow->addStretch(1);
    logRow->addWidget(clearLog);
    logRow->addWidget(saveLog);
    logLay->addLayout(logRow);

    connect(clearLog, &QPushButton::clicked, this, &MainWindow::onClearLog);
    connect(saveLog, &QPushButton::clicked, this, &MainWindow::onSaveLog);

    logBox->setTitle(QStringLiteral("原始数据与事件"));
    workspace->addTab(makeScrollPage(logBox, 480), QStringLiteral("通信日志"));

    /* 蓝牙 Feature Report 配置页：读取不擦 Flash，只有保存按钮触发掉电存储。 */
    m_configBox = new QGroupBox(QStringLiteral("左右摇杆灵敏度与死区（蓝牙 HID Feature Report）"), workspace);
    auto *configLay = new QVBoxLayout(m_configBox);
    auto *configGrid = new QGridLayout;
    const char *stickNames[2] = { "左摇杆", "右摇杆" };
    for (int i = 0; i < 2; ++i) {
        configGrid->addWidget(new QLabel(QString::fromUtf8(stickNames[i]), m_configBox), i, 0);
        configGrid->addWidget(new QLabel(QStringLiteral("灵敏度"), m_configBox), i, 1);
        m_sens[i] = new QSpinBox(m_configBox);
        m_sens[i]->setRange(25, 200); m_sens[i]->setSingleStep(5);
        m_sens[i]->setValue(100); m_sens[i]->setSuffix(QStringLiteral(" %"));
        configGrid->addWidget(m_sens[i], i, 2);
        configGrid->addWidget(new QLabel(QStringLiteral("死区"), m_configBox), i, 3);
        m_deadzone[i] = new QSpinBox(m_configBox);
        m_deadzone[i]->setRange(0, 24000); m_deadzone[i]->setSingleStep(500);
        m_deadzone[i]->setValue(0); m_deadzone[i]->setToolTip(QStringLiteral("相对 32767 满量程的死区幅值，默认 0"));
        configGrid->addWidget(m_deadzone[i], i, 4);
    }
    configLay->addLayout(configGrid);
    auto *configButtons = new QHBoxLayout;
    auto *readConfig = new QPushButton(QStringLiteral("读取配置"), m_configBox);
    auto *writeConfig = new QPushButton(QStringLiteral("写入并保存"), m_configBox);
    auto *defaultConfig = new QPushButton(QStringLiteral("恢复默认并保存"), m_configBox);
    writeConfig->setProperty("primary", true);
    configButtons->addWidget(readConfig); configButtons->addWidget(writeConfig);
    configButtons->addWidget(defaultConfig); configButtons->addStretch(1);
    configLay->addLayout(configButtons);
    m_configStatus = new QLabel(QStringLiteral("连接手柄后可读取；仅点击保存才擦写 Flash。"), m_configBox);
    m_configStatus->setWordWrap(true);
    configLay->addWidget(m_configStatus);
    configLay->addStretch(1);
    workspace->addTab(makeScrollPage(m_configBox, 360), QStringLiteral("摇杆配置"));
    connect(readConfig, &QPushButton::clicked, this, &MainWindow::onConfigRead);
    connect(writeConfig, &QPushButton::clicked, this, &MainWindow::onConfigWrite);
    connect(defaultConfig, &QPushButton::clicked, this, &MainWindow::onConfigDefault);
    m_configBox->setEnabled(false);

    m_performance = new PerformanceMonitor(workspace);
    m_performance->setSequenceSupport(false);
    workspace->addTab(makeScrollPage(m_performance, 700), QStringLiteral("性能测试"));
    connect(m_performance, &PerformanceMonitor::resetRequested,
            this, &MainWindow::onClearCounters);
    connect(m_performance, &PerformanceMonitor::exportRequested,
            this, &MainWindow::onExportReport);

    setCentralWidget(central);
    setStyleSheet(Dashboard::styleSheet(QStringLiteral("#0F8F83"),
                                        QStringLiteral("#0B6E66")));
    m_statusLabel = new QLabel(QStringLiteral("Windows HID  ·  输入 Report ID 1  ·  RGB 输出 Report ID 3"), this);
    statusBar()->addWidget(m_statusLabel);
    updateStats();
}

/* ---------------------------------------------------------------------------
 * 设备枚举
 * ------------------------------------------------------------------------- */
void MainWindow::populateDeviceCombo()
{
    const QString keep = m_deviceCombo->currentData().toString();

    m_deviceCombo->blockSignals(true);
    m_deviceCombo->clear();
    if (m_devices.isEmpty())
        m_deviceCombo->addItem(QStringLiteral("（没有发现游戏控制器，先连上手柄再刷新）"),
                               QString());
    for (const HidDeviceInfo &d : m_devices)
        m_deviceCombo->addItem(d.label(), d.path);
    m_deviceCombo->blockSignals(false);

    if (!keep.isEmpty()) {
        const int idx = m_deviceCombo->findData(keep);
        if (idx >= 0) m_deviceCombo->setCurrentIndex(idx);
    }
}

void MainWindow::onRefreshDevices()
{
    m_devices = HidDevice::enumerateGamepads();
    populateDeviceCombo();

    if (m_devices.isEmpty()) {
        m_connLabel->setText(QStringLiteral("未发现游戏控制器"));
        m_connLabel->setStyleSheet(QStringLiteral("color:#D14B3F;font-weight:bold;"));
        appendLog(QStringLiteral("系统"),
                  QStringLiteral("没有找到游戏控制器。先让手柄进入配对/回连状态，"
                                 "在 Windows「设置 → 蓝牙和其他设备」里连上，"
                                 "再点「刷新设备」。"));
    } else {
        if (!m_connected) {
            m_connLabel->setText(QStringLiteral("发现 %1 个手柄").arg(m_devices.size()));
            m_connLabel->setStyleSheet(QStringLiteral("color:#2E9E5B;font-weight:bold;"));
        }
        QStringList names;
        for (const HidDeviceInfo &d : m_devices) names << d.label();
        appendLog(QStringLiteral("扫描"),
                  QStringLiteral("发现 %1 个游戏控制器：%2")
                      .arg(m_devices.size()).arg(names.join(QStringLiteral("；"))));
    }
}

/* ---------------------------------------------------------------------------
 * 连接 / 断开
 * ------------------------------------------------------------------------- */
void MainWindow::onConnectClicked()
{
    if (m_connected) {
        m_connectWanted = false;
        m_reconnectTimer.stop();
        m_hid.close();
        setConnected(false);
        appendLog(QStringLiteral("系统"), QStringLiteral("已断开"));
        return;
    }

    const int idx = m_deviceCombo->currentIndex();
    if (idx < 0 || idx >= m_devices.size()) {
        QMessageBox::information(this, QStringLiteral("提示"),
                                 QStringLiteral("还没有选中设备。先插好/连好手柄，再点「刷新设备」。"));
        return;
    }

    if (!m_hid.open(m_devices.at(idx))) {
        m_connLabel->setText(QStringLiteral("打开失败"));
        m_connLabel->setStyleSheet(QStringLiteral("color:#D14B3F;font-weight:bold;"));
        return;
    }

    m_connectWanted = true;
    m_reconnectTimer.start();
    setConnected(true);

    /* 延迟 400ms 再下发首帧颜色：让读线程先把 ReadFile 挂上，
     * 避免连接瞬间的输出报告抢占 I/O 队列导致读线程启动失败。 */
    QTimer::singleShot(400, this, [this]() {
        if (m_connected)
            sendRgbNow(m_curRgb[0], m_curRgb[1], m_curRgb[2]);
    });
}

void MainWindow::setConnected(bool on)
{
    m_connected = on;
    m_performance->setConnectionState(on);
    m_connectBtn->setText(on ? QStringLiteral("断开连接") : QStringLiteral("连接设备"));
    m_deviceCombo->setEnabled(!on);
    m_refreshBtn->setEnabled(!on);
    m_configBox->setEnabled(on);
    if (!on) {
        m_configWaitingSave = false;
        m_configPollTimer.stop();
    } else {
        QTimer::singleShot(250, this, &MainWindow::onConfigRead);
    }

    if (on) {
        m_connLabel->setText(QStringLiteral("已连接"));
        m_connLabel->setStyleSheet(QStringLiteral("color:#7EE2B8;font-weight:bold;"));
        m_heartbeatLabel->setText(QStringLiteral("链路：已连接，等待报告"));
        m_heartbeatLabel->setStyleSheet(QStringLiteral("color:#15803D;font-weight:bold;"));
        m_connTime.restart();
        m_frames = 0;
        m_outFrames = 0;
        m_lastButtons = 0;
        m_testedButtons = 0;
        m_leftDirs = 0;
        m_rightDirs = 0;
        m_rgbBaseline = m_hid.outOkCount();
        m_lastReportMs = 0;
        m_rateWindowFrames = 0;
        m_rateWindowStart = 0;
        m_rateHz = 0.0;
        for (KeyIndicator *k : m_keys) k->resetCount();

        QString p = m_hid.info().path;
        if (p.size() > 84) p = p.left(26) + QStringLiteral(" … ") + p.right(54);
        m_pathLabel->setText(QStringLiteral("设备路径：%1").arg(p));
    } else {
        m_connLabel->setText(QStringLiteral("未连接"));
        m_connLabel->setStyleSheet(QStringLiteral("color:#C4D5E2;font-weight:bold;"));
        m_heartbeatLabel->setText(QStringLiteral("链路：未连接"));
        m_heartbeatLabel->setStyleSheet(QStringLiteral("color:#627D98;font-weight:bold;"));
        m_padL->reset();
        m_padR->reset();
        for (KeyIndicator *k : m_keys) k->setDown(false);
        m_btnLabel->setText(QStringLiteral("当前按下：（无）"));
        m_padLValue->setText(QStringLiteral("X 32768   Y 32768"));
        m_padRValue->setText(QStringLiteral("Z 32768   Rz 32768"));
        m_rawLabel->setText(QStringLiteral("原始：—"));
        m_ageLabel->setText(QStringLiteral("最近一帧：—"));
        m_frameLabel->setText(QStringLiteral("收到报告：0 帧"));
        m_rateLabel->setText(QStringLiteral("报告速率：0.0 帧/秒（固件 50Hz）"));
        m_outLabel->setText(QStringLiteral("已下发：0 帧 RGB"));
        m_durationLabel->setText(QStringLiteral("连接时长：—"));
        m_pathLabel->setText(QStringLiteral("设备路径：—"));
        m_rgbTestTimer.stop();
        m_assessmentBar->testButton()->setText(QStringLiteral("RGB 通道自检"));
        if (m_rainbow->isChecked()) m_rainbow->setChecked(false);
    }
    updateStats();
    updateAssessment();
}

void MainWindow::onDeviceLost(const QString &reason)
{
    if (!m_connected) return;

    appendLog(QStringLiteral("错误"), QStringLiteral("链路断开：%1").arg(reason), kWarn);
    m_hid.close();
    setConnected(false);

    if (m_connectWanted && m_autoReconnect->isChecked()) {
        appendLog(QStringLiteral("系统"),
                  QStringLiteral("开始自动重连：手柄回连成功后会自动接上（每 2 秒扫一次）"));
        m_reconnectTimer.start();
    }
}

void MainWindow::onAutoReconnectTick()
{
    if (m_connected || !m_connectWanted) {
        m_reconnectTimer.stop();
        return;
    }

    const QList<HidDeviceInfo> list = HidDevice::enumerateGamepads();
    if (list.isEmpty()) return;

    m_devices = list;
    populateDeviceCombo();

    /* 优先按设备路径找原来那一个；换了系统分配的路径就退而求其次，
     * 只要系统里只有一个手柄，就直接连它。 */
    const QString want = m_hid.info().path;
    int pick = -1;
    for (int i = 0; i < m_devices.size(); ++i) {
        if (!want.isEmpty() && m_devices.at(i).path == want) { pick = i; break; }
    }
    if (pick < 0 && m_devices.size() == 1) pick = 0;
    if (pick < 0) return;

    if (m_hid.open(m_devices.at(pick))) {
        setConnected(true);
        sendRgbNow(m_curRgb[0], m_curRgb[1], m_curRgb[2]);
        appendLog(QStringLiteral("系统"), QStringLiteral("自动重连成功"));
        m_reconnectTimer.stop();
    }
}

/* ---------------------------------------------------------------------------
 * 收到一帧输入报告
 * ------------------------------------------------------------------------- */
void MainWindow::onReport(const QByteArray &raw)
{
    const qint64 perfNow = QDateTime::currentMSecsSinceEpoch();
    Proto::Report rpt;
    if (!Proto::parseReport(raw, &rpt)) {
        m_performance->recordInput(perfNow, false);
        return;
    }
    m_performance->recordInput(perfNow, true);

    ++m_frames;
    ++m_rateWindowFrames;
    m_lastReportMs = perfNow;

    /* 考核覆盖只由真实报告更新：按键按下过一次即记录，摇杆需推到 60% 以外。 */
    const quint16 now = rpt.buttons;
    m_testedButtons = quint16(m_testedButtons | now);
    const auto directionMask = [](const Proto::Stick &stick) {
        const int x = Proto::stickPercent(stick.x);
        const int y = Proto::stickPercent(stick.y);
        quint8 mask = 0;
        if (x >= 60) mask |= 0x01;
        if (x <= -60) mask |= 0x02;
        if (y >= 60) mask |= 0x04;
        if (y <= -60) mask |= 0x08;
        return mask;
    };
    m_leftDirs = quint8(m_leftDirs | directionMask(rpt.left));
    m_rightDirs = quint8(m_rightDirs | directionMask(rpt.right));

    /* 按键：只在变化时计数和记事件，避免 50Hz 刷屏 */
    const quint16 changed = quint16(now ^ m_lastButtons);
    if (changed != 0) {
        for (int i = 0; i < 10; ++i) {
            const quint16 bit = quint16(1u << i);
            if ((changed & bit) == 0) continue;
            if (now & bit) {
                m_keys[i]->setPressCount(m_keys[i]->pressCount() + 1);
                m_events.append(QStringLiteral("%1 %2 按下")
                                    .arg(QDateTime::currentDateTime().toString("HH:mm:ss"))
                                    .arg(QString::fromUtf8(Proto::kKeys[i].name)));
                if (m_events.size() > 6) m_events.removeFirst();
            }
        }
        m_lastButtons = now;
        if (m_logEveryFrame->isChecked())
            appendLog(QStringLiteral("按键"), Proto::bitmapToText(now));
    }

    /* 摇杆/按键的实际重绘交给 50ms 的 onUiTick */
    m_latest = rpt;
    m_lastRaw = raw;
    m_uiDirty = true;

    if (m_logEveryFrame->isChecked())
        appendLog(QStringLiteral("IN"), Proto::hexDump(raw));
}

/* ---------------------------------------------------------------------------
 * 界面刷新（20Hz）
 * ------------------------------------------------------------------------- */
void MainWindow::onUiTick()
{
    if (!m_uiDirty || !m_connected) return;
    m_uiDirty = false;

    const Proto::Report &r = m_latest;

    m_padL->setStick(r.left.x, r.left.y);
    m_padR->setStick(r.right.x, r.right.y);
    m_padLValue->setText(QStringLiteral("X %1   Y %2").arg(r.left.x).arg(r.left.y));
    m_padRValue->setText(QStringLiteral("Z %1   Rz %2").arg(r.right.x).arg(r.right.y));

    for (int i = 0; i < 10; ++i)
        m_keys[i]->setDown((r.buttons & (1u << i)) != 0u);

    m_btnLabel->setText(QStringLiteral("当前按下：%1").arg(Proto::bitmapToText(r.buttons)));
    m_rawLabel->setText(QStringLiteral("原始：%1").arg(Proto::hexDump(m_lastRaw)));
}

void MainWindow::onStatTick()
{
    if (!m_connected) return;

    /* 速率：按 1 秒窗口算，避免用"最近两帧间隔"抖得厉害 */
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_rateWindowStart == 0) {
        m_rateWindowStart = now;
        m_rateWindowFrames = 0;
    } else {
        const qint64 span = now - m_rateWindowStart;
        if (span >= 1000) {
            m_rateHz = double(m_rateWindowFrames) * 1000.0 / double(span);
            m_rateWindowStart = now;
            m_rateWindowFrames = 0;
        }
    }

    updateStats();
}

void MainWindow::updateStats()
{
    m_performance->tick(QDateTime::currentMSecsSinceEpoch(), m_hid.outOkCount());
    m_frameLabel->setText(QStringLiteral("收到报告：%1 帧").arg(m_frames));
    m_rateLabel->setText(QStringLiteral("报告速率：%1 帧/秒（固件 50Hz）")
                             .arg(m_rateHz, 0, 'f', 1));
    m_outLabel->setText(QStringLiteral("已下发：%1 帧 RGB").arg(m_hid.outOkCount()));

    int tested = 0;
    for (int i = 0; i < 10; ++i) tested += ((m_testedButtons & (1u << i)) != 0) ? 1 : 0;
    m_framesCard->setValue(QString::number(m_frames));
    m_rateCard->setValue(QStringLiteral("%1 Hz").arg(m_rateHz, 0, 'f', 1));
    m_keysCard->setValue(QStringLiteral("%1 / 10").arg(tested));
    m_rgbCard->setValue(QString::number(m_hid.outOkCount()));
    updateAssessment();

    if (m_connected) {
        const qint64 sec = m_connTime.elapsed() / 1000;
        m_durationLabel->setText(QStringLiteral("连接时长：%1:%2")
                                     .arg(sec / 60, 2, 10, QChar('0'))
                                     .arg(sec % 60, 2, 10, QChar('0')));
    }

    if (m_lastReportMs > 0) {
        const qint64 age = QDateTime::currentMSecsSinceEpoch() - m_lastReportMs;
        m_ageLabel->setText(QStringLiteral("最近一帧：%1 ms 前").arg(age));

        /* 手柄每 20ms 一帧，超过 500ms 没数据基本可以判定链路断了 */
        if (age > 500) {
            m_heartbeatLabel->setText(QStringLiteral("链路：超过 500ms 没有数据"));
            m_heartbeatLabel->setStyleSheet(QStringLiteral("color:#D14B3F;font-weight:bold;"));
        } else {
            m_heartbeatLabel->setText(QStringLiteral("链路：正常（20ms 一帧）"));
            m_heartbeatLabel->setStyleSheet(QStringLiteral("color:#2E9E5B;font-weight:bold;"));
        }
    } else {
        m_ageLabel->setText(QStringLiteral("最近一帧：—"));
    }

    if (!m_events.isEmpty())
        m_eventLabel->setText(QStringLiteral("事件：%1").arg(m_events.join(QStringLiteral("  ·  "))));
}

/* ---------------------------------------------------------------------------
 * RGB 下发
 * ------------------------------------------------------------------------- */
void MainWindow::onSliderChanged(int)
{
    const int r = m_sliders[0]->value();
    const int g = m_sliders[1]->value();
    const int b = m_sliders[2]->value();

    m_colorPreview->setColor(r, g, b);
    m_curRgb[0] = r;
    m_curRgb[1] = g;
    m_curRgb[2] = b;

    if (m_liveSend->isChecked()) queueRgb(r, g, b);
}

void MainWindow::onPresetClicked()
{
    auto *btn = qobject_cast<QPushButton *>(sender());
    if (!btn) return;
    const int i = btn->property("pr").toInt();

    static const int preset[8][3] = {
        { 100, 0, 0 }, { 0, 100, 0 }, { 0, 0, 100 }, { 100, 100, 100 },
        { 100, 100, 0 }, { 0, 100, 100 }, { 100, 0, 100 }, { 0, 0, 0 },
    };
    applyColor(preset[i][0], preset[i][1], preset[i][2], true);
}

void MainWindow::applyColor(int r, int g, int b, bool send)
{
    /* 直接改滑块会把 valueChanged 再打回来一次（→ onSliderChanged → 递归），
     * 这里连 spinBox 一起挡信号，界面只做一次赋值。 */
    const int v[3] = { r, g, b };
    for (int i = 0; i < 3; ++i) {
        m_sliders[i]->blockSignals(true);
        m_spinBoxes[i]->blockSignals(true);
        m_sliders[i]->setValue(v[i]);
        m_spinBoxes[i]->setValue(v[i]);
        m_sliders[i]->blockSignals(false);
        m_spinBoxes[i]->blockSignals(false);
    }

    m_colorPreview->setColor(r, g, b);
    m_curRgb[0] = r;
    m_curRgb[1] = g;
    m_curRgb[2] = b;

    if (send) queueRgb(r, g, b);
}

void MainWindow::queueRgb(int r, int g, int b)
{
    m_pendingRgb[0] = r;
    m_pendingRgb[1] = g;
    m_pendingRgb[2] = b;
    m_rgbDirty = true;
    /* 单次定时器：连续拖动只会重置，40ms 后统一发一帧 */
    m_sendTimer.start();
}

void MainWindow::onSendTick()
{
    if (!m_rgbDirty) return;
    m_rgbDirty = false;
    sendRgbNow(m_pendingRgb[0], m_pendingRgb[1], m_pendingRgb[2]);
}

void MainWindow::onSendRgb()
{
    sendRgbNow(m_curRgb[0], m_curRgb[1], m_curRgb[2]);
}

void MainWindow::sendRgbNow(int r, int g, int b)
{
    if (!m_connected) {
        appendLog(QStringLiteral("提示"), QStringLiteral("还没有连接手柄，颜色只保存在界面上"));
        return;
    }

    const QByteArray frame = Proto::makeRgbFrame(r, g, b);
    m_hid.sendOutputReport(frame);

    /* 日志限流：正常 200ms 一条，勾了「记录每一帧」就每帧都记 */
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_logEveryFrame->isChecked() || (now - m_lastOutLogMs) >= 200) {
        m_lastOutLogMs = now;
        appendLog(QStringLiteral("OUT"),
                  QStringLiteral("%1   → R%2 G%3 B%4")
                      .arg(Proto::hexDump(frame)).arg(r).arg(g).arg(b));
    }
}

void MainWindow::onRainbowTick()
{
    m_rainbowHue = (m_rainbowHue + 4) % 360;
    QColor c = QColor::fromHsv(m_rainbowHue, 235, 255);

    /* HSV 的 0~255 映射到固件的 0~100 亮度 */
    const int r = int(c.red() * 100.0 / 255.0);
    const int g = int(c.green() * 100.0 / 255.0);
    const int b = int(c.blue() * 100.0 / 255.0);
    applyColor(r, g, b, true);
}

/* ---------------------------------------------------------------------------
 * 日志
 * ------------------------------------------------------------------------- */
void MainWindow::appendLog(const QString &tag, const QString &text, const QColor &color)
{
    Q_UNUSED(color);
    const QString line = QStringLiteral("%1  [%2] %3")
                             .arg(QDateTime::currentDateTime().toString("HH:mm:ss.zzz"),
                                  tag, text);

    /* 只在内存里攒，80ms 批量塞进控件：一帧一行地 setPlainText 会把界面拖死 */
    m_logBuf.append(line);
    if (m_logBuf.size() > 400) m_logBuf = m_logBuf.mid(m_logBuf.size() - 400);
}

void MainWindow::onLog(const QString &text)
{
    appendLog(QStringLiteral("系统"), text);
}

void MainWindow::onFlushLog()
{
    if (m_logBuf.isEmpty()) return;

    const QStringList lines = m_logBuf;
    m_logBuf.clear();

    if (m_logView->blockCount() > 1200)
        m_logView->clear();

    for (const QString &l : lines)
        m_logView->appendPlainText(l);

    if (!m_pauseLog->isChecked()) {
        QScrollBar *sb = m_logView->verticalScrollBar();
        sb->setValue(sb->maximum());
    }
}

void MainWindow::onClearLog()
{
    m_logBuf.clear();
    m_logView->clear();
}

void MainWindow::onSaveLog()
{
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("保存日志"),
        QStringLiteral("btgamepad_log_%1.txt")
            .arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")),
        QStringLiteral("文本文件 (*.txt)"));
    if (path.isEmpty()) return;

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, QStringLiteral("保存失败"), f.errorString());
        return;
    }
    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);
    ts << m_logView->toPlainText();
    appendLog(QStringLiteral("系统"), QStringLiteral("日志已保存：%1").arg(path));
}

void MainWindow::onClearCounters()
{
    for (KeyIndicator *k : m_keys) k->resetCount();
    m_events.clear();
    m_frames = 0;
    m_lastButtons = 0;
    m_testedButtons = 0;
    m_leftDirs = 0;
    m_rightDirs = 0;
    m_rgbBaseline = m_hid.outOkCount();
    m_performance->resetSession(m_hid.outOkCount());
    m_eventLabel->setText(QStringLiteral("事件：—"));
    updateStats();
    updateAssessment();
}

/* ---------------------------------------------------------------------------
 * 考核助手：仅记录上位机能够证明的结果，不替代现场评分。
 * ------------------------------------------------------------------------- */
void MainWindow::updateAssessment()
{
    if (!m_assessmentBar) return;
    int done = 0;
    if (m_connected) ++done;
    if ((m_testedButtons & 0x03FFu) == 0x03FFu) ++done;
    if ((m_leftDirs & 0x0Fu) == 0x0Fu) ++done;
    if ((m_rightDirs & 0x0Fu) == 0x0Fu) ++done;
    if (m_hid.outOkCount() > m_rgbBaseline) ++done;
    m_assessmentBar->setState(done, 5, m_connected ? QStringLiteral("实时采集中")
                                                   : QStringLiteral("等待连接"));
}

QString MainWindow::assessmentReport() const
{
    const auto mark = [](bool ok) { return ok ? QStringLiteral("[已验证]") : QStringLiteral("[待测试]"); };
    int tested = 0;
    QStringList keyNames;
    for (int i = 0; i < 10; ++i) {
        if ((m_testedButtons & (1u << i)) == 0) continue;
        ++tested;
        keyNames << QString::fromUtf8(Proto::kKeys[i].name);
    }
    const auto dirs = [](quint8 mask) {
        QStringList text;
        if (mask & 0x01) text << QStringLiteral("右");
        if (mask & 0x02) text << QStringLiteral("左");
        if (mask & 0x04) text << QStringLiteral("下");
        if (mask & 0x08) text << QStringLiteral("上");
        return text.isEmpty() ? QStringLiteral("无") : text.join(QStringLiteral("、"));
    };

    QString text;
    QTextStream out(&text);
    out << "BT GAMEPAD 上位机验证报告\n"
        << "生成时间：" << QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss") << "\n"
        << "说明：以下结果来自真实 HID 数据，不等同于考核官最终评分。\n\n"
        << "设备：" << (m_hid.info().product.isEmpty() ? QStringLiteral("MP GAMEPAD") : m_hid.info().product) << "\n"
        << "链路：" << (m_hid.info().bluetooth ? QStringLiteral("蓝牙 HID") : QStringLiteral("USB HID")) << "\n"
        << "设备路径：" << (m_hid.info().path.isEmpty() ? QStringLiteral("未连接") : m_hid.info().path) << "\n\n"
        << mark(m_connected) << " 游戏控制器已连接\n"
        << mark(tested == 10) << " 10 个按键覆盖：" << tested << "/10（" << (keyNames.isEmpty() ? QStringLiteral("无") : keyNames.join("、")) << "）\n"
        << mark((m_leftDirs & 0x0F) == 0x0F) << " 左摇杆四方向：" << dirs(m_leftDirs) << "\n"
        << mark((m_rightDirs & 0x0F) == 0x0F) << " 右摇杆四方向：" << dirs(m_rightDirs) << "\n"
        << mark(m_hid.outOkCount() > m_rgbBaseline) << " RGB 输出报告写入成功\n\n"
        << "输入报告：" << m_frames << " 帧\n"
        << "当前速率：" << QString::number(m_rateHz, 'f', 1) << " Hz\n"
        << "RGB 成功写入：" << m_hid.outOkCount() << " 帧\n"
        << "当前按键：" << Proto::bitmapToText(m_lastButtons) << "\n\n"
        << "仍需现场验证：蓝牙名称与配对、Windows 游戏控制器中的实际表现、掉电后按键自动回连、板载 RGB 实际颜色。\n"
        << "摇杆方向判定阈值：相对中位偏移达到 60%。\n";
    return text;
}

void MainWindow::onShowAssessment()
{
    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("考核验证结果"));
    box.setIcon(QMessageBox::Information);
    box.setText(assessmentReport());
    box.setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    box.exec();
}

void MainWindow::onExportReport()
{
    QString selectedFilter;
    QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出性能与验收报告"),
        QStringLiteral("btgamepad_performance_%1.html")
            .arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")),
        QStringLiteral("HTML 报告 (*.html);;文本报告 (*.txt)"), &selectedFilter);
    if (path.isEmpty()) return;
    const bool html = !path.endsWith(QStringLiteral(".txt"), Qt::CaseInsensitive)
                      && !selectedFilter.startsWith(QStringLiteral("文本"));
    if (!path.endsWith(QStringLiteral(".html"), Qt::CaseInsensitive)
        && !path.endsWith(QStringLiteral(".txt"), Qt::CaseInsensitive))
        path += html ? QStringLiteral(".html") : QStringLiteral(".txt");

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), file.errorString());
        return;
    }
    const QString functional = assessmentReport();
    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    if (html) {
        out << m_performance->reportHtml(
            QStringLiteral("BT GAMEPAD 蓝牙手柄"),
            m_hid.info().product.isEmpty() ? QStringLiteral("MP GAMEPAD") : m_hid.info().product,
            m_connected ? (m_hid.info().bluetooth ? QStringLiteral("蓝牙 HID") : QStringLiteral("USB HID"))
                        : QStringLiteral("未连接"), functional);
    } else {
        out << functional << m_performance->reportText();
    }
    if (!file.flush()) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), file.errorString());
        return;
    }
    file.close();
    appendLog(QStringLiteral("系统"), QStringLiteral("性能与验收报告已导出：%1").arg(path));
    m_statusLabel->setText(QStringLiteral("报告已导出：%1").arg(path));
}

void MainWindow::onStartRgbTest()
{
    if (m_rgbTestTimer.isActive()) {
        m_rgbTestTimer.stop();
        m_assessmentBar->testButton()->setText(QStringLiteral("RGB 通道自检"));
        appendLog(QStringLiteral("系统"), QStringLiteral("RGB 通道自检已停止"));
        return;
    }
    if (!m_connected) {
        QMessageBox::information(this, QStringLiteral("RGB 通道自检"),
                                 QStringLiteral("请先连接 MP GAMEPAD，再执行红、绿、蓝、白、熄灭自检。"));
        return;
    }
    m_rgbTestStep = 0;
    m_assessmentBar->testButton()->setText(QStringLiteral("停止自检"));
    appendLog(QStringLiteral("系统"), QStringLiteral("开始 RGB 通道自检：红 → 绿 → 蓝 → 白 → 熄灭"));
    onRgbTestTick();
    m_rgbTestTimer.start();
}

void MainWindow::onRgbTestTick()
{
    static const int colors[5][3] = {
        { 100, 0, 0 }, { 0, 100, 0 }, { 0, 0, 100 },
        { 100, 100, 100 }, { 0, 0, 0 }
    };
    if (!m_connected || m_rgbTestStep >= 5) {
        m_rgbTestTimer.stop();
        m_assessmentBar->testButton()->setText(QStringLiteral("RGB 通道自检"));
        if (m_rgbTestStep >= 5) appendLog(QStringLiteral("系统"), QStringLiteral("RGB 通道自检完成，请确认板载灯光"));
        return;
    }
    applyColor(colors[m_rgbTestStep][0], colors[m_rgbTestStep][1], colors[m_rgbTestStep][2], true);
    ++m_rgbTestStep;
}

/* ---------------------------------------------------------------------------
 * 配置持久化
 * ------------------------------------------------------------------------- */
void MainWindow::loadSettings()
{
    QSettings s;
    const int r = s.value(QStringLiteral("rgb/r"), 0).toInt();
    const int g = s.value(QStringLiteral("rgb/g"), 0).toInt();
    const int b = s.value(QStringLiteral("rgb/b"), 0).toInt();
    applyColor(r, g, b, false);

    m_liveSend->setChecked(s.value(QStringLiteral("ui/liveSend"), true).toBool());
    m_autoReconnect->setChecked(s.value(QStringLiteral("ui/autoReconnect"), true).toBool());
    m_logEveryFrame->setChecked(s.value(QStringLiteral("ui/logEveryFrame"), false).toBool());

    const QByteArray geo = s.value(QStringLiteral("ui/geometry")).toByteArray();
    if (!geo.isEmpty()) restoreGeometry(geo);
}

void MainWindow::saveSettings()
{
    QSettings s;
    s.setValue(QStringLiteral("rgb/r"), m_curRgb[0]);
    s.setValue(QStringLiteral("rgb/g"), m_curRgb[1]);
    s.setValue(QStringLiteral("rgb/b"), m_curRgb[2]);
    s.setValue(QStringLiteral("ui/liveSend"), m_liveSend->isChecked());
    s.setValue(QStringLiteral("ui/autoReconnect"), m_autoReconnect->isChecked());
    s.setValue(QStringLiteral("ui/logEveryFrame"), m_logEveryFrame->isChecked());
    s.setValue(QStringLiteral("ui/geometry"), saveGeometry());
}

void MainWindow::onConfigRead()
{
    if (!m_connected) return;
    m_configWaitingSave = false;
    m_configPollTimer.stop();
    if (!m_hid.requestFeatureReport()) {
        m_configStatus->setText(QStringLiteral("读取请求入队失败，请重新连接手柄。"));
        return;
    }
    m_configStatus->setText(QStringLiteral("正在读取摇杆配置…"));
}

void MainWindow::onConfigWrite()
{
    if (!m_connected) return;
    QByteArray cfg = Proto::defaultConfig();
    cfg[2] = char(Proto::kConfigCmdSave);
    for (int i = 0; i < 2; ++i) {
        cfg[4 + i] = char(m_sens[i]->value());
        Proto::put16(cfg, 6 + i * 2, quint16(m_deadzone[i]->value()));
    }
    Proto::sealConfig(cfg);
    if (!m_hid.sendFeatureReport(QByteArray(1, char(Proto::kConfigReportId)) + cfg)) {
        m_configStatus->setText(QStringLiteral("写入请求入队失败，请重新连接手柄。"));
        return;
    }
    m_configStatus->setText(QStringLiteral("正在写入配置，随后确认 Flash 保存状态…"));
}

void MainWindow::onConfigDefault()
{
    if (!m_connected) return;
    QByteArray cfg = Proto::defaultConfig();
    cfg[2] = char(Proto::kConfigCmdDefault);
    Proto::sealConfig(cfg);
    if (m_hid.sendFeatureReport(QByteArray(1, char(Proto::kConfigReportId)) + cfg))
        m_configStatus->setText(QStringLiteral("正在恢复默认摇杆参数并保存…"));
    else
        m_configStatus->setText(QStringLiteral("恢复默认请求入队失败，请重新连接手柄。"));
}

void MainWindow::onConfigPoll()
{
    if (!m_configWaitingSave || !m_connected) return;
    if (!m_hid.requestFeatureReport()) {
        m_configWaitingSave = false;
        m_configStatus->setText(QStringLiteral("保存状态读取失败，请手动点击“读取配置”确认。"));
    }
}

void MainWindow::onFeatureResult(bool success, bool write, const QByteArray &data, const QString &error)
{
    m_performance->recordFeature(write, success);
    if (write) {
        if (!success) {
            m_configWaitingSave = false;
            m_configPollTimer.stop();
            m_configStatus->setText(QStringLiteral("配置写入失败：%1").arg(error));
            return;
        }
        m_configWaitingSave = true;
        m_configPolls = 0;
        m_configPollTimer.start();
        return;
    }

    if (!success || data.size() < Proto::kConfigXferSize ||
        quint8(data.at(0)) != Proto::kConfigReportId) {
        if (m_configWaitingSave && ++m_configPolls < 10) {
            m_configPollTimer.start();
            return;
        }
        m_configWaitingSave = false;
        m_configPollTimer.stop();
        m_configStatus->setText(QStringLiteral("配置读取失败：%1").arg(
            error.isEmpty() ? QStringLiteral("特性报告或 Report ID 不匹配") : error));
        return;
    }

    const QByteArray cfg = data.mid(1, Proto::kConfigSize);
    if (!Proto::validConfig(cfg)) {
        m_configWaitingSave = false;
        m_configStatus->setText(QStringLiteral("设备配置校验失败，请确认已烧录支持摇杆配置的新固件。"));
        return;
    }
    const int status = quint8(cfg.at(3));
    if (m_configWaitingSave && status == 1) {
        if (++m_configPolls >= 30) {
            m_configWaitingSave = false;
            m_configStatus->setText(QStringLiteral("等待 Flash 保存超时，请读取配置后重试。"));
        } else {
            m_configPollTimer.start();
        }
        return;
    }
    if (status == 1 && !m_configWaitingSave) {
        m_configStatus->setText(QStringLiteral("设备正在保存配置，请稍候…"));
        return;
    }

    const bool confirmed = m_configWaitingSave;
    m_configWaitingSave = false;
    m_configPollTimer.stop();
    for (int i = 0; i < 2; ++i) {
        m_sens[i]->setValue(quint8(cfg.at(4 + i)));
        m_deadzone[i]->setValue(Proto::get16(cfg, 6 + i * 2));
    }

    if (confirmed) {
        m_configStatus->setText(status == 2
            ? QStringLiteral("已应用并写入 Flash，断电后配置仍会保留。")
            : QStringLiteral("配置已应用，但 Flash 保存失败；可再次点击“写入并保存”。"));
    } else {
        m_configStatus->setText(QStringLiteral("配置读取成功（灵敏度 %1% / %2%，死区 %3 / %4）。")
            .arg(m_sens[0]->value()).arg(m_sens[1]->value())
            .arg(m_deadzone[0]->value()).arg(m_deadzone[1]->value()));
    }
    appendLog(QStringLiteral("CFG"), m_configStatus->text());
}
