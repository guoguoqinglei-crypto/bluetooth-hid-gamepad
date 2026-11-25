/* RDA5876 与 STM32 的 HCI 串口（USART2）：中断收 + 环形缓冲，阻塞发。
 * PA2=TX PA3=RX PA1=RTS(输出低,允许模块发) PA0=CTS(上拉输入,不用硬件流控)。 */
#ifndef __BT_UART_H
#define __BT_UART_H

#include <stdint.h>

/* 初始化 USART2。parity 取 USART_Parity_No / USART_Parity_Even，
 * 注意偶校验时必须用 9 位字长（8 数据位 + 1 校验位）。 */
void BtUart_Init(uint32_t baud, uint16_t parity);

/* 运行中改波特率/校验（补丁灌完从 115200 8N1 切到 921600 8E1） */
void BtUart_SetBaud(uint32_t baud, uint16_t parity);

/* 阻塞发送一整帧 */
void BtUart_Send(const uint8_t *data, uint16_t len);

/* 从环形缓冲取一个字节，超时返回 0 */
uint8_t BtUart_RxByte(uint8_t *out, uint16_t timeout_ms);

/* 当前缓冲区里有多少字节可读 */
uint16_t BtUart_Available(void);

/* 取一个字节（不等待），没有返回 0 */
uint8_t BtUart_TryRead(uint8_t *out);

/* 清空缓冲区 */
void BtUart_Flush(void);

/* 累计收到的字节数 / 因缓冲满或溢出丢掉的字节数 / 累计发出的字节数 */
uint32_t BtUart_RxTotal(void);
uint32_t BtUart_RxDropped(void);
uint32_t BtUart_TxTotal(void);

#endif
