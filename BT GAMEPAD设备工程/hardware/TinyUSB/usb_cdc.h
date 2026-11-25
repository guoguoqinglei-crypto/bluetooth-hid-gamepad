/* USB CDC 虚拟串口（开关 ENABLE_USB_CDC，见 tusb_config.h） */

#ifndef _USB_CDC_H
#define _USB_CDC_H

/* 只为了拿到 ENABLE_USB_CDC / CFG_TUD_CDC 这几个宏。
 * tusb_config.h 本身是纯宏文件，不含任何 include，单独引它没有副作用。 */
#include <tusb_config.h>

#include <stdint.h>

#if ENABLE_USB_CDC

/* 初始化 USB 时钟、PA11/PA12、中断优先级并启动 TinyUSB。
 * 必须在 UsbBoard_PreInit() 之后调用（那个函数先把 D+ 拉低，
 * 这里再切换成 USB 外设复用功能，主机才会看到一次“插入”）。 */
void UsbCdc_Init(void);

/* 由 FreeRTOS 的 CDC 任务周期调用：推进 TinyUSB 状态机并发送 FIFO 数据。
 * 每轮都要调，不能加延时，否则枚举会超时。 */
void UsbCdc_Task(void);

/* 非阻塞写。主机没连上、或 FIFO 满了，就直接返回实际写入的字节数（可能是 0），
 * 绝不等待——调试日志丢几个字节比卡住蓝牙时序好得多。 */
uint16_t UsbCdc_Write(const uint8_t *data, uint16_t len);

/* 非阻塞写字符串（给 UART1_SendString 做镜像用） */
void UsbCdc_SendString(const char *s);

/* 1 = 主机已打开串口（收到过 SET_CONTROL_LINE_STATE 的 DTR） */
uint8_t UsbCdc_IsReady(void);

#endif /* ENABLE_USB_CDC */

#endif /* _USB_CDC_H */
