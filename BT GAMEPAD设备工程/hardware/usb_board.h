#ifndef _USB_BOARD_H
#define _USB_BOARD_H

#include <stdint.h>

/* 板级 USB 公共层：1ms 时基（TIM4）、D+ 预拉低、USB 中断入口 */

void UsbBoard_PreInit(void);        // PA12(D+) 拉低，初始化期间主机看不到设备，须在 main() 最先调用
void UsbBoard_TimeBaseInit(void);   // 启动 1ms 时基，幂等
uint32_t hal_time_ms(void);         // 上电起累计毫秒数

#endif /* _USB_BOARD_H */
