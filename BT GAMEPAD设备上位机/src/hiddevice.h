#ifndef HIDDEVICE_H
#define HIDDEVICE_H

/* Windows 原生 HID 层（SetupAPI + hid.dll），不引第三方库。
 * 这里故意不包含 windows.h，避免 min/max 宏污染；
 * 系统句柄以 void* 保存，在 .cpp 里再转回去。
 *
 * 线程模型：一个读线程（阻塞式重叠 ReadFile）+ 一个写线程。
 * sendOutputReport() 只负责入队，界面线程永远不会卡在蓝牙传输里。 */

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QList>
#include <QMutex>
#include <atomic>

struct HidDeviceInfo
{
    QString path;
    QString product;
    QString serial;
    quint16 vid = 0;
    quint16 pid = 0;
    quint16 usagePage = 0;
    quint16 usage = 0;
    quint16 inputReportLen = 0;
    quint16 outputReportLen = 0;
    bool    bluetooth = false;      /* 设备路径里带 BTHENUM 或蓝牙 HID GUID */

    bool isValid() const { return !path.isEmpty(); }
    QString label() const;
};

class HidDevice : public QObject
{
    Q_OBJECT
public:
    explicit HidDevice(QObject *parent = nullptr);
    ~HidDevice() override;

    /* 只列出"游戏控制器"接口（Usage Page 0x01 / Usage 0x05）。
     * 蓝牙手柄在 Windows 里就是一个 HID 设备，和 USB 手柄没有区别，
     * 所以枚举逻辑与 USB 版完全一致，只有筛选条件不同。 */
    static QList<HidDeviceInfo> enumerateGamepads();

    bool open(const HidDeviceInfo &info);
    void close();
    bool isOpen() const { return m_handle != nullptr; }
    const HidDeviceInfo &info() const { return m_info; }

    /* 非阻塞：帧进队列，由写线程发出。
     * 队列里连续出现的 RGB 帧只保留最后一帧，拖滑块/彩虹动画不会把
     * 蓝牙链路塞满（一次输出报告要占用一个 ACL 包的时隙）。 */
    bool sendOutputReport(const QByteArray &frame);

    /* 配置特性报告也在写线程执行；空帧表示读取，非空帧表示写入。 */
    bool requestFeatureReport();
    bool sendFeatureReport(const QByteArray &frame);

    int pendingWrites();

    /* 真正被设备接受的输出报告数，由写线程统计。
     * 旧版在入队时就 +1，写失败也照样往上涨，看着像发了其实一帧都没到，
     * 排查 RGB 灯不亮时会被这个数字误导。 */
    quint64 outOkCount() const { return m_outOk.load(std::memory_order_relaxed); }

signals:
    void reportReceived(const QByteArray &raw);
    void disconnected(const QString &reason);
    void logLine(const QString &text);
    void featureResult(bool success, bool write, const QByteArray &data, const QString &error);

private:
    void readLoop();
    void writeLoop();
    bool writeRaw(const QByteArray &frame);

    void *m_handle = nullptr;       /* 读句柄：重叠 I/O，专用于输入报告 */
    void *m_writeHandle = nullptr;  /* 写句柄：独立同步句柄，专用于输出报告 */
    void *m_stopEvent = nullptr;
    void *m_thread = nullptr;
    void *m_writeEvent = nullptr;
    void *m_writerThread = nullptr;
    HidDeviceInfo m_info;
    volatile bool m_running = false;

    QMutex m_queueMutex;
    QList<QByteArray> m_queue;
    QList<QByteArray> m_featureQueue;
    int     m_writeMode = 0;   /* 0 = 未确定，1 = SET_REPORT（控制通道），2 = 中断 OUT */
    bool    m_writeDiagDone = false;  /* 每次连接只打一条写入诊断，避免刷屏 */
    bool    m_readDiagDone  = false;  /* 每次连接只打一条输入诊断 */
    quint64 m_writeErrors = 0;
    std::atomic<quint64> m_outOk{0};
};

#endif /* HIDDEVICE_H */
