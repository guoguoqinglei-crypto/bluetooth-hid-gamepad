#ifndef PROTOCOL_H
#define PROTOCOL_H

/* ============================================================================
 * 蓝牙手柄 HID 协议定义（与固件 hardware/bt_sdp_record.h 的报告描述符一一对应）
 * ----------------------------------------------------------------------------
 * 设备：蓝牙经典 HID 手柄，广播名 "MP GAMEPAD"
 *       Usage Page 0x0001（Generic Desktop）/ Usage 0x0005（Game Pad）
 *
 * 【输入报告】Report ID 1，数据 15 字节
 *     [0..1]   X    16 位，左摇杆 X，0x0000~0xFFFF，中位 0x8000
 *     [2..3]   Y    16 位，左摇杆 Y
 *     [4..5]   Z    16 位，右摇杆 X
 *     [6..7]   Rz   16 位，右摇杆 Y
 *     [8..9]   LT   10 位 + 6 位填充（本板无模拟扳机，恒 0）
 *     [10..11] RT   10 位 + 6 位填充（同上）
 *     [12]     Hat  4 位 + 4 位填充（本板不用方向帽，恒 0）
 *     [13..14] 按键 10 位，bit0 = 按键 1 … bit9 = 按键 10
 *
 *   注意极性：摇杆是 ADC 直采，数值越大偏一侧；按键是上拉输入，按下为 0，
 *   固件组装报告时已经取反，所以上位机看到的位图 1 = 按下，与普通手柄一致。
 *
 * 【输出报告】Report ID 3，数据 8 字节（复用描述符里的 PID Set Effect 报告）
 *     [0]   命令标记，固定 0x01（用来和系统自己发的震动报告区分）
 *     [1]   R   0~100
 *     [2]   G   0~100
 *     [3]   B   0~100
 *     [4..7] 未用，填 0
 *
 *   Windows 的 ReadFile / WriteFile / HidD_SetOutputReport 缓冲区首字节固定是
 *   Report ID，所以实际传输长度是 16 字节（输入）和 9 字节（输出）。
 * ========================================================================== */

#include <QtGlobal>
#include <QString>
#include <QByteArray>
#include <QStringList>
#include <QList>

namespace Proto
{
/* 设备身份：蓝牙 HID 由 Usage Page / Usage 识别。
 * 经典蓝牙手柄的 VID/PID 来自 SDP 里的 PnP Information 记录，多数是
 * 0x0000 或 0x0001，不能拿来做筛选条件，只能作为显示信息。 */
constexpr quint16 kUsagePage = 0x0001;
constexpr quint16 kUsage     = 0x0005;

/* 报告长度 */
constexpr int kInputReportId  = 1;
constexpr int kInputDataLen   = 15;
constexpr int kInputXferLen   = kInputDataLen + 1;   /* 16：含 Report ID */
constexpr int kOutputReportId = 3;
constexpr int kOutputDataLen  = 8;
constexpr int kOutputXferLen  = kOutputDataLen + 1;  /* 9：含 Report ID */

/* 特性报告 Report ID 5：16 字节摇杆配置，蓝牙 HID Control 通道读写。 */
constexpr int kConfigReportId = 5;
constexpr int kConfigSize = 16;
constexpr int kConfigXferSize = kConfigSize + 1;
constexpr quint8 kConfigMagic = 0x47;
constexpr quint8 kConfigVersion = 1;
constexpr quint8 kConfigCmdSave = 1;
constexpr quint8 kConfigCmdDefault = 2;

inline quint16 get16(const QByteArray &data, int at)
{
    return quint16(quint8(data.at(at)) | (quint8(data.at(at + 1)) << 8));
}
inline void put16(QByteArray &data, int at, quint16 value)
{
    data[at] = char(value & 0xFFu); data[at + 1] = char(value >> 8);
}
inline quint32 get32(const QByteArray &data, int at)
{
    quint32 value = 0;
    for (int i = 0; i < 4; ++i) value |= quint32(quint8(data.at(at + i))) << (i * 8);
    return value;
}
inline void put32(QByteArray &data, int at, quint32 value)
{
    for (int i = 0; i < 4; ++i) data[at + i] = char((value >> (i * 8)) & 0xFFu);
}
inline quint32 configCrc(const QByteArray &data)
{
    quint32 crc = 0xFFFFFFFFu;
    for (int i = 0; i < 12; ++i) {
        crc ^= quint8(data.at(i));
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
    }
    return ~crc;
}
inline void sealConfig(QByteArray &data)
{
    put32(data, 12, 0); put32(data, 12, configCrc(data));
}
inline QByteArray defaultConfig()
{
    QByteArray data(kConfigSize, char(0));
    data[0] = char(kConfigMagic); data[1] = char(kConfigVersion);
    data[4] = char(100); data[5] = char(100);
    sealConfig(data);
    return data;
}
inline bool validConfig(const QByteArray &data)
{
    return data.size() >= kConfigSize && quint8(data.at(0)) == kConfigMagic &&
           quint8(data.at(1)) == kConfigVersion && get32(data, 12) == configCrc(data) &&
           quint8(data.at(4)) >= 25 && quint8(data.at(4)) <= 200 &&
           quint8(data.at(5)) >= 25 && quint8(data.at(5)) <= 200 &&
           get16(data, 6) <= 24000 && get16(data, 8) <= 24000;
}

/* 输出报告里的字段下标（含 Report ID 的整帧坐标） */
constexpr int kOutIdxMark = 1;   /* 0x01 标记 */
constexpr int kOutIdxR    = 2;
constexpr int kOutIdxG    = 3;
constexpr int kOutIdxB    = 4;
constexpr quint8 kMarkByte = 0x01;

/* 扳机与方向帽（本板硬件没有，留常量方便以后换板子） */
constexpr int kIdxLT  = 8;
constexpr int kIdxRT  = 10;
constexpr int kIdxHat = 12;
constexpr int kIdxBtn = 13;

/* 10 个按键：报告描述符里 Button 1~10，
 * 对应板载 A B X Y UP DN LF RG，加上两个摇杆按下 L3 R3。 */
struct KeyDef
{
    const char *name;      /* 板载丝印 / 手柄键名 */
    const char *function;  /* 在游戏控制器里的含义 */
};

inline const KeyDef kKeys[10] = {
    { "A",  "按键 1"   },   /* bit0 */
    { "B",  "按键 2"   },   /* bit1 */
    { "X",  "按键 3"   },   /* bit2 */
    { "Y",  "按键 4"   },   /* bit3 */
    { "UP", "按键 5"   },   /* bit4 */
    { "DN", "按键 6"   },   /* bit5 */
    { "LF", "按键 7"   },   /* bit6 */
    { "RG", "按键 8"   },   /* bit7 */
    { "L3", "左摇杆按下" },  /* bit8 */
    { "R3", "右摇杆按下" },  /* bit9 */
};

/* 一根摇杆的一次采样。raw 是 0~65535 的原始值，中位 0x8000。 */
struct Stick
{
    quint16 x = 0x8000;
    quint16 y = 0x8000;
};

struct Report
{
    Stick   left;
    Stick   right;
    quint16 lt = 0;
    quint16 rt = 0;
    quint8  hat = 0;
    quint16 buttons = 0;
};

inline quint16 rd16(const QByteArray &ba, int off)
{
    return quint16(quint8(ba.at(off)) | (quint8(ba.at(off + 1)) << 8));
}

/* 把 Windows 读到的一帧解析成报告体。
 * 正常情况首字节是 Report ID（1），某些驱动/抓包工具会把它剥掉，
 * 所以这里按长度判断，两种都吃。 */
inline bool parseReport(const QByteArray &raw, Report *out)
{
    if (!out) return false;

    int base = 0;
    if (raw.size() >= kInputXferLen && quint8(raw.at(0)) == kInputReportId)
        base = 1;                                  /* 有 Report ID */
    if (raw.size() - base < kInputDataLen)
        return false;

    out->left.x  = rd16(raw, base + 0);
    out->left.y  = rd16(raw, base + 2);
    out->right.x = rd16(raw, base + 4);
    out->right.y = rd16(raw, base + 6);
    out->lt      = quint16(rd16(raw, base + kIdxLT) & 0x03FF);
    out->rt      = quint16(rd16(raw, base + kIdxRT) & 0x03FF);
    out->hat     = quint8(quint8(raw.at(base + kIdxHat)) & 0x0F);
    out->buttons = quint16(quint8(raw.at(base + kIdxBtn)) |
                           ((quint8(raw.at(base + kIdxBtn + 1)) & 0x03) << 8));
    return true;
}

/* 构造一帧 9 字节的下行数据（首字节是 Report ID） */
inline QByteArray makeRgbFrame(int r, int g, int b)
{
    auto clamp = [](int v) -> quint8 {
        return quint8(v < 0 ? 0 : (v > 100 ? 100 : v));
    };

    QByteArray f(kOutputXferLen, char(0));
    f[0] = char(kOutputReportId);
    f[kOutIdxMark] = char(kMarkByte);
    f[kOutIdxR] = char(clamp(r));
    f[kOutIdxG] = char(clamp(g));
    f[kOutIdxB] = char(clamp(b));
    return f;
}

/* 按键位图 -> 按下的按键下标列表 */
inline QList<int> bitmapToKeys(quint16 bm)
{
    QList<int> out;
    for (int i = 0; i < 10; ++i)
        if ((bm & (1u << i)) != 0u) out.append(i);
    return out;
}

/* 位图 -> 可读文字，例如 "A + UP" */
inline QString bitmapToText(quint16 bm)
{
    QStringList names;
    const QList<int> keys = bitmapToKeys(bm);
    for (int i : keys)
        names.append(QString::fromUtf8(kKeys[i].name));
    if (names.isEmpty())
        return QString::fromUtf8("（无按键）");
    return names.join(QString::fromUtf8(" + "));
}

/* 摇杆原始值 -> -100..+100 的百分比（值大对应坐标增大；Y 轴向下为正） */
inline int stickPercent(quint16 raw)
{
    int v = int(raw) - 0x8000;
    int pct = (v * 100) / 0x8000;
    if (pct > 100) pct = 100;
    if (pct < -100) pct = -100;
    return pct;
}

/* 摇杆原始值是否已经明显离开中位（用来提示"回中"是否正常） */
inline bool stickAtCenter(quint16 raw)
{
    return (raw > 0x8000 - 0x0C00) && (raw < 0x8000 + 0x0C00);
}

/* 把字节数组格式化成 "01 64 32 00 ..." */
inline QString hexDump(const QByteArray &ba)
{
    QStringList parts;
    parts.reserve(ba.size());
    for (int i = 0; i < ba.size(); ++i)
        parts << QString("%1").arg(quint8(ba.at(i)), 2, 16, QChar('0')).toUpper();
    return parts.join(QLatin1Char(' '));
}

} /* namespace Proto */

#endif /* PROTOCOL_H */
