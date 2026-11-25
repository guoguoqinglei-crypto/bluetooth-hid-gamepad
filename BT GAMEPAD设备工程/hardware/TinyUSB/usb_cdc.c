/* USB CDC 虚拟串口：把 UART1 日志从板子原生 USB 口送出（无需 USB-TTL）。
 * 未连接时先攒进 3KB 环形缓冲，主机打开串口后补发，上电日志不丢。 */

#include <usb_cdc.h>

#if ENABLE_USB_CDC

#include <tusb.h>

#include <string.h>
#include <stm32f10x.h>

/* 1ms 时基（usb_board.c 里维护） */
#include <usb_board.h>

/* 3KB 待发缓冲：上电日志（含 HCI 自检的 TX/RX 内容）约 1.5KB，
 * 保留一倍余量，同时控制蓝牙协议栈与 FreeRTOS 的 RAM 占用。 */
#define CDC_LOG_RING_SIZE       3072U

static uint8_t  s_ring[CDC_LOG_RING_SIZE];
static uint16_t s_ring_head = 0U;       /* 写位置 */
static uint16_t s_ring_tail = 0U;       /* 读位置 */

static volatile uint8_t s_cdc_seen_connected = 0U;

/* 压一个字节进缓冲；满了就丢掉最老的（宁可丢开头也不丢最新的） */
static void cdc_ring_push(uint8_t byte)
{
    uint16_t next = (uint16_t)((s_ring_head + 1U) % CDC_LOG_RING_SIZE);

    if (next == s_ring_tail)
    {
        s_ring_tail = (uint16_t)((s_ring_tail + 1U) % CDC_LOG_RING_SIZE);
    }

    s_ring[s_ring_head] = byte;
    s_ring_head = next;
}

/* 把缓冲里的内容尽量推进 CDC FIFO，返回推了多少字节 */
static uint16_t cdc_ring_drain(void)
{
    uint16_t moved = 0U;

    while ((s_ring_tail != s_ring_head) && (tud_cdc_write_available() > 0U))
    {
        uint8_t  chunk[32];
        uint16_t n = 0U;

        while ((s_ring_tail != s_ring_head) && (n < (uint16_t)sizeof(chunk)))
        {
            chunk[n] = s_ring[s_ring_tail];
            s_ring_tail = (uint16_t)((s_ring_tail + 1U) % CDC_LOG_RING_SIZE);
            n++;
        }

        if (tud_cdc_write(chunk, (uint32_t)n) == 0U)
        {
            break;
        }

        moved = (uint16_t)(moved + n);
    }

    return moved;
}

void UsbCdc_Init(void)
{
    GPIO_InitTypeDef gpio;
    tusb_rhport_init_t dev_init = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_AUTO };

    /* 第 1 步：USB 时钟 = 72MHz / 1.5 = 48MHz。
     * RM0008 要求 USBPRE 必须在 USBEN 之前配好，顺序不能换。 */
    RCC_USBCLKConfig(RCC_USBCLKSource_PLLCLK_1Div5);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USB, ENABLE);

    /* 第 2 步：PA11/PA12 复用推挽。
     * 这一步会覆盖 UsbBoard_PreInit() 里"PA12 输出低电平"的设置，
     * 主机侧看到的才是一次正常的插入。 */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    gpio.GPIO_Pin   = GPIO_Pin_11 | GPIO_Pin_12;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode  = GPIO_Mode_AF_PP;
    GPIO_Init(GPIOA, &gpio);

    /* 第 3 步：毫秒时基 + USB 中断优先级（时基是幂等的，重复调没事） */
    UsbBoard_TimeBaseInit();
    NVIC_SetPriority(USB_LP_CAN1_RX0_IRQn, 2);
    NVIC_SetPriority(USB_HP_CAN1_TX_IRQn, 2);

    /* 第 4 步：启动 TinyUSB 设备协议栈 */
    tusb_init(0, &dev_init);
}

void UsbCdc_Task(void)
{
    /* 协议栈状态机：枚举、控制传输、端点事件全在这里推进 */
    tud_task();

    if (tud_cdc_connected())
    {
        if (s_cdc_seen_connected == 0U)
        {
            s_cdc_seen_connected = 1U;

            /* 先报一句"连接成功"，再补发上电期间的积压日志。
             * 这句是直接写 FIFO 的，不走缓冲，保证它排在最前面。 */
            (void)tud_cdc_write_str("\r\n[usb] CDC 串口已连接（波特率随意，8N1）\r\n");
        }

        /* 把积压日志推到 FIFO，再 flush 启动实际的 IN 传输。
         * 必须每轮都做：tud_cdc_write 只是往 FIFO 里塞，
         * flush 才会让 USB 外设把数据发出去。 */
        (void)cdc_ring_drain();
        tud_cdc_write_flush();
    }

    /* 主机发来的数据本工程暂时不用（以后可以做成命令行），
     * 但必须读走，否则 RX FIFO 满了端点会一直 NAK，主机侧写超时。 */
    while (tud_cdc_available() > 0U)
    {
        uint8_t junk[32];
        (void)tud_cdc_read(junk, (uint32_t)sizeof(junk));
    }
}

uint16_t UsbCdc_Write(const uint8_t *data, uint16_t len)
{
    uint32_t avail;

    if ((data == 0) || (len == 0U))
    {
        return 0U;
    }

    if (!tud_cdc_connected())
    {
        return 0U;
    }

    avail = tud_cdc_write_available();

    if (avail == 0U)
    {
        return 0U;                          /* FIFO 满：丢，绝不阻塞 */
    }

    if ((uint32_t)len > avail)
    {
        len = (uint16_t)avail;
    }

    return (uint16_t)tud_cdc_write(data, (uint32_t)len);
}

void UsbCdc_SendString(const char *s)
{
    if (s == 0)
    {
        return;
    }

    while (*s != '\0')
    {
        uint16_t n;

        if (!tud_cdc_connected())
        {
            /* 还没枚举完：先攒着，等连接后补发 */
            cdc_ring_push((uint8_t)*s);
            s++;
            continue;
        }

        n = UsbCdc_Write((const uint8_t *)s, (uint16_t)strlen(s));

        if (n == 0U)
        {
            /* FIFO 满了：剩下的也先攒进环形缓冲，下一轮 Task 再吐。
             * 这样即使日志爆发（比如 248 字节的名称命令整包 hex），
             * 也不会因为一次塞不进去就整段丢掉。 */
            cdc_ring_push((uint8_t)*s);
            s++;
            continue;
        }

        s += n;
    }
}

uint8_t UsbCdc_IsReady(void)
{
    return (tud_cdc_connected() != 0U) ? 1U : 0U;
}

#endif /* ENABLE_USB_CDC */
