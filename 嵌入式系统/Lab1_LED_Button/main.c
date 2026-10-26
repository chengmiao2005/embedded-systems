#include <stdbool.h>
#include <stdint.h>

#include "inc/hw_memmap.h"
#include "inc/hw_gpio.h"
#include "inc/hw_types.h"

#include "driverlib/sysctl.h"
#include "driverlib/gpio.h"


// LED状态
// 0 = 红
// 1 = 黄
// 2 = 蓝
// 3 = 熄灭
int state = 0;


// 根据state设置LED颜色
void SetLED(int color)
{
    switch(color)
    {
        // 红色：PF1
        case 0:
            GPIOPinWrite(GPIO_PORTF_BASE,
                         GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3,
                         GPIO_PIN_1);
            break;

        // 黄色：红 + 绿 = PF1 + PF3
        case 1:
            GPIOPinWrite(GPIO_PORTF_BASE,
                         GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3,
                         GPIO_PIN_1 | GPIO_PIN_3);
            break;

        // 蓝色：PF2
        case 2:
            GPIOPinWrite(GPIO_PORTF_BASE,
                         GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3,
                         GPIO_PIN_2);
            break;

        // 熄灭
        case 3:
            GPIOPinWrite(GPIO_PORTF_BASE,
                         GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3,
                         0);
            break;
    }
}


int main(void)
{
    //==================================================
    // 1. 开启 GPIO Port F
    //==================================================
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOF);

    while(!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOF))
    {
    }


    //==================================================
    // 2. 解锁 PF0
    //
    // 左边按键 SW2 使用 PF0
    // PF0 默认被锁定，需要先解锁
    //==================================================
    HWREG(GPIO_PORTF_BASE + GPIO_O_LOCK) = GPIO_LOCK_KEY;
    HWREG(GPIO_PORTF_BASE + GPIO_O_CR) |= GPIO_PIN_0;
    HWREG(GPIO_PORTF_BASE + GPIO_O_LOCK) = 0;


    //==================================================
    // 3. 设置板载 RGB LED
    //
    // PF1 = 红
    // PF2 = 蓝
    // PF3 = 绿
    //==================================================
    GPIOPinTypeGPIOOutput(GPIO_PORTF_BASE,
                          GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3);


    //==================================================
    // 4. 设置两个按键
    //
    // SW2 = PF0  左边
    // SW1 = PF4  右边
    //==================================================
    GPIOPinTypeGPIOInput(GPIO_PORTF_BASE,
                         GPIO_PIN_0 | GPIO_PIN_4);


    // 使用内部上拉电阻
    //
    // 没按 = 1
    // 按下 = 0
    GPIOPadConfigSet(GPIO_PORTF_BASE,
                     GPIO_PIN_0 | GPIO_PIN_4,
                     GPIO_STRENGTH_2MA,
                     GPIO_PIN_TYPE_STD_WPU);


    //==================================================
    // 5. 初始状态：红灯
    //==================================================
    state = 0;
    SetLED(state);


    //==================================================
    // 6. 主循环
    //==================================================
    while(1)
    {
        //==============================================
        // 左边按键 SW2 / PF0
        //
        // 正向：
        // 红 → 黄 → 蓝 → 灭 → 红
        //==============================================
        if(GPIOPinRead(GPIO_PORTF_BASE, GPIO_PIN_0) == 0)
        {
            // 按下消抖
            SysCtlDelay(100000);

            // 一直等待按键松开
            // 所以“按下时”不会换颜色
            while(GPIOPinRead(GPIO_PORTF_BASE, GPIO_PIN_0) == 0)
            {
            }

            // 松开消抖
            SysCtlDelay(100000);

            // 松开后才改变状态
            state++;

            if(state > 3)
            {
                state = 0;
            }

            SetLED(state);
        }


        //==============================================
        // 右边按键 SW1 / PF4
        //
        // 反向：
        // 红 → 灭 → 蓝 → 黄 → 红
        //==============================================
        if(GPIOPinRead(GPIO_PORTF_BASE, GPIO_PIN_4) == 0)
        {
            // 按下消抖
            SysCtlDelay(100000);

            // 等待松开
            while(GPIOPinRead(GPIO_PORTF_BASE, GPIO_PIN_4) == 0)
            {
            }

            // 松开消抖
            SysCtlDelay(100000);

            // 松开后往回切
            state--;

            if(state < 0)
            {
                state = 3;
            }

            SetLED(state);
        }
    }
}
