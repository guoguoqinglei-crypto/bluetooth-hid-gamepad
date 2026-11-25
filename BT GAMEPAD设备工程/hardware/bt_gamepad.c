/* 应用层：绑定信息(Flash 末页)存取、摇杆按键采样、HID 报告组装、断线主动回连。
 * 输入报告 ID=1，15 字节：X/Y/Z/Rz 各 16 位，LT/RT 各 10 位，Hat 4 位，按键 10 位；
 * 上线再加 0xA1 + 0x01 共 17 字节。 */
#include <bt_hci.h>

#include <string.h>
#include <stm32f10x.h>

#include <bt_uart.h>
#include <uart.h>
#include <delay.h>
#include <joystick.h>
#include <key.h>
#include <tim2.h>
#include <usb_board.h>      /* hal_time_ms()：1ms 时基（TIM4 提供） */

/* ==========================================================================
 * 绑定信息（Flash 最后一页）
 *   STM32F103RCT6 有 256KB Flash，最后一页起始 0x0803F800，页大小 2KB。
 *   本工程代码约 60KB，离这里很远，不会冲突。
 * ========================================================================== */
#define BOND_FLASH_ADDR   0x0803F800u
#define BOND_MAGIC        0x424F4E44u    /* "BOND" */

typedef struct
{
    uint32_t magic;
    uint8_t  addr[6];
    uint8_t  pad[2];
    uint8_t  key[16];
    uint32_t xorCheck;
} bond_t;

static uint8_t s_bondAddr[6];
static uint8_t s_bondKey[16];
static uint8_t s_bondValid = 0u;

static uint8_t  s_bondPending = 0u;      /* 有 Key 待落盘 */
static uint32_t s_bondStamp   = 0u;      /* Key 到达时刻 */

/* 回连状态 */
static uint8_t  s_reconnectOn = 0u;
static uint8_t  s_tryCount    = 0u;
static uint32_t s_lastTry     = 0u;

/* 采样 */
static uint32_t s_lastSample = 0u;
static uint32_t s_lastReport = 0u;
static uint32_t s_connMs     = 0u;      /* 本次连接的起点（状态行算 t= 用） */
static uint32_t s_hidCtrlMs  = 0u;      /* HID Control 配好的时刻（用来延时自己开 Interrupt） */
static uint8_t  s_byUs       = 0u;      /* 本次连接是不是我们主动回连的 */

#define SAMPLE_MS      20u
#define REPORT_MS      20u
#define RECONNECT_MS   6000u    /* 断线后等 6 秒再回连（控制器要时间释放 ACL） */
#define RECONNECT_MAX  4u
#define BOND_DELAY_MS  3000u

/* ==========================================================================
 * Flash 读写
 * ========================================================================== */
static uint32_t bond_xor(const uint8_t *addr, const uint8_t *key)
{
    uint32_t v = BOND_MAGIC;
    uint8_t  i;

    for (i = 0u; i < 6u; i++)
    {
        v = (v << 1) ^ (uint32_t)addr[i];
    }
    for (i = 0u; i < 16u; i++)
    {
        v = (v << 1) ^ (uint32_t)key[i];
    }
    return v;
}

static void bond_load(void)
{
    const bond_t *b = (const bond_t *)BOND_FLASH_ADDR;

    s_bondValid = 0u;

    if (b->magic != BOND_MAGIC)
    {
        return;
    }
    if (b->xorCheck != bond_xor(b->addr, b->key))
    {
        return;
    }

    memcpy(s_bondAddr, b->addr, 6u);
    memcpy(s_bondKey, b->key, 16u);
    s_bondValid = 1u;

    /* s_bondAddr 存的是 HCI 线上的字节序（低字节在前），打印时反过来说 */
    UART1_Printf("[bond] found peer %02X:%02X:%02X:%02X:%02X:%02X\r\n",
                 s_bondAddr[5], s_bondAddr[4], s_bondAddr[3],
                 s_bondAddr[2], s_bondAddr[1], s_bondAddr[0]);
}

static void bond_save(void)
{
    bond_t   tmp;
    uint32_t *w;
    uint32_t  i;

    memset(&tmp, 0xFF, sizeof(tmp));
    tmp.magic = BOND_MAGIC;
    memcpy(tmp.addr, s_bondAddr, 6u);
    memcpy(tmp.key,  s_bondKey, 16u);
    tmp.xorCheck = bond_xor(s_bondAddr, s_bondKey);

    FLASH_Unlock();
    FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_PGERR | FLASH_FLAG_WRPRTERR);
    FLASH_ErasePage(BOND_FLASH_ADDR);

    w = (uint32_t *)&tmp;
    for (i = 0u; i < (uint32_t)(sizeof(tmp) / 4u); i++)
    {
        FLASH_ProgramWord(BOND_FLASH_ADDR + i * 4u, w[i]);
    }
    FLASH_Lock();

    UART1_Printf("[bond] saved to flash (%u bytes)\r\n", (unsigned)sizeof(tmp));
}

/* ==========================================================================
 * 对外：绑定信息
 * ========================================================================== */
void bt_app_on_link_key(const uint8_t addr[6], const uint8_t key[16])
{
    memcpy(s_bondAddr, addr, 6u);
    memcpy(s_bondKey, key, 16u);
    s_bondValid   = 1u;
    s_bondPending = 1u;
    s_bondStamp   = 0u;              /* 由 Task 里补时间戳 */
}

uint8_t bt_app_link_key_for(const uint8_t addr[6], uint8_t out_key[16])
{
    if ((s_bondValid == 0u) || (memcmp(addr, s_bondAddr, 6u) != 0))
    {
        return 0u;
    }
    memcpy(out_key, s_bondKey, 16u);
    return 1u;
}

void bt_app_commit_bond(void)
{
    if (s_bondPending != 0u)
    {
        s_bondPending = 0u;
        bond_save();
    }
}

/* 作废本地绑定：擦掉 Flash 里的 Key，并允许重新配对。
 *
 * 什么时候会用到：
 *   1. 电脑那边删了配对（或者重装系统/换电脑），它的 Key 没了，
 *      我们这边还留着旧的 → 认证返回 0x06 (PIN or Key Missing)，
 *      不擦掉就会一直拿坏 Key 重试，永远配不上。
 *   2. 开机时按住 A 键，手动清一次。 */
void bt_app_drop_bond(void)
{
    s_bondValid   = 0u;
    s_bondPending = 0u;
    s_bondStamp   = 0u;
    s_reconnectOn = 0u;
    s_tryCount    = 0u;

    memset(s_bondAddr, 0, sizeof(s_bondAddr));
    memset(s_bondKey, 0, sizeof(s_bondKey));

    FLASH_Unlock();
    FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_PGERR | FLASH_FLAG_WRPRTERR);
    FLASH_ErasePage(BOND_FLASH_ADDR);
    FLASH_Lock();

    UART1_SendString("[bond] CLEARED - ready to pair again\r\n");
}

/* ==========================================================================
 * 链路事件回调
 * ========================================================================== */
void bt_app_on_connected(void)
{
    /* 此刻 g_conn_by_us 还没被事件层清零，正好存下来给加密完成时用 */
    s_byUs       = g_conn_by_us;
    s_tryCount = 0u;
    s_lastSample = 0u;
    s_lastReport = 0u;
    s_hidCtrlMs  = 0u;
    s_connMs = hal_time_ms();       /* 记录连接起点，状态行里算 t= */
}

void bt_app_on_disconnected(void)
{
    /* 断开后先重新打开扫描。
     * 首次回连要等一会儿：刚断开时控制器还占着那条 ACL，
     * 立刻 Create Connection 会返回 0x0B (Limited Resources)。
     * 所以把计时起点设在"断开时刻"，等满一个周期再试。 */
    bt_cmd_scan_enable(0x03u);
    s_reconnectOn = (s_bondValid != 0u) ? 1u : 0u;
    s_tryCount    = 0u;
    s_lastTry     = hal_time_ms();
    s_hidCtrlMs   = 0u;
    s_byUs        = 0u;
}

void bt_app_on_encrypted(void)
{
    /* 这里把扫描关掉：连接期间还开着查询扫描会占天线，下降包速率。 */
    bt_cmd_scan_enable(0x00u);

    /* 回连时必须由我们主动开 HID Control 通道。
     *
     * 实测：回连后认证、加密全部成功，但电脑一个 L2CAP 包都不发，
     * 通道表一直 ch=000，设备在"游戏控制器"里也不出现、属性点不动。
     * 因为电脑觉得"这设备我早配对过了"，等设备来连它。
     * 首次配对则相反，主机会自己来开，那时我们主动发反而会跟它抢 PSM，
     * 所以只在 s_byUs 时发起。 */
    if (s_byUs != 0u)
    {
        s_byUs = 0u;
        UART1_SendString("[bt] reconnect: we open HID Control\r\n");
        bt_l2cap_open_hid_control();
    }
}

/* ==========================================================================
 * 手柄报告
 * ========================================================================== */
static uint16_t adc_to_u16(uint16_t v)
{
    /* ADC 0..4095 → 0..65535，中位 2048 → 32768 */
    return (uint16_t)((uint32_t)v * 16u);
}

static void build_report(uint8_t rpt[17])
{
    const uint16_t rawLX = adc_to_u16(Joystick_ReadAxis(JOYSTICK_LX));
    const uint16_t rawLY = adc_to_u16(Joystick_ReadAxis(JOYSTICK_LY));
    const uint16_t rawRX = adc_to_u16(Joystick_ReadAxis(JOYSTICK_RX));
    const uint16_t rawRY = adc_to_u16(Joystick_ReadAxis(JOYSTICK_RY));

    /* 电位器相对 PCB 旋转了 90°（与 joystick.c 的 JOY_ROTATE_90 一致）：
     * 水平 = 原 Y 通道，竖直 = 原 X 通道。
     * Windows HID 约定 Y 值小 = 上，物理上推让原 X 通道变小，正好直接对应。 */
    const uint16_t lx = rawLY;
    const uint16_t ly = rawLX;
    const uint16_t rx = rawRY;
    const uint16_t ry = rawRX;

    uint16_t btn = 0u;

    /* 10 个按键位，顺序与报告描述符一致。
     * ⚠ 极性：按键脚是上拉输入，**按下读到 0**，松开读到 1。
     *   Key_GetState / Joystick_GetKeyState 返回的是原始电平，
     *   所以这里必须判 == 0 才算按下，判 != 0 会把所有键一直当成按着。 */
    if (Key_GetState(KEY_A)  == 0u) { btn |= (1u << 0); }
    if (Key_GetState(KEY_B)  == 0u) { btn |= (1u << 1); }
    if (Key_GetState(KEY_X)  == 0u) { btn |= (1u << 2); }
    if (Key_GetState(KEY_Y)  == 0u) { btn |= (1u << 3); }
    if (Key_GetState(KEY_UP) == 0u) { btn |= (1u << 4); }
    if (Key_GetState(KEY_DN) == 0u) { btn |= (1u << 5); }
    if (Key_GetState(KEY_LF) == 0u) { btn |= (1u << 6); }
    if (Key_GetState(KEY_RG) == 0u) { btn |= (1u << 7); }
    if (Joystick_GetKeyState(JOYSTICK_L3) == 0u) { btn |= (1u << 8); }
    if (Joystick_GetKeyState(JOYSTICK_R3) == 0u) { btn |= (1u << 9); }

    memset(rpt, 0, 17u);

    rpt[0] = 0xA1u;                 /* HIDP：输入报告 */
    rpt[1] = 0x01u;                 /* Report ID */

    /* 四个 16 位轴：X / Y / Z / Rz */
    rpt[2]  = (uint8_t)(lx & 0xFFu);
    rpt[3]  = (uint8_t)(lx >> 8);
    rpt[4]  = (uint8_t)(ly & 0xFFu);
    rpt[5]  = (uint8_t)(ly >> 8);
    rpt[6]  = (uint8_t)(rx & 0xFFu);
    rpt[7]  = (uint8_t)(rx >> 8);
    rpt[8]  = (uint8_t)(ry & 0xFFu);
    rpt[9]  = (uint8_t)(ry >> 8);

    /* LT / RT：本板没有模拟扳机，恒 0（10 位 + 6 位填充） */
    rpt[10] = 0u;
    rpt[11] = 0u;
    rpt[12] = 0u;
    rpt[13] = 0u;

    /* Hat：不用方向帽，0 = 中位；低 4 位 + 高 4 位填充 */
    rpt[14] = 0u;

    /* 按键 10 位：低 8 位放 rpt[15]，高 2 位放 rpt[16] 的 bit0-1 */
    rpt[15] = (uint8_t)(btn & 0xFFu);
    rpt[16] = (uint8_t)((btn >> 8) & 0x03u);
}

/* ==========================================================================
 * 链路自检：在多组串口参数上各试一次 Read Local Version
 *
 * 为什么要试这么多组：
 *   厂商注释写"921600 偶校验"，但补丁表里 0x8000005C 写的是 0x03200000，
 *   按 RDA5876 手册的 LCR 定义那是 8N1（无校验）。两种说法冲突，
 *   只能实测。哪一个组合收到 04 0E 就锁定哪一个。
 * ========================================================================== */
uint8_t BT_Probe(void)
{
    typedef struct
    {
        uint32_t baud;
        uint32_t parity;
        uint32_t tag;         /* 统一用 32 位，结构体内部不会有填充字节 */
    } line_t;

    static const line_t lines[6] =
    {
        /* 实测 RDA5876 跑 921600 8N1，正常情况第一条就命中，
         * 后面几条是给别的固件版本留的兜底 */
        { 921600u, USART_Parity_No,   'N' },
        { 921600u, USART_Parity_Even, 'E' },
        { 921600u, USART_Parity_Odd,  'O' },
        { 115200u, USART_Parity_No,   'N' },
        { 115200u, USART_Parity_Even, 'E' },
        { 460800u, USART_Parity_No,   'N' }
    };

    uint8_t  cap[32];
    uint8_t  n;
    uint8_t  i;
    uint16_t k;
    uint16_t tries;

    for (i = 0u; i < 6u; i++)
    {
        BtUart_SetBaud(lines[i].baud, (uint16_t)lines[i].parity);
        bt_transport_reset();

        /* 同一条命令连发两次，中间隔 50ms：
         * 芯片刚启动时第一条可能被吃掉，发两次能显著提高命中率 */
        bt_send_cmd(BT_OP_Read_Local_Version, 0, 0u);
        Delay_ms(50);
        bt_send_cmd(BT_OP_Read_Local_Version, 0, 0u);

        n = 0u;
        for (k = 0u; k < 300u; k++)
        {
            uint8_t b;
            while ((n < (uint8_t)sizeof(cap)) && (BtUart_TryRead(&b) != 0u))
            {
                cap[n++] = b;
            }
            Delay_ms(1);
        }

        UART1_Printf("[probe] %lu %c rx=%u :", (unsigned long)lines[i].baud,
                     (int)lines[i].tag, (unsigned)n);
        for (tries = 0u; tries < (uint16_t)n; tries++)
        {
            UART1_Printf(" %02X", (unsigned)cap[tries]);
        }
        UART1_SendString("\r\n");

        /* 收到 04 0E（事件 + Command Complete）才算真通 */
        for (k = 0u; (uint16_t)(k + 1u) < (uint16_t)n; k++)
        {
            if ((cap[k] == H4_TYPE_EVENT) && (cap[k + 1u] == EVT_Command_Complete))
            {
                UART1_SendString("[probe] LINK ALIVE -> keep this setting\r\n");
                bt_transport_reset();
                return 1u;
            }
        }
    }

    BtUart_SetBaud(921600u, USART_Parity_Even);
    bt_transport_reset();
    UART1_SendString("[probe] DEAD on all 6 settings\r\n");
    return 0u;
}

/* ==========================================================================
 * 输出报告：主机 → 设备（上位机调 RGB 灯）
 * --------------------------------------------------------------------------
 * 用报告描述符里本来就有的 PID Set Effect Report（Report ID 3），数据 8 字节：
 *     data[0]  Start Delay（4 位 + 4 位填充）—— 本机当命令标记用，固定 0x01
 *     data[1]  R   0~100
 *     data[2]  G   0~100
 *     data[3]  B   0~100
 *     data[4..7]  Magnitude / Duration / Loop Count，本机不用，填 0
 *
 * 为什么不新加一个厂商自定义的输出报告：
 *   这份 SDP 记录是逐字节手写、且已经验证过能配对能开 HID 通道的版本，
 *   往报告描述符里插字节要同时改三层长度域（306 / 513 / 642）和内层
 *   SEQUENCE 头，改错就直接连不上；而 Windows 下发输出报告只按声明的
 *   长度（9 字节，含 Report ID）校验，复用 3 号报告完全够用。
 *   标记字节的用处：游戏的震动报告也是 3 号，靠它把两者区分开。
 * ========================================================================== */
static uint8_t s_rgbLast[3] = { 0u, 0u, 0u };

static void rgb_apply(uint8_t r, uint8_t g, uint8_t b)
{
    if (r > 100u) { r = 100u; }
    if (g > 100u) { g = 100u; }
    if (b > 100u) { b = 100u; }

    /* 值没变就不重配 PWM：彩虹动画一秒几十帧，逐帧写 CCR 没必要 */
    if ((r == s_rgbLast[0]) && (g == s_rgbLast[1]) && (b == s_rgbLast[2]))
    {
        return;
    }

    s_rgbLast[0] = r;
    s_rgbLast[1] = g;
    s_rgbLast[2] = b;

    Tim2_SetRedBrightness(r);
    Tim2_SetGreenBrightness(g);
    Tim2_SetBlueBrightness(b);

    UART1_Printf("[rgb] R=%u G=%u B=%u\r\n", (unsigned)r, (unsigned)g, (unsigned)b);
}

void bt_app_on_output_report(const uint8_t *p, uint16_t len)
{
    if ((p == NULL) || (len < 5u))
    {
        return;                       /* 太短，连 RGB 三字节都凑不齐 */
    }
    if (p[0] != 0x03u)
    {
        return;                       /* 只处理 3 号（PID Set Effect）报告 */
    }
    if (p[1] != 0x01u)
    {
        return;                       /* 没有上位机标记：系统自己发的震动报告，不理 */
    }

    rgb_apply(p[2], p[3], p[4]);
}

/* ==========================================================================
 * 初始化
 * ========================================================================== */
void BT_Init(void)
{
    uint8_t ok;

    bt_l2cap_reset();

    /* 开机时按住 A 键 = 清除绑定。
     * 换电脑、电脑重装、配对状态乱了的时候用得上，
     * 不然板子会一直拿旧 Key 去认证，对方不认（0x06）。 */
    if (Key_GetState(KEY_A) == 0u)
    {
        UART1_SendString("[bond] KEY_A held at boot -> erase bond\r\n");
        bt_app_drop_bond();
    }

    bond_load();

    g_bt_state = BT_STATE_IDLE;

    UART1_SendString("[bt] init sequence start\r\n");
    ok = bt_init_sequence();

    if (ok == 0u)
    {
        g_bt_error = 1u;
        UART1_SendString("[bt] init FAILED: no event from controller\r\n");
        return;
    }

    g_bt_error = 0u;
    g_bt_state = BT_STATE_READY;
    s_reconnectOn = (s_bondValid != 0u) ? 1u : 0u;

    UART1_Printf("[bt] ready, bond=%u (name=%s)\r\n",
                 (unsigned)s_bondValid, "STM32 BT Gamepad");
}

/* ==========================================================================
 * 周期状态：每 2 秒打一行，一眼看出三个 L2CAP 通道各开了没有
 *   st   = 状态（0=初始化 1=可被发现 2=已连接 3=HID 就绪）
 *   ch   = SDP / HID-Control / HID-Interrupt 三个通道的 open 标志（1=已开）
 *   rpt  = 已发出的手柄报告数
 *   t    = 本次连接已持续多少毫秒（看超时是多久死的）
 *   drop = 串口累计丢字节数（不为 0 说明接收跟不上，H4 会错位）
 * ========================================================================== */
static void print_status(uint32_t now)
{
    static uint32_t last = 0u;

    if ((uint32_t)(now - last) < 2000u)
    {
        return;
    }
    last = now;

    /* 用实时的 hal_time_ms() 算连接时长：now 是本轮开头取的，
     * 而 s_connMs 可能在 bt_transport_poll() 里刚被更新成更大的值，
     * 直接用 now - s_connMs 会下溢成 4294967295。 */
    now = hal_time_ms();

    UART1_Printf("[bt] st=%u ch=%u%u%u rpt=%lu t=%lu drop=%lu\r\n",
                 (unsigned)g_bt_state,
                 (unsigned)g_chan[BT_CHAN_SDP].open,
                 (unsigned)g_chan[BT_CHAN_HID_CTRL].open,
                 (unsigned)g_chan[BT_CHAN_HID_INT].open,
                 (unsigned long)g_bt_reports,
                 (unsigned long)(now - s_connMs),
                 (unsigned long)BtUart_RxDropped());
}

/* ==========================================================================
 * 蓝牙任务
 * ========================================================================== */
void BT_Task(void)
{
    uint32_t now = hal_time_ms();

    /* 1. 收包（必须每轮跑，不能有毫秒级阻塞） */
    bt_transport_poll();

    /* 1.5 连上之后周期报一次通道状态，方便定位卡在哪个通道 */
    if (g_conn_handle != BT_HANDLE_NONE)
    {
        print_status(now);
    }

    /* 2. 绑定落盘：必须等 HID 通道建好之后再写。
     *    擦一页 Flash 会让 CPU 停 20~40ms，这期间 USART2 中断也进不来，
     *    控制器等不到主机的 LMP 应答，30 秒后会以 0x22 断开链路。
     *    所以门控条件是"HID 已就绪"，而不是"已连接"。 */
    if ((s_bondPending != 0u) && (g_conn_handle != BT_HANDLE_NONE) &&
        (bt_hid_channel_ready() != 0u))
    {
        if (s_bondStamp == 0u)
        {
            s_bondStamp = now;
        }
        else if ((uint32_t)(now - s_bondStamp) > BOND_DELAY_MS)
        {
            bt_app_commit_bond();
        }
    }

    /* 3. 已连接：按 20ms 采样并发送手柄报告 */
    if (g_conn_handle != BT_HANDLE_NONE)
    {
        /* 3.0 HID Control 配好之后，如果主机一直不来开 Interrupt，
         *     等 300ms 就由我们自己发起一次（只试一次）。
         *     实测 Windows 和安卓都会在 Control 配置完成后停止发送，
         *     不自己发起的话永远等不到。 */
        if ((g_chan[BT_CHAN_HID_CTRL].open != 0u) &&
            (g_chan[BT_CHAN_HID_INT].used == 0u))
        {
            if (s_hidCtrlMs == 0u)
            {
                s_hidCtrlMs = now;
            }
            else if ((uint32_t)(now - s_hidCtrlMs) > 300u)
            {
                bt_l2cap_open_hid_interrupt();
            }
        }

        if (bt_hid_channel_ready() != 0u)
        {
            if (g_bt_state != BT_STATE_HID_READY)
            {
                g_bt_state = BT_STATE_HID_READY;
                UART1_SendString("[bt] HID interrupt channel open\r\n");
            }

            if ((uint32_t)(now - s_lastSample) >= SAMPLE_MS)
            {
                uint32_t due;

                s_lastSample = now;
                due = (uint32_t)(now - s_lastReport);

                if (due >= REPORT_MS)
                {
                    uint8_t rpt[17];
                    build_report(rpt);
                    bt_hid_send_report(rpt, 17u);
                    s_lastReport = now;
                }
            }
        }
        else if (g_bt_state == BT_STATE_HID_READY)
        {
            /* 通道被关了，退回"已连接未就绪" */
            g_bt_state = BT_STATE_CONNECTED;
        }
    }
    else
    {
        /* 4. 未连接：回连 / 保持可被发现 */
        if ((s_reconnectOn != 0u) && (s_bondValid != 0u) &&
            (s_tryCount < RECONNECT_MAX))
        {
            if ((s_lastTry == 0u) ||
                ((uint32_t)(now - s_lastTry) >= RECONNECT_MS))
            {
                s_lastTry = now;
                s_tryCount++;

                UART1_Printf("[bt] reconnect try %u/%u\r\n",
                             (unsigned)s_tryCount, (unsigned)RECONNECT_MAX);

                bt_cmd_scan_enable(0x00u);       /* 主动寻呼时先关扫描 */
                g_conn_by_us = 1u;
                bt_cmd_create_connection(s_bondAddr);
            }
        }
        else if (s_tryCount >= RECONNECT_MAX)
        {
            /* 回连尝试用完，退回"等电脑来连"，重新打开扫描 */
            s_tryCount = 0u;
            s_reconnectOn = 0u;
            bt_cmd_scan_enable(0x03u);
            UART1_SendString("[bt] reconnect gave up, back to discoverable\r\n");
        }

        if (g_bt_state != BT_STATE_READY && g_bt_error == 0u)
        {
            g_bt_state = BT_STATE_READY;
        }

        /* 保持 ACL 信用不枯竭（控制器发来 Completed Packets 时自动回补） */
        if (g_acl_credits == 0u)
        {
            g_acl_credits = g_acl_pkts;
        }
    }
}
