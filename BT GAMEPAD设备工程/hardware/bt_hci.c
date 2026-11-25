/* H4 传输层：串口字节流 -> HCI 包 -> 分发。
 * 包类型 CMD 0x01 / ACL 0x02 / SCO 0x03 / EVT 0x04；长度字段不含自身。
 * 中断只搬字节，拼包/解析/发命令都在FreeRTOS 蓝牙任务上下文。 */
#include <bt_hci.h>

#include <string.h>
#include <stm32f10x.h>

#include <bt_uart.h>
#include <uart.h>
#include <delay.h>

/* ==========================================================================
 * 全局运行时状态
 * ========================================================================== */
volatile uint8_t  g_bt_state   = BT_STATE_IDLE;
volatile uint32_t g_bt_reports = 0u;
volatile uint8_t  g_bt_error   = 0u;

uint16_t g_conn_handle = BT_HANDLE_NONE;
uint8_t  g_peer_addr[6] = {0};
uint8_t  g_local_addr[6] = {0};

uint16_t g_acl_mtu  = 128u;
uint16_t g_acl_pkts = 4u;
volatile uint16_t g_cmd_credits = 1u;
volatile uint16_t g_acl_credits = 4u;

uint8_t g_link_key[16] = {0};
uint8_t g_link_key_valid = 0u;

volatile uint32_t g_evt_count = 0u;

/* 1 = 本次连接是本机主动发起的（回连） */
uint8_t g_conn_by_us = 0u;

bt_chan_t g_chan[BT_CHAN_COUNT];

/* ==========================================================================
 * H4 收包状态机
 * ========================================================================== */
static uint8_t  s_rx[BT_RX_BUF_SIZE];
static uint16_t s_rxLen   = 0u;   /* 已收字节数 */
static uint16_t s_rxNeed  = 0u;   /* 当前阶段目标字节数 */
static uint8_t  s_rxHdr   = 0u;   /* 该类型的头长度（不含类型字节） */
static uint8_t  s_rxType  = 0u;
static uint8_t  s_rxPhase = 0u;   /* 0=等类型 1=收头 2=收体 */

/* 发送缓冲：静态区，主栈只有 1KB，不能放局部数组 */
static uint8_t  s_tx[BT_TX_ACL_MAX + 8u];

/* ==========================================================================
 * 归还控制器→主机的 ACL 信用
 *
 * 【这是本工程最关键的一处修复】
 *   初始化时我们发了 Host Buffer Size，等于告诉控制器"我这边有 8 个接收缓冲"。
 *   控制器每发一个 ACL 包就消耗一个，**必须由主机用
 *   Host Number Of Completed Packets 归还**，否则 8 个用完之后
 *   控制器就停止向主机发任何 ACL 数据 —— 而它自己并不知道出错了。
 *
 *   实测症状：SDP 的 5 个包 + HID Control 的 3 个包 = 正好 8 个，
 *   之后 Windows 请求打开 HID Interrupt 的 CONN_REQ 永远到不了主机，
 *   双方干等，最后 LINK SUPERVISION TIMEOUT 断链。
 *
 *   所以每收一个 ACL 包就记一笔，poll 结束时一次性还掉。
 * ========================================================================== */
static uint16_t s_aclRxPending = 0u;

static void bt_return_acl_credits(void)
{
    uint8_t p[4];

    if (s_aclRxPending == 0u)
    {
        return;
    }

    if (g_conn_handle == BT_HANDLE_NONE)
    {
        s_aclRxPending = 0u;
        return;
    }

    /* ★ 命令信用为 0 时不要发。
     * 控制器用 Num_HCI_Command_Packets 告诉我们"还能收几条命令"，
     * 信用耗尽的返回值就是 0x0C(Command Disallowed) —— 讲义第 49 页
     * 的排错表把"命令返回 0x0C"直接指向"命令信用是否耗尽"。
     * 这里没信用就先把计数留着，下一轮 poll 再还，不会丢。 */
    if (g_cmd_credits == 0u)
    {
        return;
    }

    p[0] = 1u;                                        /* Num_Handles */
    p[1] = (uint8_t)(g_conn_handle & 0xFFu);
    p[2] = (uint8_t)(g_conn_handle >> 8);
    p[3] = (uint8_t)((s_aclRxPending > 255u) ? 255u : s_aclRxPending);

    bt_send_cmd(BT_OP_Host_Num_Completed_Packets, p, 4u);

    s_aclRxPending = 0u;
}

/* ==========================================================================
 * 原始 HCI 抓包输出（给 Actions SerialUtility 解析用）
 *
 * 【为什么要这个模式】
 *   Actions 串口工具能把 HCI 报文解析成"命令名 + 参数 + 事件名"，
 *   但它只认特定格式的行：**主机发出以 TX: 开头，控制器发来以 RX: 开头，
 *   后面跟空格分隔的十六进制字节**（见配套的 readme.txt）。
 *
 *   打开这个开关后会往串口输出：
 *     TX: 01 03 0C 00
 *     RX: 04 0E 04 01 03 0C 00
 *   把这份日志用 SerialUtility 打开，就能直接看到每条命令是干什么的、
 *   每个事件是什么含义，不用再手工对着规范查。
 *
 * ⚠ 数据量很大（一个 653 字节的 SDP 响应会变成 2000 多字符），
 *   只在排错时打开，平时关掉。
 * ========================================================================== */
#define BT_HCI_TRACE   1

#if BT_HCI_TRACE
static void bt_trace(const char *tag, const uint8_t *d, uint16_t n)
{
    static const char hexd[] = "0123456789ABCDEF";
    static char line[2400];
    uint16_t i;
    uint16_t p = 0u;

    while ((*tag != '\0') && (p < 4u))
    {
        line[p++] = *tag++;
    }

    for (i = 0u; (i < n) && ((uint16_t)(p + 4u) < (uint16_t)sizeof(line)); i++)
    {
        line[p++] = ' ';
        line[p++] = hexd[d[i] >> 4];
        line[p++] = hexd[d[i] & 0x0Fu];
    }
    line[p] = '\0';

    UART1_SendString(line);
    UART1_SendString("\r\n");
}
#define BT_TRACE_TX(d, n)   bt_trace("TX:", (d), (n))
#define BT_TRACE_RX(d, n)   bt_trace("RX:", (d), (n))
#else
#define BT_TRACE_TX(d, n)   do { } while (0)
#define BT_TRACE_RX(d, n)   do { } while (0)
#endif

/* 把某个类型对应的头长度填出来 */
static uint8_t h4_hdr_len(uint8_t type)
{
    switch (type)
    {
        case H4_TYPE_CMD:   return 3u;   /* Opcode(2) + Plen(1) */
        case H4_TYPE_ACL:   return 4u;   /* Handle(2) + Dlen(2) */
        case H4_TYPE_SCO:   return 3u;   /* Handle(2) + Dlen(1) */
        case H4_TYPE_EVENT: return 2u;   /* EvtCode(1) + Plen(1) */
        default:            return 0u;
    }
}

/* 从已收到的头里算出负载长度 */
static uint16_t h4_payload_len(uint8_t type, const uint8_t *p)
{
    switch (type)
    {
        case H4_TYPE_CMD:   return (uint16_t)p[2];                        /* p[0..1]=opcode */
        case H4_TYPE_ACL:   return (uint16_t)(p[2] | ((uint16_t)p[3] << 8));
        case H4_TYPE_SCO:   return (uint16_t)p[2];
        case H4_TYPE_EVENT: return (uint16_t)p[1];                        /* p[0]=事件码 */
        default:            return 0u;
    }
}

/* 一个完整包到手，按类型分发 */
static void h4_dispatch(void)
{
    uint8_t *body = &s_rx[1];           /* 跳过 H4 类型字节 */

    /* 整包原样打出来（含 H4 类型字节），供 Actions SerialUtility 解析 */
    BT_TRACE_RX(s_rx, s_rxLen);

    switch (s_rxType)
    {
        case H4_TYPE_EVENT:
            /* body[0] = 事件码，body[1] = 参数长度，body[2..] = 参数 */
            bt_evt_dispatch(body, (uint8_t)(s_rxLen - 1u));
            break;

        case H4_TYPE_ACL:
            /* 每收一个 ACL 包都要记账，poll 结束时还给控制器 */
            s_aclRxPending++;
            bt_acl_input(body, (uint16_t)(s_rxLen - 1u));
            break;

        default:
            /* 控制器不会给我们发命令/SCO，忽略即可 */
            break;
    }
}

void bt_transport_reset(void)
{
    s_rxLen   = 0u;
    s_rxNeed  = 0u;
    s_rxHdr   = 0u;
    s_rxPhase = 0u;
    s_rxType  = 0u;
}

void bt_transport_poll(void)
{
    uint8_t b;

    while (BtUart_TryRead(&b) != 0u)
    {
        if (s_rxPhase == 0u)
        {
            /* 等一个合法的 H4 类型字节。噪声直接丢掉，
             * 比"收到就当真"强得多——波特率切错时会有一堆乱码 */
            if (h4_hdr_len(b) == 0u)
            {
                continue;
            }
            s_rxType  = b;
            s_rxHdr   = h4_hdr_len(b);
            s_rx[0]   = b;
            s_rxLen   = 1u;
            s_rxNeed  = (uint16_t)(1u + s_rxHdr);
            s_rxPhase = 1u;
            continue;
        }

        if (s_rxLen >= BT_RX_BUF_SIZE)
        {
            /* 超长包，丢掉重来，避免踩内存 */
            bt_transport_reset();
            continue;
        }

        s_rx[s_rxLen++] = b;

        if (s_rxPhase == 1u && s_rxLen >= s_rxNeed)
        {
            uint16_t payload = h4_payload_len(s_rxType, &s_rx[1]);

            if ((uint32_t)payload + 1u + s_rxHdr > BT_RX_BUF_SIZE)
            {
                bt_transport_reset();       /* 头里声明的长度不合理 */
                continue;
            }
            s_rxNeed  = (uint16_t)(1u + s_rxHdr + payload);
            s_rxPhase = 2u;

            if (s_rxLen >= s_rxNeed)
            {
                h4_dispatch();
                bt_transport_reset();
            }
            continue;
        }

        if (s_rxPhase == 2u && s_rxLen >= s_rxNeed)
        {
            h4_dispatch();
            bt_transport_reset();
        }
    }

    /* 本轮收到的 ACL 包，统一归还信用给控制器。
     * 不还的话控制器发满 8 个（Host Buffer Size 声明的数量）就彻底停发，
     * 现象是"通道建到一半，双方都不说话，最后链路超时"。 */
    bt_return_acl_credits();
}

/* ==========================================================================
 * 发送：命令
 * ========================================================================== */
void bt_send_cmd(uint16_t opcode, const uint8_t *params, uint8_t plen)
{
    s_tx[0] = H4_TYPE_CMD;
    s_tx[1] = (uint8_t)(opcode & 0xFFu);
    s_tx[2] = (uint8_t)(opcode >> 8);
    s_tx[3] = plen;

    if ((plen != 0u) && (params != 0))
    {
        memcpy(&s_tx[4], params, plen);
    }

    if (g_cmd_credits > 0u)
    {
        g_cmd_credits--;
    }

    BT_TRACE_TX(s_tx, (uint16_t)(4u + plen));
    BtUart_Send(s_tx, (uint16_t)(4u + plen));
}

/* ==========================================================================
 * 发送：ACL（L2CAP 数据）
 *
 * 一帧 L2CAP 可能比分片上限长（SDP 响应 653 字节 vs 控制器常见 1021，
 * 但也可能是 128）。超长必须按 ACL 分片发：
 *   第 1 片 PB = 0b10（L2CAP PDU 起始）
 *   后续片 PB = 0b01（续片）
 * 少发或不带 PB 标志，对端会一直等续包，永远不回应。
 * ========================================================================== */
static uint8_t s_frame[4u + BT_TX_ACL_MAX];

void bt_send_acl(uint16_t handle, uint16_t cid,
                 const uint8_t *payload, uint16_t len)
{
    uint16_t total;
    uint16_t off;
    uint16_t fragMax;
    uint8_t  first = 1u;

    if (len > BT_TX_ACL_MAX)
    {
        return;                              /* 超长直接拒绝，绝不截断后照发 */
    }

    /* 先拼一帧完整的 L2CAP：Length(2) + CID(2) + 负载 */
    total = (uint16_t)(4u + len);
    s_frame[0] = (uint8_t)(len & 0xFFu);
    s_frame[1] = (uint8_t)(len >> 8);
    s_frame[2] = (uint8_t)(cid & 0xFFu);
    s_frame[3] = (uint8_t)(cid >> 8);
    if (len != 0u)
    {
        memcpy(&s_frame[4], payload, len);
    }

    /* 每片最多塞 g_acl_mtu 字节；拿不到有效值就按 128 保守分片 */
    fragMax = (g_acl_mtu >= 32u) ? g_acl_mtu : 128u;
    if (fragMax > (uint16_t)sizeof(s_tx) - 5u)
    {
        fragMax = (uint16_t)(sizeof(s_tx) - 5u);
    }

    off = 0u;
    while (off < total)
    {
        uint16_t n = (uint16_t)(total - off);
        uint16_t h;

        if (n > fragMax)
        {
            n = fragMax;
        }

        s_tx[0] = H4_TYPE_ACL;

        h = (uint16_t)(handle & 0x0FFFu);
        h = (uint16_t)(h | (first != 0u ? 0x2000u : 0x1000u));
        s_tx[1] = (uint8_t)(h & 0xFFu);
        s_tx[2] = (uint8_t)(h >> 8);

        s_tx[3] = (uint8_t)(n & 0xFFu);
        s_tx[4] = (uint8_t)(n >> 8);

        memcpy(&s_tx[5], &s_frame[off], n);

        BT_TRACE_TX(s_tx, (uint16_t)(5u + n));
        BtUart_Send(s_tx, (uint16_t)(5u + n));

        off = (uint16_t)(off + n);
        first = 0u;
    }

    if (g_acl_credits > 0u)
    {
        g_acl_credits--;
    }
}

/* 主机侧主动归还 ACL 信用（Host Number Of Completed Packets） */
void bt_flush_acl_credits(void)
{
    uint8_t p[4];

    if (g_conn_handle == BT_HANDLE_NONE)
    {
        return;
    }

    p[0] = 1u;                                        /* Num_Handles */
    p[1] = (uint8_t)(g_conn_handle & 0xFFu);
    p[2] = (uint8_t)(g_conn_handle >> 8);
    p[3] = 1u;                                        /* Complete_Count */

    bt_send_cmd(BT_OP_Host_Num_Completed_Packets, p, 4u);
}
