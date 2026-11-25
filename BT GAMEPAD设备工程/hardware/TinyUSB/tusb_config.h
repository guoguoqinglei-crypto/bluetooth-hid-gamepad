#ifndef TUSB_CONFIG_H_
#define TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

/* TinyUSB 配置：蓝牙手柄，USB 口只做 CDC 虚拟串口（打印调试日志） */

#define CFG_TUSB_MCU                OPT_MCU_STM32F1         // STM32F1 USB FS Device
#ifndef STM32F103xE
#define STM32F103xE                                         // 256KB Flash，xE 密度
#endif

/* TinyUSB 不启用内置 RTOS 抽象，由 FreeRTOS 的 CDC 任务周期推进协议栈 */
#define CFG_TUSB_OS                 OPT_OS_NONE
#define CFG_TUSB_DEBUG              0
#define CFG_TUSB_RHPORT0_MODE       (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUD_ENABLED             1
#define CFG_TUD_MAX_SPEED           OPT_MODE_FULL_SPEED
#define CFG_TUD_ENDPOINT0_SIZE      64                      // 全速端点 0 固定 64 字节

#define ENABLE_USB_CDC              1                       // USB 口当虚拟串口用
#define CFG_TUD_CDC                 ENABLE_USB_CDC
#define CFG_TUD_CDC_EP_BUFSIZE      64                      // 与描述符 wMaxPacketSize 一致
#define CFG_TUD_CDC_RX_BUFSIZE      256
#define CFG_TUD_CDC_TX_BUFSIZE      256

#define ENABLE_HID_DEMO             0                       // USB HID 调试接口，关闭
#define CFG_TUD_HID                 ENABLE_HID_DEMO
#define CFG_TUD_HID_EP_BUFSIZE      64

#define CFG_TUD_AUDIO               0                       // 音频类关闭

/* 标准外设库未导出，显式给出 */
#define FSDEV_REG_BASE              0x40005C00UL
#define FSDEV_PMA_BASE              0x40006000UL

#ifdef __cplusplus
}
#endif

#endif /* TUSB_CONFIG_H_ */
