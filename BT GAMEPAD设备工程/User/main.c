/* 蓝牙手柄主程序：STM32F103RCT6 + RDA5876，FreeRTOS 双任务调度 */

#include <stdint.h>

/* ---- FreeRTOS ---- */
#include <FreeRTOS.h>
#include <task.h>

#include <uart.h>           /* 调试串口 USART1（PA9/PA10） */
#include <usb_board.h>      /* 1ms 时基与 USB D+ 预拉低 */
#include <usb_cdc.h>        /* USB CDC 虚拟串口日志 */
#include <bt_uart.h>        /* USART2：RDA5876 HCI 串口（PA2/PA3） */
#include <Init_rda5876.h>   /* 芯片上电时序与厂商补丁下载 */
#include <bt_hci.h>         /* HCI / L2CAP / SDP / HID 主机协议栈 */
#include <joystick.h>       /* 四轴摇杆与 L3/R3 */
#include <key.h>            /* A/B/X/Y/UP/DN/LF/RG 八个按键 */
#include <tim2.h>           /* PC7/PC8/PC9 软件 PWM RGB */

#define UART_BAUD_RATE      921600  /* 调试串口波特率 */

/* ============================================================================
 * FreeRTOS 任务划分（周期均为 1ms）
 * ----------------------------------------------------------------------------
 * BT_TASK  优先级 3：处理 HCI 收发、连接维护、输入采样与手柄报告。
 * CDC_TASK 优先级 2：推进 TinyUSB 协议栈并发送调试日志。
 *
 * HCI 串口通过中断写入环形缓冲区，BT_TASK 周期解析；手柄报告周期为 20ms，
 * 由 BT_Task() 内部根据 hal_time_ms() 独立控制。
 * ========================================================================== */

#define BT_TASK_PRIO        3u
#define BT_TASK_STACK       512u        /* 512 字 = 2KB（HCI/L2CAP/SDP 调用链较深） */
#define CDC_TASK_PRIO       2u
#define CDC_TASK_STACK      256u        /* 256 字 = 1KB */
#define TASK_PERIOD_MS      1u

/* 蓝牙主任务：收包、回连、采样并发送手柄报告 */
static void vBtTask(void *pvParameters)
{
    uint8_t lastState = 0xFFu;

    (void)pvParameters;

    for (;;)
    {
        BT_Task();

        /* 状态变化才打印，避免刷屏 */
        if (g_bt_state != lastState)
        {
            lastState = (uint8_t)g_bt_state;
            // 0=初始化 1=可被发现 2=已连接 3=HID 通道就绪
            UART1_Printf("[bt] state=%u reports=%lu\r\n",
                         (unsigned)g_bt_state,
                         (unsigned long)g_bt_reports);
        }

        vTaskDelay(TASK_PERIOD_MS);
    }
}

#if ENABLE_USB_CDC
/* USB CDC 任务：tud_task + 日志出队，优先级低于蓝牙 */
static void vCdcTask(void *pvParameters)
{
    (void)pvParameters;

    for (;;)
    {
        UsbCdc_Task();
        vTaskDelay(TASK_PERIOD_MS);
    }
}
#endif

/* ============================================================================
 * FreeRTOS 钩子
 * ========================================================================== */

void vApplicationMallocFailedHook(void)
{
    taskDISABLE_INTERRUPTS();
    UART1_SendString("\r\n[malloc FAILED] heap exhausted!\r\n");
    for (;;)
    {
    }
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    taskDISABLE_INTERRUPTS();
    UART1_Printf("\r\n[stack OVERFLOW] task=%s\r\n", pcTaskName);
    for (;;)
    {
    }
}

/* ============================================================================
 * main：硬件初始化（阻塞，调度器启动之前）→ 创建任务 → 启动调度器
 * ========================================================================== */

int main(void)
{
    uint32_t t0;
    uint32_t btMs;
    uint8_t  bt;

    /* NVIC 优先级分组必须先设成 Group 4（4 位抢占、无子优先级），
     * FreeRTOS 的 configMAX_SYSCALL_INTERRUPT_PRIORITY 屏蔽关系依赖它。 */
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);

    UsbBoard_PreInit();             // PA12(D+) 拉低，初始化期间主机看不到设备
    UART1_Init(UART_BAUD_RATE);
    UsbBoard_TimeBaseInit();        // 1ms 时基，蓝牙超时判断都读 hal_time_ms()

    // 手柄输入必须在蓝牙上电前跑：Joystick_Init() 在此记下摇杆中位
    Joystick_Init();
    Key_Init();
    Tim2_Init();                    // RGB 三路 PWM
    UART1_SendString("joystick + keys + rgb pwm ready\r\n");

    // 阻塞约 1.5s：复位/LDO -> 32.768kHz -> 灌五张补丁表 -> 切 921600 8E1
    t0   = hal_time_ms();
    bt   = Init_rda5876();
    btMs = hal_time_ms() - t0;

    UART1_Printf("[bt] init=%s stage=%u ms=%lu tx=%lu rx=%lu drop=%lu\r\n",
                 (bt == RDA_OK) ? "OK" : "FAIL",
                 (unsigned)rda_stage,
                 (unsigned long)btMs,
                 (unsigned long)BtUart_TxTotal(),
                 (unsigned long)BtUart_RxTotal(),
                 (unsigned long)BtUart_RxDropped());

    if (bt == RDA_OK)
    {
        (void)BT_Probe();           // 链路自检，正常第一组就命中
        BT_Init();                  // 18 条初始化命令：扫描、名称、CoD、事件掩码

        if (g_bt_error == 0u)
        {
            UART1_SendString("[bt] hci host stack up (HID gamepad)\r\n");
        }
    }
    else
    {
        UART1_SendString("[bt] controller init failed, see stage above\r\n");
    }

#if ENABLE_USB_CDC
    // 放在蓝牙之后：枚举时主机看到的是干净插入，日志走 3KB 环形缓冲补发
    UsbCdc_Init();
    UART1_SendString("USB CDC started (look for a new COM port)\r\n");
#endif

    /* 创建任务并启动调度器 */
    UART1_Printf("[rtos] heap free before tasks = %u bytes\r\n",
                 (unsigned)xPortGetFreeHeapSize());

    (void)xTaskCreate(vBtTask, "BT", BT_TASK_STACK, NULL, BT_TASK_PRIO, NULL);
#if ENABLE_USB_CDC
    (void)xTaskCreate(vCdcTask, "CDC", CDC_TASK_STACK, NULL, CDC_TASK_PRIO, NULL);
#endif

    UART1_Printf("[rtos] heap free after tasks  = %u bytes\r\n",
                 (unsigned)xPortGetFreeHeapSize());
    UART1_SendString("[rtos] starting scheduler\r\n");

    vTaskStartScheduler();          // 正常情况下不会返回

    UART1_SendString("[rtos] scheduler start FAILED\r\n");
    for (;;)
    {
    }
}
