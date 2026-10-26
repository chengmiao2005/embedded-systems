#include <stdint.h>
#include <stdbool.h>

#include "inc/hw_memmap.h"

#include "driverlib/sysctl.h"
#include "driverlib/gpio.h"
#include "driverlib/pin_map.h"
#include "driverlib/ssi.h"


// ============================================================
// ST7735
// ============================================================

#define LCD_WIDTH       128
#define LCD_HEIGHT      160

// PB1 -> CS
#define LCD_CS_PORT     GPIO_PORTB_BASE
#define LCD_CS_PIN      GPIO_PIN_1

// PC6 -> DC
#define LCD_DC_PORT     GPIO_PORTC_BASE
#define LCD_DC_PIN      GPIO_PIN_6

// PE5 -> RES
#define LCD_RST_PORT    GPIO_PORTE_BASE
#define LCD_RST_PIN     GPIO_PIN_5


// ============================================================
// DHT11
//
// DATA -> PA6
// ============================================================

#define DHT_PORT        GPIO_PORTA_BASE
#define DHT_PIN         GPIO_PIN_6


// ============================================================
// RGB565 Color
// ============================================================

#define COLOR_BLACK     0x0000
#define COLOR_WHITE     0xFFFF
#define COLOR_RED       0xF800
#define COLOR_GREEN     0x07E0
#define COLOR_BLUE      0x001F
#define COLOR_YELLOW    0xFFE0
#define COLOR_CYAN      0x07FF


// ============================================================
// Debug variables
//
// x10 = 实际数值扩大10倍
//
// 例如：
// 243 -> 24.3
// 620 -> 62.0
// ============================================================

volatile uint8_t  g_dht_ok = 0;

volatile uint16_t g_temperature_x10 = 0;
volatile uint16_t g_humidity_x10 = 0;


// DHT11 原始数据包
volatile uint8_t g_raw0 = 0;
volatile uint8_t g_raw1 = 0;
volatile uint8_t g_raw2 = 0;
volatile uint8_t g_raw3 = 0;
volatile uint8_t g_raw4 = 0;


// ============================================================
// Delay
//
// System Clock = 80 MHz
// ============================================================

void DelayMs(uint32_t ms)
{
    while(ms--)
    {
        SysCtlDelay(26667);
    }
}


void DelayUs(uint32_t us)
{
    while(us--)
    {
        SysCtlDelay(27);
    }
}


// ============================================================
// SPI / SSI3
// ============================================================

void SPI_Write(uint8_t data)
{
    uint32_t dummy;

    SSIDataPut(SSI3_BASE, data);

    while(SSIBusy(SSI3_BASE))
    {
    }

    // SSI 是全双工
    // 清掉我们不需要的接收数据
    while(SSIDataGetNonBlocking(SSI3_BASE, &dummy))
    {
    }
}


// ============================================================
// LCD Command
//
// DC = 0 -> Command
// ============================================================

void LCD_WriteCommand(uint8_t cmd)
{
    // CS LOW
    GPIOPinWrite(LCD_CS_PORT,
                 LCD_CS_PIN,
                 0);

    // DC LOW = Command
    GPIOPinWrite(LCD_DC_PORT,
                 LCD_DC_PIN,
                 0);

    SPI_Write(cmd);

    // CS HIGH
    GPIOPinWrite(LCD_CS_PORT,
                 LCD_CS_PIN,
                 LCD_CS_PIN);
}


// ============================================================
// LCD Data
//
// DC = 1 -> Data
// ============================================================

void LCD_WriteData(uint8_t data)
{
    GPIOPinWrite(LCD_CS_PORT,
                 LCD_CS_PIN,
                 0);

    // DC HIGH = Data
    GPIOPinWrite(LCD_DC_PORT,
                 LCD_DC_PIN,
                 LCD_DC_PIN);

    SPI_Write(data);

    GPIOPinWrite(LCD_CS_PORT,
                 LCD_CS_PIN,
                 LCD_CS_PIN);
}


// ============================================================
// LCD Reset
// ============================================================

void LCD_Reset(void)
{
    // HIGH
    GPIOPinWrite(LCD_RST_PORT,
                 LCD_RST_PIN,
                 LCD_RST_PIN);

    DelayMs(10);


    // LOW
    GPIOPinWrite(LCD_RST_PORT,
                 LCD_RST_PIN,
                 0);

    DelayMs(20);


    // HIGH
    GPIOPinWrite(LCD_RST_PORT,
                 LCD_RST_PIN,
                 LCD_RST_PIN);

    DelayMs(120);
}


// ============================================================
// LCD Init
// ============================================================

void LCD_Init(void)
{
    LCD_Reset();


    // Software Reset
    LCD_WriteCommand(0x01);
    DelayMs(150);


    // Sleep Out
    LCD_WriteCommand(0x11);
    DelayMs(120);


    // 16-bit RGB565
    LCD_WriteCommand(0x3A);
    LCD_WriteData(0x05);


    // Memory Access Control
    LCD_WriteCommand(0x36);

    // 目前保持你已经成功工作的方向
    LCD_WriteData(0x00);


    // Display ON
    LCD_WriteCommand(0x29);

    DelayMs(100);
}


// ============================================================
// LCD Address Window
// ============================================================

void LCD_SetAddressWindow(uint8_t x0,
                          uint8_t y0,
                          uint8_t x1,
                          uint8_t y1)
{
    // Column Address
    LCD_WriteCommand(0x2A);

    LCD_WriteData(0x00);
    LCD_WriteData(x0);

    LCD_WriteData(0x00);
    LCD_WriteData(x1);


    // Row Address
    LCD_WriteCommand(0x2B);

    LCD_WriteData(0x00);
    LCD_WriteData(y0);

    LCD_WriteData(0x00);
    LCD_WriteData(y1);


    // Memory Write
    LCD_WriteCommand(0x2C);
}


// ============================================================
// Fill Rectangle
// ============================================================

void LCD_FillRect(uint8_t x,
                  uint8_t y,
                  uint8_t w,
                  uint8_t h,
                  uint16_t color)
{
    uint32_t i;
    uint32_t count;

    uint8_t highByte;
    uint8_t lowByte;


    if((w == 0) || (h == 0))
    {
        return;
    }


    highByte = (uint8_t)(color >> 8);
    lowByte  = (uint8_t)(color & 0xFF);


    LCD_SetAddressWindow(x,
                         y,
                         x + w - 1,
                         y + h - 1);


    // CS LOW
    GPIOPinWrite(LCD_CS_PORT,
                 LCD_CS_PIN,
                 0);


    // DC HIGH = Pixel Data
    GPIOPinWrite(LCD_DC_PORT,
                 LCD_DC_PIN,
                 LCD_DC_PIN);


    count = (uint32_t)w * (uint32_t)h;


    for(i = 0; i < count; i++)
    {
        SPI_Write(highByte);
        SPI_Write(lowByte);
    }


    // CS HIGH
    GPIOPinWrite(LCD_CS_PORT,
                 LCD_CS_PIN,
                 LCD_CS_PIN);
}


// ============================================================
// Fill Screen
// ============================================================

void LCD_FillScreen(uint16_t color)
{
    LCD_FillRect(0,
                 0,
                 LCD_WIDTH,
                 LCD_HEIGHT,
                 color);
}


// ============================================================
// 5 x 7 Font
// ============================================================

void GetCharPattern(char c, uint8_t p[5])
{
    uint8_t i;

    for(i = 0; i < 5; i++)
    {
        p[i] = 0;
    }


    switch(c)
    {
        case '0':
            p[0]=0x3E; p[1]=0x51; p[2]=0x49;
            p[3]=0x45; p[4]=0x3E;
            break;

        case '1':
            p[0]=0x00; p[1]=0x42; p[2]=0x7F;
            p[3]=0x40; p[4]=0x00;
            break;

        case '2':
            p[0]=0x42; p[1]=0x61; p[2]=0x51;
            p[3]=0x49; p[4]=0x46;
            break;

        case '3':
            p[0]=0x21; p[1]=0x41; p[2]=0x45;
            p[3]=0x4B; p[4]=0x31;
            break;

        case '4':
            p[0]=0x18; p[1]=0x14; p[2]=0x12;
            p[3]=0x7F; p[4]=0x10;
            break;

        case '5':
            p[0]=0x27; p[1]=0x45; p[2]=0x45;
            p[3]=0x45; p[4]=0x39;
            break;

        case '6':
            p[0]=0x3C; p[1]=0x4A; p[2]=0x49;
            p[3]=0x49; p[4]=0x30;
            break;

        case '7':
            p[0]=0x01; p[1]=0x71; p[2]=0x09;
            p[3]=0x05; p[4]=0x03;
            break;

        case '8':
            p[0]=0x36; p[1]=0x49; p[2]=0x49;
            p[3]=0x49; p[4]=0x36;
            break;

        case '9':
            p[0]=0x06; p[1]=0x49; p[2]=0x49;
            p[3]=0x29; p[4]=0x1E;
            break;


        // 小数点
        case '.':
            p[0]=0x00;
            p[1]=0x60;
            p[2]=0x60;
            p[3]=0x00;
            p[4]=0x00;
            break;


        case 'T':
            p[0]=0x01; p[1]=0x01; p[2]=0x7F;
            p[3]=0x01; p[4]=0x01;
            break;

        case 'E':
            p[0]=0x7F; p[1]=0x49; p[2]=0x49;
            p[3]=0x49; p[4]=0x41;
            break;

        case 'M':
            p[0]=0x7F; p[1]=0x02; p[2]=0x0C;
            p[3]=0x02; p[4]=0x7F;
            break;

        case 'P':
            p[0]=0x7F; p[1]=0x09; p[2]=0x09;
            p[3]=0x09; p[4]=0x06;
            break;

        case 'H':
            p[0]=0x7F; p[1]=0x08; p[2]=0x08;
            p[3]=0x08; p[4]=0x7F;
            break;

        case 'U':
            p[0]=0x3F; p[1]=0x40; p[2]=0x40;
            p[3]=0x40; p[4]=0x3F;
            break;

        case 'C':
            p[0]=0x3E; p[1]=0x41; p[2]=0x41;
            p[3]=0x41; p[4]=0x22;
            break;

        case '%':
            p[0]=0x63; p[1]=0x13; p[2]=0x08;
            p[3]=0x64; p[4]=0x63;
            break;

        case '-':
            p[0]=0x08; p[1]=0x08; p[2]=0x08;
            p[3]=0x08; p[4]=0x08;
            break;

        case ' ':
        default:
            break;
    }
}


// ============================================================
// Draw Character
// ============================================================

void LCD_DrawChar(uint8_t x,
                  uint8_t y,
                  char c,
                  uint16_t color,
                  uint16_t bg,
                  uint8_t scale)
{
    uint8_t pattern[5];

    uint8_t col;
    uint8_t row;


    GetCharPattern(c, pattern);


    for(col = 0; col < 5; col++)
    {
        for(row = 0; row < 7; row++)
        {
            if(pattern[col] & (1 << row))
            {
                LCD_FillRect(x + col * scale,
                             y + row * scale,
                             scale,
                             scale,
                             color);
            }
            else
            {
                LCD_FillRect(x + col * scale,
                             y + row * scale,
                             scale,
                             scale,
                             bg);
            }
        }
    }


    // Character spacing
    LCD_FillRect(x + 5 * scale,
                 y,
                 scale,
                 7 * scale,
                 bg);
}


// ============================================================
// Draw String
// ============================================================

void LCD_DrawString(uint8_t x,
                    uint8_t y,
                    const char *str,
                    uint16_t color,
                    uint16_t bg,
                    uint8_t scale)
{
    while(*str)
    {
        LCD_DrawChar(x,
                     y,
                     *str,
                     color,
                     bg,
                     scale);

        x += 6 * scale;

        str++;
    }
}


// ============================================================
// Degree Symbol
// ============================================================

void LCD_DrawDegree(uint8_t x,
                    uint8_t y,
                    uint16_t color)
{
    LCD_FillRect(x + 2, y,     4, 2, color);
    LCD_FillRect(x,     y + 2, 2, 4, color);
    LCD_FillRect(x + 6, y + 2, 2, 4, color);
    LCD_FillRect(x + 2, y + 6, 4, 2, color);
}


// ============================================================
// DHT11 Wait Level
// ============================================================

uint8_t DHT_WaitForLevel(uint8_t level,
                         uint32_t timeout_us)
{
    uint8_t current;


    while(timeout_us--)
    {
        if(GPIOPinRead(DHT_PORT, DHT_PIN))
        {
            current = 1;
        }
        else
        {
            current = 0;
        }


        if(current == level)
        {
            return 1;
        }


        DelayUs(1);
    }


    return 0;
}


// ============================================================
// DHT11 Read One Bit
//
// 这是你之前已经成功读出 24 / 62 的方法。
// 不再修改它。
// ============================================================

uint8_t DHT_ReadBit(uint8_t *bit)
{
    // 等待 HIGH
    if(!DHT_WaitForLevel(1, 120))
    {
        return 0;
    }


    // DHT11:
    //
    // 0 -> HIGH 大约 26~28 us
    // 1 -> HIGH 大约 70 us
    //
    // 在中间位置采样
    DelayUs(40);


    if(GPIOPinRead(DHT_PORT, DHT_PIN))
    {
        // 40 us 后仍然 HIGH
        *bit = 1;
    }
    else
    {
        // 40 us 前已经 LOW
        *bit = 0;
    }


    // 如果是 1，
    // 等 HIGH 结束
    if(*bit == 1)
    {
        if(!DHT_WaitForLevel(0, 120))
        {
            return 0;
        }
    }


    return 1;
}


// ============================================================
// DHT11 Read
//
// 输出扩大10倍：
//
// 240 -> 24.0
// 243 -> 24.3
// ============================================================

uint8_t DHT11_Read(uint16_t *temperature_x10,
                   uint16_t *humidity_x10)
{
    uint8_t data[5] = {0, 0, 0, 0, 0};

    uint8_t i;
    uint8_t j;
    uint8_t bit;


    // ========================================================
    // MCU Start Signal
    // ========================================================

    GPIOPinTypeGPIOOutput(DHT_PORT,
                          DHT_PIN);


    // DATA LOW
    GPIOPinWrite(DHT_PORT,
                 DHT_PIN,
                 0);


    // 至少拉低18 ms
    DelayMs(20);


    // DATA HIGH
    GPIOPinWrite(DHT_PORT,
                 DHT_PIN,
                 DHT_PIN);


    DelayUs(30);


    // ========================================================
    // 释放总线
    //
    // 改成 Input
    // ========================================================

    GPIOPinTypeGPIOInput(DHT_PORT,
                         DHT_PIN);


    GPIOPadConfigSet(DHT_PORT,
                     DHT_PIN,
                     GPIO_STRENGTH_2MA,
                     GPIO_PIN_TYPE_STD_WPU);


    // ========================================================
    // DHT11 Response
    //
    // LOW -> HIGH -> LOW
    // ========================================================

    if(!DHT_WaitForLevel(0, 120))
    {
        return 0;
    }


    if(!DHT_WaitForLevel(1, 120))
    {
        return 0;
    }


    if(!DHT_WaitForLevel(0, 120))
    {
        return 0;
    }


    // ========================================================
    // Read 40 bits
    //
    // 5 bytes x 8
    // ========================================================

    for(i = 0; i < 5; i++)
    {
        for(j = 0; j < 8; j++)
        {
            if(!DHT_ReadBit(&bit))
            {
                return 0;
            }


            data[i] <<= 1;


            if(bit)
            {
                data[i] |= 1;
            }
        }
    }


    // ========================================================
    // 保存原始数据
    // ========================================================

    g_raw0 = data[0];
    g_raw1 = data[1];
    g_raw2 = data[2];
    g_raw3 = data[3];
    g_raw4 = data[4];


    // ========================================================
    // Checksum
    // ========================================================

    if((uint8_t)(data[0] +
                 data[1] +
                 data[2] +
                 data[3]) != data[4])
    {
        return 0;
    }


    // ========================================================
    // DHT11 Format
    //
    // data[0] = Humidity integer
    // data[1] = Humidity decimal
    //
    // data[2] = Temperature integer
    // data[3] = Temperature decimal
    //
    // data[4] = Checksum
    // ========================================================


    // 湿度 x10
    *humidity_x10 =
        ((uint16_t)data[0] * 10) +
        (uint16_t)(data[1] % 10);


    // 温度 x10
    *temperature_x10 =
        ((uint16_t)data[2] * 10) +
        (uint16_t)(data[3] % 10);


    return 1;
}


// ============================================================
// Display Temperature
//
// 243 -> 24.3
// ============================================================

void DisplayTemperature(uint16_t value_x10)
{
    char text[5];

    uint16_t integer;
    uint8_t decimal;


    integer = value_x10 / 10;
    decimal = (uint8_t)(value_x10 % 10);


    text[0] = '0' + (integer / 10);
    text[1] = '0' + (integer % 10);

    text[2] = '.';

    text[3] = '0' + decimal;

    text[4] = '\0';


    // 只清温度数字区域
    LCD_FillRect(2,
                 34,
                 124,
                 40,
                 COLOR_BLACK);


    LCD_DrawString(3,
                   40,
                   text,
                   COLOR_YELLOW,
                   COLOR_BLACK,
                   3);


    // Degree Symbol
    LCD_DrawDegree(78,
                   40,
                   COLOR_YELLOW);


    // C
    LCD_DrawChar(91,
                 40,
                 'C',
                 COLOR_YELLOW,
                 COLOR_BLACK,
                 3);
}


// ============================================================
// Display Humidity
//
// 623 -> 62.3
// ============================================================

void DisplayHumidity(uint16_t value_x10)
{
    char text[5];

    uint16_t integer;
    uint8_t decimal;


    integer = value_x10 / 10;
    decimal = (uint8_t)(value_x10 % 10);


    text[0] = '0' + (integer / 10);
    text[1] = '0' + (integer % 10);

    text[2] = '.';

    text[3] = '0' + decimal;

    text[4] = '\0';


    // 只清湿度数字区域
    LCD_FillRect(2,
                 104,
                 124,
                 40,
                 COLOR_BLACK);


    LCD_DrawString(3,
                   110,
                   text,
                   COLOR_CYAN,
                   COLOR_BLACK,
                   3);


    LCD_DrawChar(82,
                 110,
                 '%',
                 COLOR_CYAN,
                 COLOR_BLACK,
                 3);
}


// ============================================================
// No Data
//
// 初始还没读到数据时显示：
//
// --.-
// ============================================================

void DisplayNoData(void)
{
    LCD_FillRect(2,
                 34,
                 124,
                 40,
                 COLOR_BLACK);


    LCD_DrawString(3,
                   40,
                   "--.-",
                   COLOR_RED,
                   COLOR_BLACK,
                   3);


    LCD_FillRect(2,
                 104,
                 124,
                 40,
                 COLOR_BLACK);


    LCD_DrawString(3,
                   110,
                   "--.-",
                   COLOR_RED,
                   COLOR_BLACK,
                   3);
}


// ============================================================
// MAIN
// ============================================================

int main(void)
{
    uint16_t temperature_x10 = 0;
    uint16_t humidity_x10    = 0;

    uint16_t last_temperature_x10 = 0xFFFF;
    uint16_t last_humidity_x10    = 0xFFFF;

    uint8_t fail_count = 0;

    uint8_t have_valid_data = 0;


    // ========================================================
    // 1. System Clock = 80 MHz
    // ========================================================

    SysCtlClockSet(SYSCTL_SYSDIV_2_5 |
                   SYSCTL_USE_PLL |
                   SYSCTL_OSC_MAIN |
                   SYSCTL_XTAL_16MHZ);


    // ========================================================
    // 2. Enable peripherals
    // ========================================================

    SysCtlPeripheralEnable(SYSCTL_PERIPH_SSI3);

    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOA);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOB);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOC);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOD);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOE);


    // ========================================================
    // 3. Wait until ready
    // ========================================================

    while(!SysCtlPeripheralReady(SYSCTL_PERIPH_SSI3))
    {
    }


    while(!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOA))
    {
    }


    while(!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOB))
    {
    }


    while(!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOC))
    {
    }


    while(!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOD))
    {
    }


    while(!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOE))
    {
    }


    // ========================================================
    // 4. SSI3 Pins
    //
    // PD0 -> SCK
    // PD3 -> MOSI / SDA
    // ========================================================

    GPIOPinConfigure(GPIO_PD0_SSI3CLK);

    GPIOPinConfigure(GPIO_PD3_SSI3TX);


    GPIOPinTypeSSI(GPIO_PORTD_BASE,
                   GPIO_PIN_0 |
                   GPIO_PIN_3);


    // ========================================================
    // 5. SSI3
    //
    // SPI Mode 0
    // Master
    // 4 MHz
    // 8 bits
    // ========================================================

    SSIConfigSetExpClk(SSI3_BASE,
                       SysCtlClockGet(),
                       SSI_FRF_MOTO_MODE_0,
                       SSI_MODE_MASTER,
                       4000000,
                       8);


    SSIEnable(SSI3_BASE);


    // ========================================================
    // 6. LCD Control Pins
    // ========================================================

    GPIOPinTypeGPIOOutput(LCD_CS_PORT,
                          LCD_CS_PIN);


    GPIOPinTypeGPIOOutput(LCD_DC_PORT,
                          LCD_DC_PIN);


    GPIOPinTypeGPIOOutput(LCD_RST_PORT,
                          LCD_RST_PIN);


    // CS HIGH
    GPIOPinWrite(LCD_CS_PORT,
                 LCD_CS_PIN,
                 LCD_CS_PIN);


    // RESET HIGH
    GPIOPinWrite(LCD_RST_PORT,
                 LCD_RST_PIN,
                 LCD_RST_PIN);


    // ========================================================
    // 7. DHT11 DATA
    // ========================================================

    GPIOPinTypeGPIOInput(DHT_PORT,
                         DHT_PIN);


    GPIOPadConfigSet(DHT_PORT,
                     DHT_PIN,
                     GPIO_STRENGTH_2MA,
                     GPIO_PIN_TYPE_STD_WPU);


    // ========================================================
    // 8. LCD Init
    // ========================================================

    LCD_Init();


    LCD_FillScreen(COLOR_BLACK);


    // ========================================================
    // Static UI
    //
    // 只画一次
    // ========================================================

    LCD_DrawString(10,
                   10,
                   "TEMP",
                   COLOR_WHITE,
                   COLOR_BLACK,
                   2);


    LCD_DrawString(10,
                   82,
                   "HUM",
                   COLOR_WHITE,
                   COLOR_BLACK,
                   2);


    // 初始没有数据
    DisplayNoData();


    // ========================================================
    // 9. 很重要：
    //
    // DHT11 上电后先稳定 2 秒
    // ========================================================

    DelayMs(2000);


    // ========================================================
    // 10. Main Loop
    // ========================================================

    while(1)
    {
        // ----------------------------------------------------
        // 读取 DHT11
        // ----------------------------------------------------

        g_dht_ok =
            DHT11_Read(&temperature_x10,
                       &humidity_x10);


        // ====================================================
        // SUCCESS
        // ====================================================

        if(g_dht_ok)
        {
            fail_count = 0;

            have_valid_data = 1;


            // 保存到全局变量
            // CCS Expressions 可以直接看
            g_temperature_x10 = temperature_x10;
            g_humidity_x10    = humidity_x10;


            // ------------------------------------------------
            // 只有温度变化才重画
            // ------------------------------------------------

            if(temperature_x10 != last_temperature_x10)
            {
                DisplayTemperature(temperature_x10);

                last_temperature_x10 =
                    temperature_x10;
            }


            // ------------------------------------------------
            // 只有湿度变化才重画
            // ------------------------------------------------

            if(humidity_x10 != last_humidity_x10)
            {
                DisplayHumidity(humidity_x10);

                last_humidity_x10 =
                    humidity_x10;
            }
        }


        // ====================================================
        // FAILURE
        // ====================================================

        else
        {
            fail_count++;


            // ------------------------------------------------
            // 如果以前已经读成功：
            //
            // 偶尔失败一次不清屏，
            // 保留上一次真实有效数据。
            // ------------------------------------------------

            if(have_valid_data)
            {
                if(fail_count >= 3)
                {
                    DisplayNoData();

                    last_temperature_x10 =
                        0xFFFF;

                    last_humidity_x10 =
                        0xFFFF;

                    have_valid_data = 0;

                    fail_count = 3;
                }
            }


            // ------------------------------------------------
            // 如果从开机开始一次都没成功：
            // 保持 --.-
            // ------------------------------------------------
        }


        // ====================================================
        // DHT11 更新周期
        //
        // 2.5秒
        // ========================================================

        DelayMs(2500);
    }
}
