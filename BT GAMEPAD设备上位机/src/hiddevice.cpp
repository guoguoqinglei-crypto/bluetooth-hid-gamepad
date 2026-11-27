/* Windows 原生 HID 通信层：SetupAPI 枚举 + hid.dll 属性 + 重叠 I/O 读写。
 * 只有本文件包含 windows.h，界面层完全不碰 Win32 类型。 */

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <setupapi.h>
/* MinGW 的 hidsdi.h / hidpi.h 没有 extern "C" 包装，
 * 不加这一段会按 C++ 名字改写去链接 libhid.a，直接报未定义符号。 */
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}

#include "hiddevice.h"
#include "protocol.h"

#include <thread>

namespace {

/* HID 字符串描述符是 UTF-16LE，长度由设备自己决定。HidD_GetProductString 只保证
 * 写入以 \0 结尾的内容，缓冲区后面的字节可能是残留数据；必须按 NUL 截断，
 * 否则下拉框里会拖出一长串方块字符（整个窗口都会被它撑宽）。
 * 蓝牙手柄的产品名来自远端设备的 SDP 记录，更不可靠。 */
QString wideStr(const wchar_t *w, int maxChars)
{
    if (!w) return QString();

    int n = 0;
    while (n < maxChars && w[n] != L'\0') ++n;

    QString raw = QString::fromWCharArray(w, n);
    raw.remove(QChar(QChar::ReplacementCharacter));
    raw.remove(QChar(0xFFFD));

    /* 只保留可打印字符，去掉描述符里带出来的控制符 */
    QString out;
    out.reserve(raw.size());
    for (const QChar ch : raw) {
        if (ch.isPrint()) out.append(ch);
    }
    return out.simplified().trimmed();
}

QString winError(DWORD code)
{
    wchar_t *msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
    QString s = msg ? QString::fromWCharArray(msg).trimmed()
                    : QString("Win32 error %1").arg(code);
    if (msg) LocalFree(msg);
    return s;
}

/* 用零权限句柄查询属性：蓝牙 HID 设备被系统自己的 HID 栈占着，
 * 用读写权限去问属性容易拿到 ERROR_ACCESS_DENIED，零权限最稳。 */
struct Query {
    bool ok = false;
    quint16 vid = 0, pid = 0, usagePage = 0, usage = 0;
    quint16 inLen = 0, outLen = 0;
    QString product, serial;
};

Query queryDevice(const QString &path)
{
    Query q;
    HANDLE h = CreateFileW(reinterpret_cast<const wchar_t *>(path.utf16()), 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return q;

    HIDD_ATTRIBUTES attr;
    attr.Size = sizeof(attr);
    if (HidD_GetAttributes(h, &attr)) {
        q.vid = quint16(attr.VendorID);
        q.pid = quint16(attr.ProductID);
    }

    PHIDP_PREPARSED_DATA ppd = nullptr;
    if (HidD_GetPreparsedData(h, &ppd) && ppd) {
        HIDP_CAPS caps;
        if (HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS) {
            q.usagePage = quint16(caps.UsagePage);
            q.usage     = quint16(caps.Usage);
            q.inLen     = quint16(caps.InputReportByteLength);
            q.outLen    = quint16(caps.OutputReportByteLength);
        }
        HidD_FreePreparsedData(ppd);
    }

    wchar_t wbuf[256];
    wmemset(wbuf, 0, 256);
    if (HidD_GetProductString(h, wbuf, sizeof(wbuf)))
        q.product = wideStr(wbuf, 256);
    wmemset(wbuf, 0, 256);
    if (HidD_GetSerialNumberString(h, wbuf, sizeof(wbuf)))
        q.serial = wideStr(wbuf, 256);

    CloseHandle(h);
    q.ok = true;
    return q;
}

/* 判断这个接口是蓝牙还是 USB：
 * 蓝牙 HID 的设备路径形如
 *   \\?\HID#{00001124-0000-1000-8000-00805F9B34FB}_VID&00010000_PID&0001\...
 * USB 的是 \\?\HID#VID_0483&PID_5710#... 所以看 BTHENUM 或那个服务 GUID 就行。 */
bool isBluetoothPath(const QString &path)
{
    return path.contains(QStringLiteral("BTHENUM"), Qt::CaseInsensitive) ||
           path.contains(QStringLiteral("00001124-0000-1000-8000-00805F9B34FB"),
                         Qt::CaseInsensitive);
}

} /* namespace */

QString HidDeviceInfo::label() const
{
    QString name = product.isEmpty() ? QStringLiteral("蓝牙手柄") : product;
    /* 产品名可能来自远端设备，限长避免把下拉框/窗口撑宽 */
    if (name.size() > 28) name = name.left(28) + QStringLiteral("...");

    const auto hex4 = [](quint16 v) {
        return QString::number(v, 16).toUpper().rightJustified(4, QChar('0'));
    };
    return QStringLiteral("%1  [%2  IN:%3B OUT:%4B]")
        .arg(name,
             bluetooth ? QStringLiteral("蓝牙") : QStringLiteral("USB"),
             QString::number(inputReportLen),
             QString::number(outputReportLen));
}

HidDevice::HidDevice(QObject *parent) : QObject(parent) {}

HidDevice::~HidDevice() { close(); }

QList<HidDeviceInfo> HidDevice::enumerateGamepads()
{
    QList<HidDeviceInfo> result;

    GUID guid;
    HidD_GetHidGuid(&guid);

    HDEVINFO infoSet = SetupDiGetClassDevsW(&guid, nullptr, nullptr,
                                            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (infoSet == INVALID_HANDLE_VALUE) return result;

    SP_DEVICE_INTERFACE_DATA iface;
    iface.cbSize = sizeof(iface);

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(infoSet, nullptr, &guid, i, &iface); ++i) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(infoSet, &iface, nullptr, 0, &needed, nullptr);
        if (needed == 0) continue;

        QByteArray storage(int(needed), char(0));
        auto *detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(storage.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(infoSet, &iface, detail, needed, nullptr, nullptr))
            continue;

        QString path = QString::fromWCharArray(detail->DevicePath);
        Query q = queryDevice(path);

        /* 一个手柄设备常常有多条接口（游戏控制器 + 电源/电池），
         * 只认主集合：Usage Page 1 / Usage 5（Game Pad），
         * 而且输入报告长度要够 15 字节，否则就是这个设备里别的集合。 */
        if (!q.ok) continue;
        if (q.usagePage != Proto::kUsagePage || q.usage != Proto::kUsage) continue;
        if (q.inLen < Proto::kInputDataLen) continue;

        HidDeviceInfo d;
        d.path = path;
        d.product = q.product;
        d.serial = q.serial;
        d.vid = q.vid;
        d.pid = q.pid;
        d.usagePage = q.usagePage;
        d.usage = q.usage;
        d.inputReportLen = q.inLen ? q.inLen : quint16(Proto::kInputXferLen);
        d.outputReportLen = q.outLen ? q.outLen : quint16(Proto::kOutputXferLen);
        d.bluetooth = isBluetoothPath(path);
        result.append(d);
    }

    SetupDiDestroyDeviceInfoList(infoSet);
    return result;
}

bool HidDevice::open(const HidDeviceInfo &info)
{
    close();
    if (!info.isValid()) return false;

    /* FILE_SHARE_READ|WRITE：手柄同时还会被系统（游戏控制器面板、DirectInput）
     * 打开着，独占打开会被拒绝。不抢独占权。 */
    const wchar_t *path = reinterpret_cast<const wchar_t *>(info.path.utf16());

    /* 读句柄：重叠 I/O，专用于输入报告（权限沿用已验证可用的组合） */
    HANDLE rh = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (rh == INVALID_HANDLE_VALUE) {
        emit logLine(QString("打开设备失败：%1").arg(winError(GetLastError())));
        return false;
    }

    /* 写句柄：独立、同步（不带 FILE_FLAG_OVERLAPPED）。
     * 读写共用同一个重叠句柄时，连接瞬间下发的 SET_REPORT / DATA 会和
     * 刚挂起的 ReadFile 抢同一条 I/O 队列，读线程可能一次报错就退出，
     * 界面从此再也不显示摇杆和按键。两条链路分开后互不干扰。 */
    HANDLE wh = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, 0, nullptr);
    const bool writeAllowed = (wh != INVALID_HANDLE_VALUE);
    if (!writeAllowed) emit logLine(QString("输出句柄打开失败：%1（按键摇杆不受影响）")
                                    .arg(winError(GetLastError())));

    m_handle = rh;
    m_writeHandle = writeAllowed ? wh : nullptr;
    m_info = info;
    m_writeMode = 0;
    m_writeDiagDone = false;
    m_readDiagDone = false;
    m_writeErrors = 0;
    m_outOk.store(0, std::memory_order_relaxed);
    {
        QMutexLocker lock(&m_queueMutex);
        m_queue.clear();
        m_featureQueue.clear();
    }
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_writeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_running = true;
    m_thread = new std::thread(&HidDevice::readLoop, this);
    m_writerThread = new std::thread(&HidDevice::writeLoop, this);

    emit logLine(QString("已连接：%1  IN=%2B OUT=%3B  读写句柄：%4")
                     .arg(info.bluetooth ? QStringLiteral("蓝牙链路") : QStringLiteral("USB 链路"))
                     .arg(info.inputReportLen)
                     .arg(info.outputReportLen)
                     .arg(writeAllowed ? QStringLiteral("已分离") : QStringLiteral("只读")));
    return true;
}

void HidDevice::close()
{
    if (!m_handle) return;

    m_running = false;
    if (m_stopEvent) SetEvent(static_cast<HANDLE>(m_stopEvent));
    if (m_writeEvent) SetEvent(static_cast<HANDLE>(m_writeEvent));

    if (m_thread) {
        auto *t = static_cast<std::thread *>(m_thread);
        if (t->joinable()) t->join();
        delete t;
        m_thread = nullptr;
    }
    if (m_writerThread) {
        auto *t = static_cast<std::thread *>(m_writerThread);
        if (t->joinable()) t->join();
        delete t;
        m_writerThread = nullptr;
    }
    if (m_writeEvent) { CloseHandle(static_cast<HANDLE>(m_writeEvent)); m_writeEvent = nullptr; }
    if (m_stopEvent) { CloseHandle(static_cast<HANDLE>(m_stopEvent)); m_stopEvent = nullptr; }
    if (m_writeHandle) { CloseHandle(static_cast<HANDLE>(m_writeHandle)); m_writeHandle = nullptr; }
    CloseHandle(static_cast<HANDLE>(m_handle));
    m_handle = nullptr;

    QMutexLocker lock(&m_queueMutex);
    m_queue.clear();
    m_featureQueue.clear();
}

void HidDevice::readLoop()
{
    HANDLE h = static_cast<HANDLE>(m_handle);
    HANDLE stop = static_cast<HANDLE>(m_stopEvent);

    /* 蓝牙手柄的报告间隔是 20ms（固件每 20ms 发一帧），
     * 缓冲长度必须严格等于系统算出来的 InputReportByteLength，
     * 少一个字节 ReadFile 就返回 ERROR_INVALID_PARAMETER。 */
    const DWORD len = DWORD(m_info.inputReportLen ? m_info.inputReportLen
                                                  : Proto::kInputXferLen);
    QByteArray buf(int(len), char(0));
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    /* 连接瞬间、蓝牙重新协商时 hidbth 会短暂返回这些错误。
     * 以前一次报错就 break，读线程永久退出，界面从此不再刷新摇杆/按键。 */
    const auto recoverable = [](DWORD e) -> bool {
        return e == ERROR_OPERATION_ABORTED || e == ERROR_INVALID_PARAMETER ||
               e == ERROR_NOT_READY      || e == ERROR_GEN_FAILURE ||
               e == ERROR_ACCESS_DENIED;
    };
    int softErrors = 0;

    while (m_running) {
        OVERLAPPED ov;
        ZeroMemory(&ov, sizeof(ov));
        ov.hEvent = ev;
        ResetEvent(ev);

        DWORD got = 0;
        BOOL ok = ReadFile(h, buf.data(), len, &got, &ov);
        if (!ok) {
            DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) {
                if (m_running && softErrors < 10 && recoverable(err)) {
                    ++softErrors;
                    Sleep(50);
                    continue;
                }
                if (m_running)
                    emit disconnected(QString("读取失败：%1").arg(winError(err)));
                break;
            }
            HANDLE waits[2] = { ev, stop };
            DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (w == WAIT_OBJECT_0 + 1) { CancelIo(h); break; }
            if (w != WAIT_OBJECT_0) break;
            if (!GetOverlappedResult(h, &ov, &got, TRUE)) {
                DWORD e2 = GetLastError();
                if (m_running && softErrors < 10 && recoverable(e2)) {
                    ++softErrors;
                    Sleep(50);
                    continue;
                }
                if (m_running)
                    emit disconnected(QString("读取失败：%1").arg(winError(e2)));
                break;
            }
        }
        if (got > 0) {
            softErrors = 0;
            if (!m_readDiagDone) {
                m_readDiagDone = true;
                emit logLine(QString("[输入] 已收到首帧报告：%1 字节，摇杆/按键开始刷新").arg(got));
            }
            emit reportReceived(QByteArray(buf.constData(), int(got)));
        }
    }

    CloseHandle(ev);
}

int HidDevice::pendingWrites()
{
    QMutexLocker lock(&m_queueMutex);
    return int(m_queue.size());
}

bool HidDevice::sendOutputReport(const QByteArray &frame)
{
    if (!m_writeHandle || frame.isEmpty()) return false;

    {
        QMutexLocker lock(&m_queueMutex);
        /* 连续的 RGB 帧只保留最后一次：彩虹动画一秒最多发 16 帧，
         * 但拖滑块时可能几十次，堆在队列里只会越积越慢。 */
        if (!m_queue.isEmpty() && m_queue.last().size() == frame.size() &&
            m_queue.last().at(0) == frame.at(0))
            m_queue.last() = frame;
        else {
            if (m_queue.size() >= 32) m_queue.removeFirst();
            m_queue.append(frame);
        }
    }
    if (m_writeEvent) SetEvent(static_cast<HANDLE>(m_writeEvent));
    return true;
}

bool HidDevice::requestFeatureReport()
{
    return sendFeatureReport(QByteArray());
}

bool HidDevice::sendFeatureReport(const QByteArray &frame)
{
    if (!m_writeHandle) return false;
    if (!frame.isEmpty() && frame.size() != Proto::kConfigXferSize) return false;
    {
        QMutexLocker lock(&m_queueMutex);
        if (m_featureQueue.size() >= 8) return false;
        m_featureQueue.append(frame);
    }
    if (m_writeEvent) SetEvent(static_cast<HANDLE>(m_writeEvent));
    return true;
}

/* 写线程：真正的蓝牙传输都在这里做，GUI 线程只负责入队 */
void HidDevice::writeLoop()
{
    HANDLE ev = static_cast<HANDLE>(m_writeEvent);

    while (m_running) {
        QByteArray feature;
        bool hasFeature = false;
        {
            QMutexLocker lock(&m_queueMutex);
            if (!m_featureQueue.isEmpty()) {
                feature = m_featureQueue.takeFirst();
                hasFeature = true;
            }
        }
        if (hasFeature) {
            HANDLE h = static_cast<HANDLE>(m_writeHandle);
            const bool write = !feature.isEmpty();
            QByteArray buf = write ? feature : QByteArray(Proto::kConfigXferSize, char(0));
            if (!write) buf[0] = char(Proto::kConfigReportId);
            const BOOL ok = write
                ? HidD_SetFeature(h, reinterpret_cast<PVOID>(buf.data()), Proto::kConfigXferSize)
                : HidD_GetFeature(h, reinterpret_cast<PVOID>(buf.data()), Proto::kConfigXferSize);
            const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
            emit featureResult(ok != 0, write, write ? QByteArray() : buf,
                               ok ? QString() : winError(error));
            continue;
        }

        QByteArray frame;
        {
            QMutexLocker lock(&m_queueMutex);
            if (!m_queue.isEmpty()) frame = m_queue.takeFirst();
        }
        if (frame.isEmpty()) {
            if (ev) WaitForSingleObject(ev, 200);
            else Sleep(20);
            continue;
        }
        if (writeRaw(frame)) {
            /* 只有这里才算"真的送到设备"：RGB 显示用它，不是入队数 */
            m_outOk.fetch_add(1, std::memory_order_relaxed);
        } else {
            ++m_writeErrors;
            /* 失败日志限流：前 3 次 + 之后每 100 次记一条，避免刷屏 */
            if (m_writeErrors <= 3 || (m_writeErrors % 100) == 0)
                emit logLine(QString("发送失败（累计 %1 次）").arg(m_writeErrors));
        }
    }
}

bool HidDevice::writeRaw(const QByteArray &frame)
{
    HANDLE h = static_cast<HANDLE>(m_writeHandle);
    if (!h) return false;

    const ULONG need = ULONG(m_info.outputReportLen ? m_info.outputReportLen : frame.size());
    QByteArray buf = frame;
    if (buf.size() < int(need)) buf.append(QByteArray(int(need) - buf.size(), char(0)));
    if (buf.size() > int(need)) buf.truncate(int(need));

    /* 只走控制通道 SET_REPORT(0x52)，不再周期性写 Interrupt 通道。
     *
     * 实测结论（D:/minzhan/_rgbdiag/rainbowtest*.c，4 秒一组数输入帧）：
     *   不下发              201 帧  最大间隔 32ms   0 次卡顿
     *   控制通道 60ms       200 帧  最大间隔 32ms   0 次卡顿
     *   中断通道 60ms        23 帧  最大间隔 968ms  4 次卡顿
     *   中断通道 100~1000ms  21~59 帧 最大间隔 ~900ms 每轮必卡
     *
     * 原因：Windows hidbth 在同一条 L2CAP HID Interrupt 通道上做输出写时，
     * 会把该通道的输入报告挂起约 900ms。彩虹动画每 60ms 一帧，等于持续
     * 把摇杆/按键掐断，界面心跳还会误报"链路超时"。控制通道是独立的
     * L2CAP 通道，写它对输入毫无影响。
     *
     * 固件侧 hidp_control_rsp() 已在 txn=5 / param=2 分支解析 0x52 帧，
     * 控制通道现在能真正点亮 RGB（旧固件会把它当 SET_PROTOCOL 丢掉）。 */
    SetLastError(0);
    const bool viaControl = HidD_SetOutputReport(h, reinterpret_cast<PVOID>(buf.data()), need);
    const DWORD ctlErr = GetLastError();

    /* 仅在控制通道 API 本身失败时才退回中断通道兜底：这是极少数驱动异常，
     * 单次写不会造成持续的输入挂起。 */
    bool viaInterrupt = false;
    DWORD intErr = 0;
    if (!viaControl) {
        DWORD written = 0;
        BOOL ok = WriteFile(h, buf.data(), need, &written, nullptr);
        intErr = GetLastError();
        viaInterrupt = bool(ok && written == need);
    }

    /* 首帧打一条诊断，界面日志里能直接看出走的是哪条通道 */
    if (!m_writeDiagDone) {
        m_writeDiagDone = true;
        QString hex;
        for (int i = 0; i < buf.size(); ++i)
            hex += QString("%1 ").arg(quint8(buf.at(i)), 2, 16, QChar('0')).toUpper();
        emit logLine(QStringLiteral("[写入] 帧=%1 长度=%2  → 控制通道:%3%4")
                     .arg(hex.trimmed(), QString::number(need),
                          viaControl ? QStringLiteral("OK（输入不受影响）")
                                     : QString("失败(%1)").arg(ctlErr),
                          (!viaControl && viaInterrupt)
                              ? QString(" 已回退中断通道(失败 %1)").arg(intErr)
                              : QStringLiteral("")));
        if (!viaControl && !viaInterrupt)
            emit logLine(QStringLiteral("提示：两条通道都失败，确认板子已烧录支持控制通道 RGB 的固件"));
    }

    if (viaControl)   m_writeMode = 1;
    if (viaInterrupt) m_writeMode = 2;

    return viaControl || viaInterrupt;
}
