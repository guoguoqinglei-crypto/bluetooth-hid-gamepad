/* ACL 重组 + L2CAP 信令 + SDP 服务端 + HID 数据发送。
 * CID 0x0001=信令；PSM 0x0001=SDP / 0x0011=HID Control / 0x0013=HID Interrupt；
 * 动态 CID 从 0x0040 起。HID 通道由主机发起，本机只应答。 */
#include <bt_hci.h>
#include <bt_sdp_record.h>

#include <string.h>
#include <stm32f10x.h>

#include <uart.h>
#include <delay.h>

#define L2CAP_MTU_LOCAL   672u

/* 是否把每个 ACL 包都打出来。
 * ⚠ 默认关：排查期打开过一次，日志量太大（每包一行 + 逐字节），
 *   串口打印本身会拖慢蓝牙任务，反而干扰协议时序。需要时再开。 */
#define BT_TRACE_ACL   0

/* 是否把每一条 L2CAP 信令（收/发）都打出来。
 * 0 = 只打通道状态变化（日志短，一屏能看完整条连接过程）
 * 1 = 逐包打印（排查信令字段问题时用） */
#define BT_TRACE_SIG   1

/* ACL 重组缓冲 */
static uint8_t  s_aclBuf[BT_ACL_BUF_SIZE];
static uint16_t s_aclLen   = 0u;    /* 已拼字节数 */
static uint16_t s_aclTotal = 0u;    /* 本 PDU 总长（L2CAP 头 + 负载） */

static uint16_t s_nextCid = 0x0040u;

/* 我方主动发出的信令 Identifier，逐条递增。
 * 规范要求"同一条通道上未完成的请求 Identifier 不能重复"，
 * 固定用一个值会在连续建多条通道时让对方的响应匹配错乱。 */
static uint8_t  s_sigId = 0x10u;

static uint8_t sig_next_id(void)
{
    uint8_t v = s_sigId++;
    if (s_sigId == 0u)
    {
        s_sigId = 0x10u;
    }
    return v;
}

/* 发送缓冲：SDP 响应 653 字节 + L2CAP 头 */
static uint8_t  s_out[700];

/* ==========================================================================
 * 小工具
 * ========================================================================== */
static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint16_t rd16be(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void wr16be(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

/* ==========================================================================
 * 通道表
 * ========================================================================== */
void bt_l2cap_reset(void)
{
    uint8_t i;

    for (i = 0u; i < BT_CHAN_COUNT; i++)
    {
        g_chan[i].psm     = 0u;
        g_chan[i].lcid    = 0u;
        g_chan[i].rcid    = 0u;
        g_chan[i].used    = 0u;
        g_chan[i].cfg_sent= 0u;
        g_chan[i].cfg_rcvd= 0u;
        g_chan[i].open    = 0u;
    }

    g_chan[BT_CHAN_SDP].psm       = L2CAP_PSM_SDP;
    g_chan[BT_CHAN_HID_CTRL].psm  = L2CAP_PSM_HID_CONTROL;
    g_chan[BT_CHAN_HID_INT].psm   = L2CAP_PSM_HID_INTERRUPT;

    s_aclLen   = 0u;
    s_aclTotal = 0u;
    s_nextCid  = 0x0040u;
}

static uint16_t cid_alloc(void)
{
    uint16_t c = s_nextCid++;
    if (s_nextCid == 0x0000u || s_nextCid > 0x00FFu)
    {
        s_nextCid = 0x0040u;            /* 回绕保护，够用就行 */
    }
    return c;
}

static int ch_by_psm(uint16_t psm)
{
    uint8_t i;
    for (i = 0u; i < BT_CHAN_COUNT; i++)
    {
        if (g_chan[i].psm == psm)
        {
            return (int)i;
        }
    }
    return -1;
}

static int ch_by_lcid(uint16_t lcid)
{
    uint8_t i;
    for (i = 0u; i < BT_CHAN_COUNT; i++)
    {
        if ((g_chan[i].used != 0u) && (g_chan[i].lcid == lcid))
        {
            return (int)i;
        }
    }
    return -1;
}

static int ch_by_rcid(uint16_t rcid)
{
    uint8_t i;
    for (i = 0u; i < BT_CHAN_COUNT; i++)
    {
        if ((g_chan[i].used != 0u) && (g_chan[i].rcid == rcid))
        {
            return (int)i;
        }
    }
    /* 有些主机在 CONF_RSP 里放的是本端 CID，两种都认，容错 */
    for (i = 0u; i < BT_CHAN_COUNT; i++)
    {
        if ((g_chan[i].used != 0u) && (g_chan[i].lcid == rcid))
        {
            return (int)i;
        }
    }
    return -1;
}

static void ch_clear(int idx)
{
    if (idx < 0)
    {
        return;
    }
    g_chan[idx].lcid     = 0u;
    g_chan[idx].rcid     = 0u;
    g_chan[idx].used     = 0u;
    g_chan[idx].cfg_sent = 0u;
    g_chan[idx].cfg_rcvd = 0u;
    g_chan[idx].open     = 0u;
}

/* ==========================================================================
 * L2CAP 发送
 * ========================================================================== */
static void l2cap_send(uint16_t cid, const uint8_t *payload, uint16_t len)
{
    /* 超过单个 ACL 包上限时由 bt_send_acl() 自动分片（PB=10/01），
     * 这里只拦"连分片缓冲都放不下"的异常长度。 */
    if (len > BT_TX_ACL_MAX)
    {
        UART1_Printf("[l2] drop: len=%u > %u\r\n",
                     (unsigned)len, (unsigned)BT_TX_ACL_MAX);
        return;
    }
    bt_send_acl(g_conn_handle, cid, payload, len);
}

static void sig_send(uint8_t code, uint8_t id,
                     const uint8_t *data, uint16_t len)
{
    uint8_t  buf[64];
    uint16_t total = (uint16_t)(4u + len);

    if (total > (uint16_t)sizeof(buf))
    {
        return;
    }

    buf[0] = code;
    buf[1] = id;
    wr16(&buf[2], len);
    if (len != 0u)
    {
        memcpy(&buf[4], data, len);
    }

    /* 把发出去的信令也打出来，和收到的对照才知道响应对不对 */
#if BT_TRACE_SIG
    {
        uint16_t n = (len > 10u) ? 10u : len;
        uint16_t j;

        UART1_Printf("[l2] TX code=0x%02X id=%u len=%u :",
                     (unsigned)code, (unsigned)id, (unsigned)len);
        for (j = 0u; j < n; j++)
        {
            UART1_Printf(" %02X", (unsigned)data[j]);
        }
        UART1_SendString("\r\n");
    }
#endif

    l2cap_send(L2CAP_CID_SIGNALING, buf, total);
}

/* ==========================================================================
 * SDP 服务端
 * ========================================================================== */
static void sdp_send_attr_rsp(uint16_t tid, uint16_t rcid)
{
    uint16_t pos = 0u;
    uint16_t attrCount;
    uint16_t paramLen;

    /* AttributeList = 外层 SEQUENCE 头(3) + 642 字节服务记录 */
    attrCount = (uint16_t)(3u + (uint16_t)sizeof(bt_sdp_attribute_list));
    /* ParamLen = AttrListByteCount(2) + AttributeList + ContinuationState(1) */
    paramLen  = (uint16_t)(2u + attrCount + 1u);

    s_out[pos++] = SDP_PDU_SERVICE_SEARCH_ATTR_RSP;   /* 0x07 */
    wr16be(&s_out[pos], tid);  pos = (uint16_t)(pos + 2u);
    wr16be(&s_out[pos], paramLen); pos = (uint16_t)(pos + 2u);
    wr16be(&s_out[pos], attrCount); pos = (uint16_t)(pos + 2u);

    /* 外层 SEQUENCE：0x36 + 16 位长度(642) */
    s_out[pos++] = 0x36u;
    s_out[pos++] = (uint8_t)(sizeof(bt_sdp_attribute_list) >> 8);
    s_out[pos++] = (uint8_t)(sizeof(bt_sdp_attribute_list) & 0xFFu);

    memcpy(&s_out[pos], bt_sdp_attribute_list,
           sizeof(bt_sdp_attribute_list));
    pos = (uint16_t)(pos + sizeof(bt_sdp_attribute_list));

    s_out[pos++] = 0x00u;            /* ContinuationState = 0（没有后续段） */

    l2cap_send(rcid, s_out, pos);

    UART1_Printf("[sdp] attr rsp %u bytes -> cid=0x%04X\r\n",
                 (unsigned)pos, (unsigned)rcid);
}

/* SDP 请求的处理直接放在 bt_acl_input() 里做（数据量小、同步回即可），
 * 这里只保留响应组包。 */

/* ==========================================================================
 * HID Control 通道上的 HIDP 请求应答
 *
 * 【为什么这一步决定"能不能用"】
 *   Windows 打开 HID Control 通道后，会立刻在这条通道上发 SET_IDLE /
 *   SET_PROTOCOL / GET_REPORT 这类请求，每一条都在等应答。不回，它就重试；
 *   重试几次仍无回应，就判定这个 HID 设备有问题，不再去开 Interrupt 通道，
 *   几秒后直接把链路拆掉 —— 现象正是"能连上一下、过一会儿自己掉、设备不能用"。
 *
 * 【HIDP 头字节 = 事务类型(高 4 位) | 参数(低 4 位)】
 *   0x0 HANDSHAKE     0x1 HID_CONTROL   0x2 GET_REPORT   0x3 SET_REPORT
 *   0x4 GET_PROTOCOL  0x5 SET_PROTOCOL  0x6 GET_IDLE     0x7 SET_IDLE
 *   0xA DATA          0xB DATC
 *   报告类请求的参数 = 报告类型（1 输入 / 2 输出 / 3 特性）
 *   协议类请求的参数 = 0(Report 协议) / 1(Boot 协议)
 *
 * 【控制请求处理】
 *   按高 4 位事务码处理 GET_REPORT、SET_REPORT、SET_PROTOCOL、SET_IDLE 等请求，
 *   每条合法请求都必须返回 HANDSHAKE 或 DATA，避免主机等待超时后断开链路。
 * ========================================================================== */
static void hidp_control_rsp(uint16_t cid, const uint8_t *p, uint16_t len)
{
    uint8_t hdr;
    uint8_t txn;
    uint8_t param;
    uint8_t rsp;

    if (len < 1u)
    {
        return;
    }

    hdr   = p[0];
    txn   = (uint8_t)((hdr >> 4) & 0x0Fu);
    param = (uint8_t)(hdr & 0x0Fu);

    switch (txn)
    {
        case 0x2u:                       /* GET_REPORT */
            /* 本机没有特性报告，报告描述符走 SDP 下发，不走这里。
             * 回 ERR_UNSUPPORTED_REQ 比"不回"好得多：主机拿到明确答复，
             * 会继续走后续流程，而不是干等超时。 */
            UART1_Printf("[hid] GET_REPORT type=%u rid=%u -> NAK\r\n",
                         (unsigned)param,
                         (unsigned)((len >= 2u) ? p[1] : 0u));
            rsp = 0x04u;
            break;

        case 0x4u:                       /* GET_PROTOCOL */
            rsp = 0xA1u;                 /* DATA，参数 1 = Report 协议 */
            break;

        case 0x6u:                       /* GET_IDLE */
            rsp = 0xA0u;                 /* DATA，参数 0 = 空闲率 0 */
            break;

        case 0x3u:                       /* SET_REPORT */
            /* 参数 1 = 输入报告，2 = 输出报告，3 = 特性报告。
             * 只有输出报告带数据，格式 [头][Report ID][报告数据…]，
             * 和 Interrupt 通道 0xA2 后面那段一样。 */
            if ((param == 0x02u) && (len >= 2u))
            {
                bt_app_on_output_report(&p[1], (uint16_t)(len - 1u));
            }
            rsp = 0x00u;                 /* HANDSHAKE：成功 */
            break;

        case 0x5u:                       /* SET_PROTOCOL，以及 Windows 的 SET_REPORT(Output) 0x52 */
            /* 蓝牙 HID 上 Windows 的 HidD_SetOutputReport 发的是 0x52：
             * 事务码 5、参数 2(Output)，后面直接跟 [Report ID][报告数据…]，
             * 与 Interrupt 通道 0xA2 的载荷完全同构。
             * 旧代码把 0x52 一律当 SET_PROTOCOL 只回 HANDSHAKE，RGB 全丢，
             * 于是上位机只能改走 Interrupt 通道 WriteFile；而实测持续往
             * Interrupt 通道写会让 hidbth 把同通道的输入报告挂起约 900ms，
             * 表现为彩虹动画期间摇杆/按键卡住、界面误报"链路超时"。
             * 这里补上解析，让灯效走控制通道，输入报告完全不受影响。
             * 真正的 SET_PROTOCOL 参数只会是 0(Boot)/1(Report)，不会误入。 */
            if ((param == 0x02u) && (len >= 2u))
            {
                bt_app_on_output_report(&p[1], (uint16_t)(len - 1u));
            }
            rsp = 0x00u;                 /* HANDSHAKE：成功 */
            break;

        case 0x1u:                       /* HID_CONTROL */
        case 0x7u:                       /* SET_IDLE */
            rsp = 0x00u;                 /* HANDSHAKE：成功 */
            break;

        default:
            rsp = 0x04u;
            break;
    }

    l2cap_send(cid, &rsp, 1u);
    UART1_Printf("[hid] rsp txn=0x%X -> %02X\r\n",
                 (unsigned)txn, (unsigned)rsp);
}

/* ==========================================================================
 * L2CAP 信令处理
 * ========================================================================== */
static void sig_conn_req(const uint8_t *p, uint8_t id)
{
    uint16_t psm  = rd16(&p[0]);
    uint16_t scid = rd16(&p[2]);
    int      idx  = ch_by_psm(psm);
    uint8_t  rsp[8];

    /* 注意：响应的 Identifier 必须回显请求里的那个。
     * 固定填一个值的话，对方按 Identifier 匹配不到，会把响应丢掉，
     * 现象是"连接请求发出去了，但通道一直不建立"。 */
    if (idx < 0)
    {
        wr16(&rsp[0], scid);
        wr16(&rsp[2], 0u);
        wr16(&rsp[4], L2CAP_CONN_PSM_NOT_SUPPORT);
        wr16(&rsp[6], 0u);
        sig_send(L2CAP_SIG_CONN_RSP, id, rsp, 8u);
        UART1_Printf("[l2] conn_req psm=0x%04X -> NOT SUPPORTED\r\n", (unsigned)psm);
        return;
    }

    if (g_chan[idx].open != 0u)
    {
        wr16(&rsp[0], scid);
        wr16(&rsp[2], 0u);
        wr16(&rsp[4], L2CAP_CONN_NO_RESOURCES);
        wr16(&rsp[6], 0u);
        sig_send(L2CAP_SIG_CONN_RSP, id, rsp, 8u);
        UART1_Printf("[l2] conn_req psm=0x%04X -> BUSY\r\n", (unsigned)psm);
        return;
    }

    /* 之前失败遗留的脏槽允许被重新使用 */
    if (g_chan[idx].lcid == 0u)
    {
        g_chan[idx].lcid = cid_alloc();
    }
    g_chan[idx].rcid     = scid;
    g_chan[idx].used     = 1u;
    g_chan[idx].cfg_sent = 0u;
    g_chan[idx].cfg_rcvd = 0u;
    g_chan[idx].open     = 0u;

    /* 连接响应：DCID = 对方 CID，SCID = 我方 CID，Result = 成功 */
    wr16(&rsp[0], scid);
    wr16(&rsp[2], g_chan[idx].lcid);
    wr16(&rsp[4], L2CAP_CONN_SUCCESS);
    wr16(&rsp[6], 0u);
    sig_send(L2CAP_SIG_CONN_RSP, id, rsp, 8u);

    UART1_Printf("[l2] channel OPEN psm=0x%04X lcid=0x%04X rcid=0x%04X\r\n",
                 (unsigned)psm, (unsigned)g_chan[idx].lcid, (unsigned)scid);

    /* 紧接着发配置请求（带 MTU 选项）。
     * 实测：不带选项时 Windows 干脆不回我们的配置请求（只出现一次
     * CONFIGURED），带上选项反而能走完双向配置。所以保留选项。 */
    {
        uint8_t cfg[8];
        wr16(&cfg[0], scid);            /* DCID = 对方 CID */
        wr16(&cfg[2], 0x0000u);         /* Flags */
        cfg[4] = 0x01u;                 /* 选项类型：MTU */
        cfg[5] = 0x02u;                 /* 选项长度 */
        wr16(&cfg[6], L2CAP_MTU_LOCAL);
        sig_send(L2CAP_SIG_CONF_REQ, sig_next_id(), cfg, 8u);
        g_chan[idx].cfg_sent = 1u;
    }
}

/* 收到连接响应 —— 这是我们自己主动发起连接请求时才会走到的分支。
 * 解析 DCID(=我们的本地 CID)、SCID(=对端 CID)、Result，
 * 成功后立刻发配置请求，和对端主动发起时保持一致的后续流程。 */
static void sig_conn_rsp(const uint8_t *p)
{
    uint16_t dcid   = rd16(&p[0]);
    uint16_t scid   = rd16(&p[2]);
    uint16_t result = rd16(&p[4]);
    int      idx    = ch_by_lcid(dcid);

    if (idx < 0)
    {
        return;
    }

    /* Pending：对端说自己处理不完，稍后会再发一条正式响应。
     * 规范要求此时保留槽位、继续等，不能当成失败清掉 ——
     * 一清掉，后面那条 Success 就找不到槽位，通道白建。 */
    if (result == L2CAP_CONN_PENDING)
    {
        UART1_Printf("[l2] our CONN_REQ pending psm=0x%04X, waiting\r\n",
                     (unsigned)g_chan[idx].psm);
        return;
    }

    if (result != L2CAP_CONN_SUCCESS)
    {
        UART1_Printf("[l2] our CONN_REQ rejected psm=0x%04X result=%u\r\n",
                     (unsigned)g_chan[idx].psm, (unsigned)result);
        ch_clear(idx);
        return;
    }

    g_chan[idx].rcid = scid;
    g_chan[idx].used = 1u;
    UART1_Printf("[l2] our CONN_REQ accepted psm=0x%04X lcid=0x%04X rcid=0x%04X\r\n",
                 (unsigned)g_chan[idx].psm, (unsigned)g_chan[idx].lcid, (unsigned)scid);

    {
        uint8_t cfg[8];
        wr16(&cfg[0], scid);
        wr16(&cfg[2], 0x0000u);
        cfg[4] = 0x01u;
        cfg[5] = 0x02u;
        wr16(&cfg[6], L2CAP_MTU_LOCAL);
        sig_send(L2CAP_SIG_CONF_REQ, sig_next_id(), cfg, 8u);
        g_chan[idx].cfg_sent = 1u;
    }
}

static void sig_conf_req(const uint8_t *p, uint16_t len, uint8_t id)
{
    uint16_t dcid = rd16(&p[0]);
    int      idx  = ch_by_lcid(dcid);
    uint8_t  rsp[12];
    uint16_t pos;
    uint16_t off;
    uint8_t  hasMtu = 0u;

    if (idx < 0)
    {
        return;
    }

    /* 扫描请求里的选项。
     *
     * 【实测结论】必须回显 MTU 选项。
     *   回显（10 字节）→ 主机会接着回我们的配置请求，两个 CONFIGURED 都出现；
     *   固定回 6 字节  → 主机不再回我们的配置请求，只出现一个 CONFIGURED。
     * 虽然同款硬件的那份抓包里设备只回 6 字节（因为它的主机请求里本就没选项），
     * 但我们面对的主机请求带了 MTU，不回显它就不往下走。 */
    off = 4u;
    while ((uint16_t)(off + 2u) <= len)
    {
        uint8_t type = p[off];
        uint8_t olen = p[off + 1u];

        if ((uint16_t)(off + 2u + olen) > len)
        {
            break;
        }
        if ((type == 0x01u) && (olen == 2u))
        {
            hasMtu = 1u;
        }
        off = (uint16_t)(off + 2u + olen);
    }

    wr16(&rsp[0], g_chan[idx].lcid);
    wr16(&rsp[2], 0x0000u);
    wr16(&rsp[4], L2CAP_CONF_SUCCESS);
    pos = 6u;

    if (hasMtu != 0u)
    {
        rsp[pos++] = 0x01u;
        rsp[pos++] = 0x02u;
        wr16(&rsp[pos], L2CAP_MTU_LOCAL);
        pos = (uint16_t)(pos + 2u);
    }

    sig_send(L2CAP_SIG_CONF_RSP, id, rsp, pos);

    g_chan[idx].cfg_rcvd = 1u;
    if (g_chan[idx].cfg_sent != 0u)
    {
        g_chan[idx].open = 1u;
        UART1_Printf("[l2] channel CONFIGURED psm=0x%04X\r\n",
                     (unsigned)g_chan[idx].psm);
    }
}

static void sig_conf_rsp(const uint8_t *p, uint16_t len)
{
    uint16_t scid   = rd16(&p[0]);
    uint16_t result = (len >= 6u) ? rd16(&p[4]) : 0u;
    int      idx    = ch_by_rcid(scid);

    if (idx < 0)
    {
        return;
    }

    if (result != L2CAP_CONF_SUCCESS)
    {
        /* 配置失败：把槽位彻底放掉，允许主机重新发起 */
        ch_clear(idx);
        UART1_Printf("[l2] conf FAIL result=0x%04X, slot released\r\n",
                     (unsigned)result);
        return;
    }

    g_chan[idx].cfg_sent = 1u;   /* 已确认 */
    if (g_chan[idx].cfg_rcvd != 0u)
    {
        g_chan[idx].open = 1u;
        UART1_Printf("[l2] channel CONFIGURED psm=0x%04X\r\n",
                     (unsigned)g_chan[idx].psm);
    }
}

static void sig_disc_req(const uint8_t *p, uint8_t id)
{
    uint16_t dcid = rd16(&p[0]);
    uint16_t scid = rd16(&p[2]);
    uint8_t  rsp[4];
    int      idx  = ch_by_lcid(dcid);

    wr16(&rsp[0], dcid);
    wr16(&rsp[2], scid);
    sig_send(L2CAP_SIG_DISC_RSP, id, rsp, 4u);

    if (idx >= 0)
    {
        UART1_Printf("[l2] channel CLOSED psm=0x%04X\r\n",
                     (unsigned)g_chan[idx].psm);
        ch_clear(idx);

        /* HID 通道掉了就退回"已连接未就绪" */
        if (bt_hid_channel_ready() == 0u)
        {
            if (g_bt_state == BT_STATE_HID_READY)
            {
                g_bt_state = BT_STATE_CONNECTED;
            }
        }
    }
}

static void sig_disc_rsp(const uint8_t *p)
{
    uint16_t dcid = rd16(&p[0]);
    int      idx  = ch_by_lcid(dcid);

    if (idx >= 0)
    {
        ch_clear(idx);
    }
}

static void sig_info_req(const uint8_t *p, uint8_t id)
{
    uint16_t type = rd16(&p[0]);
    uint8_t  rsp[4];

    wr16(&rsp[0], type);       /* 回显 InfoType */
    wr16(&rsp[2], 0x0001u);    /* Result = 1：不支持的 InfoType */
    sig_send(L2CAP_SIG_INFO_RSP, id, rsp, 4u);
}

/* ==========================================================================
 * ACL 输入：重组 + 按 CID 分发
 * ========================================================================== */
void bt_acl_input(const uint8_t *data, uint16_t len)
{
    uint16_t flags;
    uint8_t  pb;
    uint16_t dlen;
    uint16_t l2len;

    if (len < 4u)
    {
        return;
    }

    flags = (uint16_t)(data[0] | ((uint16_t)data[1] << 8));
    pb    = (uint8_t)((flags >> 12) & 0x03u);
    dlen  = rd16(&data[2]);

    /* 每个 ACL 包都打一行。
     * 之前没有这层日志，导致"Windows 到底发没发 Interrupt 请求"完全靠猜：
     * 如果包在重组阶段被丢掉，上层什么都不会打，看起来就像对方没发。 */
#if BT_TRACE_ACL
    UART1_Printf("[acl] h=%04X pb=%u len=%u\r\n",
                 (unsigned)(flags & 0x0FFFu), (unsigned)pb, (unsigned)dlen);
#endif

    /* 句柄不匹配的包直接丢（PDF 要求先取低 12 位再比对） */
    if ((flags & 0x0FFFu) != (g_conn_handle & 0x0FFFu))
    {
#if BT_TRACE_ACL
        UART1_Printf("[acl] drop: handle mismatch (want %04X)\r\n",
                     (unsigned)(g_conn_handle & 0x0FFFu));
#endif
        return;
    }

    if ((uint16_t)(dlen + 4u) > len)
    {
        return;                       /* 声明长度比实际收到的大，丢弃 */
    }

    if (pb == 0x01u)
    {
        /* 续包：追加 */
        if ((s_aclTotal == 0u) || ((uint32_t)s_aclLen + dlen > BT_ACL_BUF_SIZE))
        {
            s_aclTotal = 0u;
            s_aclLen   = 0u;
            return;
        }
        memcpy(&s_aclBuf[s_aclLen], &data[4], dlen);
        s_aclLen = (uint16_t)(s_aclLen + dlen);
    }
    else
    {
        /* 起始包（PB = 0b10 或 0b00）：L2CAP Length 在 data[4..5] */
        if (dlen < 4u)
        {
            return;
        }
        l2len = rd16(&data[4]);
        if ((uint32_t)l2len + 4u > BT_ACL_BUF_SIZE)
        {
            return;
        }
        s_aclTotal = (uint16_t)(l2len + 4u);
        memcpy(s_aclBuf, &data[4], dlen);
        s_aclLen = dlen;
    }

    if (s_aclLen < s_aclTotal)
    {
        return;                       /* 还没收齐，等后续分片 */
    }

    /* 收齐了：按 CID 分发 */
    {
        uint16_t cid = rd16(&s_aclBuf[2]);
        uint16_t payloadLen = (uint16_t)(s_aclTotal - 4u);
        uint8_t *payload = &s_aclBuf[4];
        int idx;

        if (cid == L2CAP_CID_SIGNALING)
        {
            /* 信令：可能一个包里串多条信令，逐条走 */
            uint16_t off = 0u;
            while ((uint16_t)(off + 4u) <= payloadLen)
            {
                uint8_t  code = payload[off];
                uint8_t  id   = payload[off + 1u];
                uint16_t slen = rd16(&payload[off + 2u]);

                if ((uint32_t)off + 4u + slen > payloadLen)
                {
                    break;
                }

                /* 每条信令都打一行：这样能看清 Windows 到底来开了哪些通道、
                 * 以及我们回没回。排查"HID Interrupt 没开"这类问题全靠它。 */
#if BT_TRACE_SIG
                UART1_Printf("[l2] sig code=0x%02X id=%u len=%u\r\n",
                             (unsigned)code, (unsigned)id, (unsigned)slen);
#endif

                switch (code)
                {
                    case L2CAP_SIG_CONN_REQ:
                        if (slen >= 4u) { sig_conn_req(&payload[off + 4u], id); }
                        break;

                    case L2CAP_SIG_CONN_RSP:
                        if (slen >= 8u) { sig_conn_rsp(&payload[off + 4u]); }
                        break;

                    case L2CAP_SIG_CONF_REQ:
                        if (slen >= 4u) { sig_conf_req(&payload[off + 4u], slen, id); }
                        break;

                    case L2CAP_SIG_CONF_RSP:
                        if (slen >= 6u) { sig_conf_rsp(&payload[off + 4u], slen); }
                        break;

                    case L2CAP_SIG_DISC_REQ:
                        if (slen >= 4u) { sig_disc_req(&payload[off + 4u], id); }
                        break;

                    case L2CAP_SIG_DISC_RSP:
                        if (slen >= 4u) { sig_disc_rsp(&payload[off + 4u]); }
                        break;

                    case L2CAP_SIG_INFO_REQ:
                        if (slen >= 2u) { sig_info_req(&payload[off + 4u], id); }
                        break;

                    default:
                        break;
                }
                off = (uint16_t)(off + 4u + slen);
            }
        }
        else
        {
            idx = ch_by_lcid(cid);
            if (idx >= 0)
            {
                if (g_chan[idx].psm == L2CAP_PSM_SDP)
                {
                    /* SDP：直接把响应发回去（数据量小，同步即可） */
                    if ((payloadLen >= 5u) &&
                        (payload[0] == SDP_PDU_SERVICE_SEARCH_ATTR_REQ))
                    {
                        uint16_t tid = rd16be(&payload[1]);
                        UART1_Printf("[sdp] search-attr req tid=%u\r\n", (unsigned)tid);
                        sdp_send_attr_rsp(tid, g_chan[idx].rcid);
                    }
                    else if (payloadLen >= 5u)
                    {
                        UART1_Printf("[sdp] pdu=0x%02X (ignored)\r\n",
                                     (unsigned)payload[0]);
                    }
                }
                else if (g_chan[idx].psm == L2CAP_PSM_HID_INTERRUPT)
                {
                    /* HID Interrupt 是双向的：0xA1 输入报告（我们发）、
                     * 0xA2 输出报告（电脑发给我们，震动/灯效） */
                    uint16_t n = (payloadLen > 8u) ? 8u : payloadLen;
                    uint16_t j;

                    UART1_Printf("[hid] INT len=%u :", (unsigned)payloadLen);
                    for (j = 0u; j < n; j++)
                    {
                        UART1_Printf(" %02X", (unsigned)payload[j]);
                    }
                    UART1_SendString("\r\n");

                    /* 0xA2 后面的内容和控制通道 SET_REPORT 的数据同构：
                     * [Report ID][报告数据…]，交给同一个解析函数。 */
                    if ((payloadLen >= 2u) && (payload[0] == 0xA2u))
                    {
                        uint8_t hs = 0x00u;   /* HANDSHAKE：成功 */

                        bt_app_on_output_report(&payload[1], (uint16_t)(payloadLen - 1u));

                        /* 收到 DATA 事务必须回 HANDSHAKE，主机在等这个应答；
                         * 不回的话它要等到超时才失败，重试几次就判设备异常，
                         * 把整条 HID 链路拆掉。 */
                        l2cap_send(g_chan[idx].rcid, &hs, 1u);
                    }
                }
                else
                {
                    /* HID Control：主机可能发 HID_CONTROL / GET_REPORT /
                     * SET_REPORT / GET_PROTOCOL 等 */
                    uint16_t n = (payloadLen > 8u) ? 8u : payloadLen;
                    uint16_t j;

                    UART1_Printf("[hid] CTRL len=%u :", (unsigned)payloadLen);
                    for (j = 0u; j < n; j++)
                    {
                        UART1_Printf(" %02X", (unsigned)payload[j]);
                    }
                    UART1_SendString("\r\n");

                    /* 每条请求都必须应答，具体规则见 hidp_control_rsp() */
                    hidp_control_rsp(g_chan[idx].rcid, payload, payloadLen);
                }
            }
            else
            {
                /* 收到一个我们没登记的 CID —— 说明通道表状态和主机不一致，
                 * 这类包如果静默丢掉，就会表现为"主机在说话，我们装没听见" */
                UART1_Printf("[l2] RX unknown cid=0x%04X len=%u\r\n",
                             (unsigned)cid, (unsigned)payloadLen);
            }
        }
    }

    s_aclLen   = 0u;
    s_aclTotal = 0u;
}

/* ==========================================================================
 * 主动建立 HID Interrupt 通道
 *
 * 【为什么要有这个】
 *   正常情况下 HID Interrupt 通道由主机发起。实测本机连 Windows 和安卓时，
 *   主机会先开 HID Control、双向配置完成，然后**就再也不发任何包了**，
 *   Interrupt 通道永远建不起来，5 秒后链路监测超时断开。
 *
 *   既然主机不发起，就由设备端自己发一次 CONN_REQ 试。主机接受就拿到通道，
 *   不接受也不会有副作用（我们按拒绝处理、清槽位）。
 * ========================================================================== */
static void l2cap_open_channel(int idx)
{
    uint8_t p[4];

    if (g_chan[idx].used != 0u)
    {
        return;                          /* 已经在建或已建好 */
    }

    if (g_chan[idx].lcid == 0u)
    {
        g_chan[idx].lcid = cid_alloc();
    }
    g_chan[idx].used     = 1u;
    g_chan[idx].rcid     = 0u;
    g_chan[idx].cfg_sent = 0u;
    g_chan[idx].cfg_rcvd = 0u;
    g_chan[idx].open     = 0u;

    wr16(&p[0], g_chan[idx].psm);
    wr16(&p[2], g_chan[idx].lcid);
    sig_send(L2CAP_SIG_CONN_REQ, sig_next_id(), p, 4u);

    UART1_Printf("[l2] we initiate psm=0x%04X lcid=0x%04X\r\n",
                 (unsigned)g_chan[idx].psm, (unsigned)g_chan[idx].lcid);
}

/* ==========================================================================
 * 回连时主动建立 HID Control 通道
 *
 * 【为什么必须我们发起】
 *   首次配对：电脑不认识这个设备，会主动跑 SDP、开 HID Control/Interrupt。
 *   回连：电脑认为"这设备我早配对过了，该你来连我"，自己一个包都不发。
 *   实测回连日志卡在 st=2 ch=000 —— 认证、加密全过，就是没人开通道。
 *   讲义第 7 章也写明：回连时"加密完成后由本机主动发起 HID Control 通道连接"。
 *
 *   Control 通道建好后，Interrupt 由主机接着开；若 300ms 还没动静，
 *   蓝牙任务里的 bt_l2cap_open_hid_interrupt() 会兜底主动发一次。
 * ========================================================================== */
void bt_l2cap_open_hid_control(void)
{
    l2cap_open_channel((int)BT_CHAN_HID_CTRL);
}

void bt_l2cap_open_hid_interrupt(void)
{
    if (g_chan[BT_CHAN_HID_CTRL].open == 0u)
    {
        return;                          /* Control 还没配好，先别急 */
    }
    l2cap_open_channel((int)BT_CHAN_HID_INT);
}

/* ==========================================================================
 * HID 报告发送
 * ========================================================================== */
uint8_t bt_hid_channel_ready(void)
{
    return ((g_chan[BT_CHAN_HID_INT].open != 0u) &&
            (g_chan[BT_CHAN_HID_INT].rcid != 0u)) ? 1u : 0u;
}

void bt_hid_send_report(const uint8_t *report, uint16_t len)
{
    if (bt_hid_channel_ready() == 0u)
    {
        return;
    }

    /* HID 报告走 Interrupt 通道，CID 用对端在本通道上分配的 CID。
     * 报告内容 = 0xA1(输入报告头) + ReportID + 报告数据。 */
    l2cap_send(g_chan[BT_CHAN_HID_INT].rcid, report, len);
    g_bt_reports++;
}
