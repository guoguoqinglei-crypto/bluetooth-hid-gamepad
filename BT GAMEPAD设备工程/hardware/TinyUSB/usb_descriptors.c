/* USB CDC 虚拟串口描述符（蓝牙手柄工程的调试日志出口） */

#include <string.h>

#include <tusb.h>

#define USB_VID             0xCafe
#define USB_PID             0x4003      // CDC 专用 PID，避免与旧 HID 驱动绑定冲突
#define USB_DEV_CLASS       0x02        // bDeviceClass = CDC（通信设备类）
#define USB_BCD             0x0200

static const tusb_desc_device_t desc_device =
{
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = USB_BCD,

    .bDeviceClass       = USB_DEV_CLASS,   // 设备级标 0x02，Windows 才认作串口
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,

    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,

    .bNumConfigurations = 0x01
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&desc_device;
}

// CDC 必须拆两个接口：接口 0 通信类（控制/通知），接口 1 数据类（真正收发）
#define CONFIG_TOTAL_LEN    67
#define CONFIG_NUM_ITF      2

static uint8_t const desc_configuration[] =
{
    0x09, 0x02, 0x43, 0x00, 0x02, 0x01, 0x00, 0x80, 0x32,   // 配置：wTotalLength=67, 2 接口, 100mA
    0x09, 0x04, 0x00, 0x00, 0x01, 0x02, 0x02, 0x01, 0x00,   // 接口 0 通信类 (CDC ACM, AT 命令)
    0x05, 0x24, 0x00, 0x10, 0x01,                             // CDC Header 1.10
    0x05, 0x24, 0x01, 0x00, 0x01,                             // Call Management, 数据在接口 1
    0x04, 0x24, 0x02, 0x02,                                    // ACM: 支持 Line Coding / 状态通知
    0x05, 0x24, 0x06, 0x00, 0x01,                             // Union: 接口 0(主) + 接口 1(从)
    0x07, 0x05, 0x81, 0x03, 0x10, 0x00, 0x0A,                // 通知端点 0x81, 中断, 16B, 10ms
    0x09, 0x04, 0x01, 0x00, 0x02, 0x0A, 0x00, 0x00, 0x00,   // 接口 1 数据类 (CDC-Data), 2 端点
    0x07, 0x05, 0x02, 0x02, 0x40, 0x00, 0x00,                // 批量 OUT 0x02, 64B（主机 -> 板子）
    0x07, 0x05, 0x82, 0x02, 0x40, 0x00, 0x00,                // 批量 IN 0x82, 64B（日志出口）
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_configuration;
}

// 数组实际长度必须等于 wTotalLength，写错一字节就编译报错
TU_VERIFY_STATIC(sizeof(desc_configuration) == CONFIG_TOTAL_LEN,
                 "configuration descriptor length mismatch");

static const char *const string_desc_arr[] =
{
    (const char[]){0x09, 0x04},     // 0: 语言 ID（英文 - 美国）
    "STM32 BT Gamepad",             // 1: 厂商
    "STM32 BT Debug (CDC)",         // 2: 产品名，显示在"端口(COM 和 LPT)"栏
    "000001",                       // 3: 序列号
};

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;

    static uint16_t desc_str[32];
    uint8_t chr_count;

    if (index == 0)
    {
        desc_str[1] = 0x0409;
        chr_count = 1;
    }
    else
    {
        if (index >= (uint8_t)(sizeof(string_desc_arr) / sizeof(string_desc_arr[0])))
        {
            return NULL;
        }

        const char *str = string_desc_arr[index];
        chr_count = (uint8_t)strlen(str);
        if (chr_count > 31)
        {
            chr_count = 31;
        }

        for (uint8_t i = 0; i < chr_count; i++)
        {
            desc_str[1 + i] = (uint16_t)str[i];
        }
    }

    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2U * chr_count + 2U));
    return desc_str;
}
