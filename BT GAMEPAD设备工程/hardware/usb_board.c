/* 板级 USB 公共层：1ms 时基、D+ 预拉低、USB 中断入口 */

#include <stm32f10x.h>
#include <usb_board.h>
#include <tusb.h>

static volatile uint32_t s_millis = 0;

uint32_t tusb_time_millis_api(void)   // TinyUSB 无 RTOS 时要求的毫秒时基
{
    return s_millis;
}

uint32_t hal_time_ms(void)           /* 蓝牙协议栈与 FreeRTOS 任务共用的 1ms 时基 */
{
    return s_millis;
}

void TIM4_IRQHandler(void)           // 只计数，不占 SysTick（Delay 驱动在用）
{
    if ((TIM4->SR & TIM_SR_UIF) != 0U)
    {
        TIM4->SR = (uint16_t)~TIM_SR_UIF;
        s_millis++;
    }
}

static uint8_t s_timebase_started = 0;

void UsbBoard_TimeBaseInit(void)     // 幂等：重复调用不会重装定时器
{
    if (s_timebase_started != 0U)
    {
        return;
    }

    s_timebase_started = 1U;

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM4, ENABLE);

    TIM4->PSC = 72U - 1U;           // 72MHz / 72 = 1MHz
    TIM4->ARR = 1000U - 1U;         // 计 1000 个数 = 1ms
    TIM4->CNT = 0U;
    TIM4->EGR = TIM_EGR_UG;
    TIM4->SR  = 0U;

    TIM4->DIER |= TIM_DIER_UIE;
    TIM4->CR1  |= TIM_CR1_CEN;

    NVIC_SetPriority(TIM4_IRQn, 3);
    NVIC_EnableIRQ(TIM4_IRQn);
}

void UsbBoard_PreInit(void)
{
    GPIO_InitTypeDef gpio;

    // PA12(D+) 输出低：主机认为未插入，避免初始化期间枚举超时留下未知设备
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    gpio.GPIO_Pin   = GPIO_Pin_12;
    gpio.GPIO_Speed = GPIO_Speed_2MHz;
    gpio.GPIO_Mode  = GPIO_Mode_Out_PP;
    GPIO_Init(GPIOA, &gpio);
    GPIO_ResetBits(GPIOA, GPIO_Pin_12);
}

void USB_LP_CAN1_RX0_IRQHandler(void)
{
    dcd_int_handler(0);
}

void USB_HP_CAN1_TX_IRQHandler(void)
{
    dcd_int_handler(0);
}
