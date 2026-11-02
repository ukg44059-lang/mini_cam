/**
  ******************************************************************************
  * @file    main.c
  * @brief   LAUNCHXL-CC1310 LED 闪烁 demo（红绿交替）
  *
  * 用 TI driverlib 裸机实现，不依赖 TI-RTOS / SysConfig / CCS。
  *
  * ---------------------------------------------------------------------------
  * CC13xx 点个灯为什么比 STM32 麻烦：电源域
  * ---------------------------------------------------------------------------
  * STM32 上开个 GPIO 时钟就能用了。CC1310 多一层【电源域】：
  * GPIO 属于 PERIPH 电源域，复位后这个域是**断电**的，
  * 不先把域上电，后面写 IOC/GPIO 寄存器全部无效（不报错，就是没反应）。
  *
  * 正确顺序是四步，少一步灯都不亮：
  *   1. PRCMPowerDomainOn(PRCM_DOMAIN_PERIPH)   给电源域上电
  *   2. 等 PRCMPowerDomainStatus() 变成 ON       上电要时间，必须轮询等
  *   3. PRCMPeripheralRunEnable(PRCM_PERIPH_GPIO) 打开 GPIO 模块时钟
  *   4. PRCMLoadSet() + 等 PRCMLoadGet()         ★ 最容易漏的一步 ★
  *
  * 第 4 步是 CC13xx 特有的：时钟使能寄存器写完【不会立刻生效】，
  * 必须再写一次 CLKLOADCTL 触发"加载"，硬件才真正把设置应用下去。
  * 漏了它的现象和"引脚接错"一模一样 —— 灯不亮、读回的寄存器却看着是对的。
  *
  * ---------------------------------------------------------------------------
  * 延时为什么用 CPUdelay 而不是 SysTick
  * ---------------------------------------------------------------------------
  * 这个 demo 刻意不开中断、不配时钟树，就是想把"能不能点亮"这件事的
  * 变量降到最少。CPUdelay(n) 是 driverlib 提供的固定 3 周期/次的忙等循环，
  * 复位后 CPU 跑在 48MHz RCOSC 上，所以 1ms ≈ 48000/3 = 16000 次。
  ******************************************************************************
  */

#include <stdint.h>
#include <stdbool.h>

#include <driverlib/prcm.h>
#include <driverlib/ioc.h>
#include <driverlib/gpio.h>
#include <driverlib/cpu.h>

#include "board.h"

/* 复位后主频是 48 MHz 的 RCOSC_HF。CPUdelay 每次循环 3 个 CPU 周期 */
#define CPU_HZ              48000000U
#define DELAY_MS(ms)        CPUdelay(((CPU_HZ / 1000U) / 3U) * (ms))

/**
  * @brief 给外设电源域上电并打开 GPIO 时钟
  * @note  四步缺一不可，尤其是最后的 PRCMLoadSet，见文件头说明
  */
static void periph_power_on(void)
{
    /* 1. 外设电源域上电 */
    PRCMPowerDomainOn(PRCM_DOMAIN_PERIPH);

    /* 2. 等它真的上电完成。不等就往下走，后面的寄存器写全是无效的 */
    while (PRCMPowerDomainStatus(PRCM_DOMAIN_PERIPH) != PRCM_DOMAIN_POWER_ON)
    {
        /* 忙等 */
    }

    /* 3. 打开 GPIO 模块在 RUN 模式下的时钟 */
    PRCMPeripheralRunEnable(PRCM_PERIPH_GPIO);

    /* 4. ★ 触发时钟设置加载，并等它生效 ★
          CC13xx 的时钟使能是"写寄存器 + 显式加载"两段式的，
          少了这一步前面那句 RunEnable 等于没写 */
    PRCMLoadSet();
    while (!PRCMLoadGet())
    {
        /* 忙等 */
    }
}

/**
  * @brief 把一个 DIO 配成推挽输出
  * @note  IOCPinTypeGpioOutput 内部做了两件事：
  *          - 把该 DIO 的 IOC 端口复用设成 GPIO
  *          - 在 GPIO 模块里使能这个脚的输出驱动(DOE)
  *        只做其中一件的话，引脚会停在高阻态，灯不会亮。
  */
static void led_init(uint32_t dio)
{
    IOCPinTypeGpioOutput(dio);
    GPIO_clearDio(dio);          /* 初始灭 */
}

int main(void)
{
    uint32_t n = 0;

    periph_power_on();

    led_init(BOARD_LED_RED);
    led_init(BOARD_LED_GREEN);

    while (1)
    {
        /*
         * 红绿交替闪，各 250ms。
         * 两个都闪是有意的：万一板子上红绿位置和文档记的相反，
         * 也能一眼看出来"两个灯都在动"，不至于怀疑是引脚配错了。
         */
        GPIO_setDio(BOARD_LED_RED);
        GPIO_clearDio(BOARD_LED_GREEN);
        DELAY_MS(250);

        GPIO_clearDio(BOARD_LED_RED);
        GPIO_setDio(BOARD_LED_GREEN);
        DELAY_MS(250);

        n++;
        (void)n;
    }
}
