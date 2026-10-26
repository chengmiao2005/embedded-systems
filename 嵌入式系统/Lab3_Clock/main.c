/*
 * EIE331 Lab 3 - Timer driven analog clock
 * Target: TM4C123GH6PM / EK-TM4C123GXL, CCS + TivaWare DriverLib.
 * Display assumption: ST7735R/S, 128 x 160 portrait, 4-wire SPI.
 * Wiring: SCK=PD0, MOSI=PD3, CS=PB1, DC=PC6, RST=PE5.
 * No LVGL, external LCD library, floating point or heap required.
 * Keep the project's startup file, linker .cmd file and driverlib.lib.
 * Hardware validation is still required on your particular LCD module.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#ifndef LAB3_HOST_TEST
#include "inc/hw_memmap.h"
#include "driverlib/sysctl.h"
#include "driverlib/gpio.h"
#include "driverlib/pin_map.h"
#include "driverlib/ssi.h"
#include "driverlib/timer.h"
#include "driverlib/interrupt.h"
#endif

/* ---------------- User settings ---------------- */
#ifndef START_HOUR
#define START_HOUR             10U
#endif
#ifndef START_MINUTE
#define START_MINUTE           9U
#endif
#ifndef START_SECOND
#define START_SECOND           50U
#endif

/* Normal=1. Change to 60 ONLY to demonstrate carry at 60x speed. */
#ifndef CLOCK_SPEED
#define CLOCK_SPEED            1U
#endif

#define CLOCK_TICK_HZ          100U
#define DISPLAY_FPS            20U
#define DISPLAY_PERIOD_TICKS   (CLOCK_TICK_HZ / DISPLAY_FPS)

/* Default: common red-tab style 128x160 ST7735 module.
 * Some green-tab modules need X_OFFSET=2, Y_OFFSET=1.
 * MADCTL 0xC8 -> 0xC0 swaps BGR/RGB order if red/blue are reversed.
 * With the same offsets, 0xC8 -> 0x08 rotates portrait by 180 degrees.
 * These values MUST match the actual module; this is not auto-detection.
 */
#define LCD_WIDTH              128
#define LCD_HEIGHT             160
#define LCD_X_OFFSET           0U
#define LCD_Y_OFFSET           0U
#define LCD_MADCTL             0xC8U
#define LCD_INVERT             0U
#define LCD_SPI_HZ             6250000U

#if START_HOUR > 23 || START_MINUTE > 59 || START_SECOND > 59
#error Invalid start time
#endif
#if CLOCK_SPEED < 1 || CLOCK_SPEED > 100
#error CLOCK_SPEED must be between 1 and 100
#endif
#if DISPLAY_FPS < 1 || DISPLAY_FPS > CLOCK_TICK_HZ
#error Invalid display refresh rate
#endif
#if CLOCK_TICK_HZ % DISPLAY_FPS != 0
#error DISPLAY_FPS must divide CLOCK_TICK_HZ
#endif

/* Two packed 4-bit buffers = 20,480 bytes total, not two RGB565 frames.
 * The LCD receives RGB565; the MCU only stores small palette indices.
 */
#define FRAME_BYTES ((LCD_WIDTH * LCD_HEIGHT) / 2)
#define CX 64
#define CY 64
#define RADIUS 60

enum { BLACK, WHITE, GREY, CYAN, YELLOW, RED, NAVY, PALETTE_SIZE };
static const uint16_t g_palette[PALETTE_SIZE] = {
    0x0000U, 0xFFFFU, 0x8410U, 0x07FFU, 0xFFE0U, 0xF800U, 0x0841U
};
static uint8_t g_frames[2][FRAME_BYTES];
static uint8_t *g_back = g_frames[0];
static uint8_t *g_front = g_frames[1];

typedef struct {
    uint32_t ticks;             /* Real 10ms ticks, independent of demo speed. */
    uint8_t hour, minute, second, fraction;
} ClockSnapshot;

static volatile uint32_t g_ticks = 0U;
static volatile uint8_t g_hour = START_HOUR;
static volatile uint8_t g_minute = START_MINUTE;
static volatile uint8_t g_second = START_SECOND;
static volatile uint8_t g_fraction = 0U;

/* Called once per hardware 10ms tick. No LCD operation in this function. */
static void ClockTick100Hz(void)
{
    uint16_t fraction;
    ++g_ticks;
    fraction = (uint16_t)(g_fraction + CLOCK_SPEED);
    if (fraction >= CLOCK_TICK_HZ) {
        fraction -= CLOCK_TICK_HZ;
        if (++g_second >= 60U) {
            g_second = 0U;
            if (++g_minute >= 60U) {
                g_minute = 0U;
                if (++g_hour >= 24U) g_hour = 0U;
            }
        }
    }
    g_fraction = (uint8_t)fraction;
}

static ClockSnapshot ClockRead(void)
{
    ClockSnapshot t;
#ifndef LAB3_HOST_TEST
    bool wasDisabled = IntMasterDisable();
#endif
    t.ticks = g_ticks;
    t.hour = g_hour;
    t.minute = g_minute;
    t.second = g_second;
    t.fraction = g_fraction;
#ifndef LAB3_HOST_TEST
    if (!wasDisabled) IntMasterEnable();
#endif
    return t;
}

/* ---------------- ST7735 + Timer0A hardware ---------------- */
#ifndef LAB3_HOST_TEST
static uint32_t g_systemClock;

static void DelayMs(uint32_t ms)
{
    /* Initialization only. Timekeeping NEVER uses this delay. */
    while (ms-- != 0U) SysCtlDelay(g_systemClock / 3000U);
}

static void PeripheralEnable(uint32_t peripheral)
{
    SysCtlPeripheralEnable(peripheral);
    while (!SysCtlPeripheralReady(peripheral)) { }
}

static void LcdCS(bool high)
{
    GPIOPinWrite(GPIO_PORTB_BASE, GPIO_PIN_1, high ? GPIO_PIN_1 : 0U);
}

static void LcdDC(bool data)
{
    GPIOPinWrite(GPIO_PORTC_BASE, GPIO_PIN_6, data ? GPIO_PIN_6 : 0U);
}

static void SpiWrite(uint8_t data)
{
    uint32_t received;
    SSIDataPut(SSI3_BASE, data);
    /* One receive for every transmit prevents RX FIFO overflow.
     * No MISO wiring is needed; the received value is discarded. */
    SSIDataGet(SSI3_BASE, &received);
    while (SSIBusy(SSI3_BASE)) { }
}

static void LcdCommand(uint8_t command, const uint8_t *data, uint32_t count)
{
    uint32_t i;
    LcdCS(false);
    LcdDC(false);
    SpiWrite(command);
    LcdDC(true);
    for (i = 0U; i < count; ++i) SpiWrite(data[i]);
    LcdCS(true);
}

static void LcdCommand1(uint8_t command, uint8_t value)
{
    LcdCommand(command, &value, 1U);
}

static void LcdInit(void)
{
    uint32_t dummy;
    static const uint8_t frameNormal[] = {0x01, 0x2C, 0x2D};
    static const uint8_t framePartial[] = {0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D};
    static const uint8_t power1[] = {0xA2, 0x02, 0x84};
    static const uint8_t power3[] = {0x0A, 0x00};
    static const uint8_t power4[] = {0x8A, 0x2A};
    static const uint8_t power5[] = {0x8A, 0xEE};

    PeripheralEnable(SYSCTL_PERIPH_GPIOB);
    PeripheralEnable(SYSCTL_PERIPH_GPIOC);
    PeripheralEnable(SYSCTL_PERIPH_GPIOD);
    PeripheralEnable(SYSCTL_PERIPH_GPIOE);
    PeripheralEnable(SYSCTL_PERIPH_SSI3);

    GPIOPinTypeGPIOOutput(GPIO_PORTB_BASE, GPIO_PIN_1);
    GPIOPinTypeGPIOOutput(GPIO_PORTC_BASE, GPIO_PIN_6);
    GPIOPinTypeGPIOOutput(GPIO_PORTE_BASE, GPIO_PIN_5);
    LcdCS(true);
    LcdDC(true);
    GPIOPinWrite(GPIO_PORTE_BASE, GPIO_PIN_5, GPIO_PIN_5);

    GPIOPinConfigure(GPIO_PD0_SSI3CLK);
    GPIOPinConfigure(GPIO_PD3_SSI3TX);
    GPIOPinTypeSSI(GPIO_PORTD_BASE, GPIO_PIN_0 | GPIO_PIN_3);
    SSIDisable(SSI3_BASE);
    SSIConfigSetExpClk(SSI3_BASE, g_systemClock, SSI_FRF_MOTO_MODE_0,
                      SSI_MODE_MASTER, LCD_SPI_HZ, 8U);
    SSIEnable(SSI3_BASE);
    while (SSIDataGetNonBlocking(SSI3_BASE, &dummy)) { }

    DelayMs(10U);
    GPIOPinWrite(GPIO_PORTE_BASE, GPIO_PIN_5, 0U);
    DelayMs(20U);
    GPIOPinWrite(GPIO_PORTE_BASE, GPIO_PIN_5, GPIO_PIN_5);
    DelayMs(120U);
    LcdCommand(0x01, 0, 0U);          /* SWRESET */
    DelayMs(150U);
    LcdCommand(0x11, 0, 0U);          /* SLPOUT */
    DelayMs(500U);
    LcdCommand(0xB1, frameNormal, sizeof(frameNormal));
    LcdCommand(0xB2, frameNormal, sizeof(frameNormal));
    LcdCommand(0xB3, framePartial, sizeof(framePartial));
    LcdCommand1(0xB4, 0x07);          /* Inversion control */
    LcdCommand(0xC0, power1, sizeof(power1));
    LcdCommand1(0xC1, 0xC5);
    LcdCommand(0xC2, power3, sizeof(power3));
    LcdCommand(0xC3, power4, sizeof(power4));
    LcdCommand(0xC4, power5, sizeof(power5));
    LcdCommand1(0xC5, 0x0E);          /* VCOM */
    LcdCommand1(0x36, LCD_MADCTL);     /* Memory access / color order */
    LcdCommand1(0x3A, 0x05);          /* RGB565, 16-bit pixels */
    LcdCommand(LCD_INVERT ? 0x21 : 0x20, 0, 0U);
    LcdCommand(0x13, 0, 0U);          /* Normal display mode */
    DelayMs(10U);
    /* DISPON is issued only after the first complete frame is written. */
}

static void LcdWriteSpan(int y, int x0, int x1, const uint8_t *frame)
{
    uint8_t range[4];
    uint16_t address, color;
    int x;
    uint32_t index;
    uint8_t packed, paletteIndex;

    address = (uint16_t)(x0 + LCD_X_OFFSET);
    range[0] = (uint8_t)(address >> 8); range[1] = (uint8_t)address;
    address = (uint16_t)(x1 + LCD_X_OFFSET);
    range[2] = (uint8_t)(address >> 8); range[3] = (uint8_t)address;
    LcdCommand(0x2A, range, 4U);      /* CASET */
    address = (uint16_t)(y + LCD_Y_OFFSET);
    range[0] = (uint8_t)(address >> 8); range[1] = (uint8_t)address;
    range[2] = range[0]; range[3] = range[1];
    LcdCommand(0x2B, range, 4U);      /* RASET */

    LcdCS(false);
    LcdDC(false);
    SpiWrite(0x2C);                   /* RAMWR */
    LcdDC(true);
    for (x = x0; x <= x1; ++x) {
        index = (uint32_t)(y * LCD_WIDTH + x);
        packed = frame[index >> 1];
        paletteIndex = (index & 1U) ? (packed & 15U) : (packed >> 4);
        color = g_palette[paletteIndex];
        SpiWrite((uint8_t)(color >> 8));
        SpiWrite((uint8_t)color);
    }
    LcdCS(true);
}

static void Timer0AHandler(void)
{
    TimerIntClear(TIMER0_BASE, TIMER_TIMA_TIMEOUT);
    ClockTick100Hz();
}

static void ClockTimerInit(void)
{
    PeripheralEnable(SYSCTL_PERIPH_TIMER0);
    TimerDisable(TIMER0_BASE, TIMER_A);
    TimerConfigure(TIMER0_BASE, TIMER_CFG_PERIODIC); /* 32-bit timer */
    TimerLoadSet(TIMER0_BASE, TIMER_A, g_systemClock / CLOCK_TICK_HZ - 1U);
    /* DriverLib installs the handler in a RAM vector table.
     * Do not manually edit the startup vector or add a duplicate ISR. */
    TimerIntRegister(TIMER0_BASE, TIMER_A, Timer0AHandler);
    TimerIntClear(TIMER0_BASE, TIMER_TIMA_TIMEOUT);
    TimerIntEnable(TIMER0_BASE, TIMER_TIMA_TIMEOUT);
    TimerEnable(TIMER0_BASE, TIMER_A);
    IntMasterEnable();
}
#endif

/* ---------------- Drawing in RAM, never erase directly on LCD ---------------- */
static void Pixel(int x, int y, uint8_t color)
{
    uint32_t index;
    uint8_t *p;
    if (x < 0 || x >= LCD_WIDTH || y < 0 || y >= LCD_HEIGHT) return;
    index = (uint32_t)(y * LCD_WIDTH + x);
    p = &g_back[index >> 1];
    if (index & 1U) *p = (uint8_t)((*p & 0xF0U) | color);
    else *p = (uint8_t)((*p & 0x0FU) | (color << 4));
}

static int AbsInt(int x) { return x < 0 ? -x : x; }

static void Dot(int x, int y, int radius, uint8_t color)
{
    int dx, dy;
    for (dy = -radius; dy <= radius; ++dy)
        for (dx = -radius; dx <= radius; ++dx)
            if (dx * dx + dy * dy <= radius * radius)
                Pixel(x + dx, y + dy, color);
}

static void Line(int x0, int y0, int x1, int y1, uint8_t color, int radius)
{
    int dx = AbsInt(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -AbsInt(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int error = dx + dy, twiceError;
    for (;;) {
        Dot(x0, y0, radius, color);
        if (x0 == x1 && y0 == y1) break;
        twiceError = 2 * error;
        if (twiceError >= dy) { error += dy; x0 += sx; }
        if (twiceError <= dx) { error += dx; y0 += sy; }
    }
}

static void Circle(int cx, int cy, int radius, uint8_t color)
{
    int x = radius, y = 0, error = 1 - radius;
    while (x >= y) {
        Pixel(cx+x,cy+y,color); Pixel(cx+y,cy+x,color);
        Pixel(cx-y,cy+x,color); Pixel(cx-x,cy+y,color);
        Pixel(cx-x,cy-y,color); Pixel(cx-y,cy-x,color);
        Pixel(cx+y,cy-x,color); Pixel(cx+x,cy-y,color);
        ++y;
        if (error < 0) error += 2*y + 1;
        else { --x; error += 2*(y-x) + 1; }
    }
}

/* Sine for 0..90 degrees, Q15. Interpolate at 0.1 degree resolution. */
static const int16_t g_sine[91] = {
    0,572,1144,1715,2286,2856,3425,3993,4560,5126,
    5690,6252,6813,7371,7927,8481,9032,9580,10126,10668,
    11207,11743,12275,12803,13328,13848,14364,14876,15383,15886,
    16383,16876,17364,17846,18323,18794,19260,19720,20173,20621,
    21062,21497,21925,22347,22762,23170,23571,23964,24351,24730,
    25101,25465,25821,26169,26509,26841,27165,27481,27788,28087,
    28377,28659,28932,29196,29451,29697,29934,30162,30381,30591,
    30791,30982,31163,31335,31498,31650,31794,31927,32051,32165,
    32269,32364,32448,32523,32587,32642,32687,32722,32747,32762,
    32767
};

static int32_t SinQ15(int angle)
{
    int sign = 1, degree, remainder;
    int32_t value;
    angle %= 3600;
    if (angle < 0) angle += 3600;
    if (angle >= 1800) { angle -= 1800; sign = -1; }
    if (angle > 900) angle = 1800 - angle;
    degree = angle / 10;
    remainder = angle % 10;
    value = g_sine[degree];
    if (degree < 90)
        value += ((int32_t)g_sine[degree+1] - value) * remainder / 10;
    return value * sign;
}

static int RadialX(int angle, int radius)
{ return CX + (int)(SinQ15(angle) * radius / 32767); }
static int RadialY(int angle, int radius)
{ return CY - (int)(SinQ15(angle + 900) * radius / 32767); }

/* Simple original 5x7 numeric glyphs; rows contain five bits. */
static const uint8_t g_digits[10][7] = {
    {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14},
    {14,17,1,2,4,8,31}, {30,1,1,14,1,1,30},
    {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
    {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8},
    {14,17,17,14,17,17,14}, {14,17,17,15,1,1,14}
};

static void Character(int x, int y, char ch, int scale, uint8_t color)
{
    int row, col, dx, dy;
    uint8_t bits;
    for (row = 0; row < 7; ++row) {
        if (ch >= '0' && ch <= '9') bits = g_digits[ch-'0'][row];
        else if (ch == ':') bits = (row == 2 || row == 4) ? 4U : 0U;
        else bits = 0U;
        for (col = 0; col < 5; ++col) {
            if ((bits & (1U << (4-col))) == 0U) continue;
            for (dy = 0; dy < scale; ++dy)
                for (dx = 0; dx < scale; ++dx)
                    Pixel(x+col*scale+dx, y+row*scale+dy, color);
        }
    }
}

static void Text(int x, int y, const char *text, int scale, uint8_t color)
{
    while (*text) {
        Character(x, y, *text++, scale, color);
        x += 6 * scale;
    }
}

static void DrawFace(void)
{
    int tick, angle, inner;
    Circle(CX, CY, RADIUS, GREY);
    for (tick = 0; tick < 60; ++tick) {
        angle = tick * 60;
        inner = (tick % 5 == 0) ? 51 : 56;
        Line(RadialX(angle, inner), RadialY(angle, inner),
             RadialX(angle, 58), RadialY(angle, 58),
             (tick % 5 == 0) ? WHITE : GREY, 0);
    }
    Text(59, 17, "12", 1, WHITE);
    Text(105, 61, "3", 1, WHITE);
    Text(62, 104, "6", 1, WHITE);
    Text(19, 61, "9", 1, WHITE);
    Line(16, 130, 111, 130, GREY, 0);
}

static void DrawHand(int angle, int length, int tail, int radius, uint8_t color)
{
    Line(RadialX(angle, -tail), RadialY(angle, -tail),
         RadialX(angle, length), RadialY(angle, length), color, radius);
}

static void RenderClock(const ClockSnapshot *t)
{
    int hourAngle, minuteAngle, secondAngle;
    char digits[9];
    /* This clears RAM only. The physical display is NEVER blanked per frame. */
    memset(g_back, 0, FRAME_BYTES);
    DrawFace();
    hourAngle = (t->hour % 12) * 300 + t->minute * 5 + t->second / 12;
    minuteAngle = t->minute * 60 + t->second;
    secondAngle = t->second * 60 + t->fraction * 60 / CLOCK_TICK_HZ;
    DrawHand(hourAngle, 28, 3, 2, CYAN);
    DrawHand(minuteAngle, 41, 4, 1, YELLOW);
    DrawHand(secondAngle, 48, 8, 0, RED);
    Dot(CX, CY, 3, WHITE);
    Dot(CX, CY, 1, RED);
    digits[0] = (char)('0' + t->hour / 10);
    digits[1] = (char)('0' + t->hour % 10); digits[2] = ':';
    digits[3] = (char)('0' + t->minute / 10);
    digits[4] = (char)('0' + t->minute % 10); digits[5] = ':';
    digits[6] = (char)('0' + t->second / 10);
    digits[7] = (char)('0' + t->second % 10); digits[8] = '\0';
    Text(17, 140, digits, 2, WHITE);
}

/* Find the changed span of one row. Packed-byte comparison includes both
 * neighboring pixels, so an occasional unchanged pixel may also be sent. */
static bool ChangedSpan(int y, bool force, int *x0, int *x1)
{
    int first = 0, last = LCD_WIDTH / 2 - 1;
    int row = y * (LCD_WIDTH / 2);
    if (!force) {
        while (first <= last && g_back[row+first] == g_front[row+first]) ++first;
        if (first > last) return false;
        while (last > first && g_back[row+last] == g_front[row+last]) --last;
    }
    *x0 = first * 2;
    *x1 = last * 2 + 1;
    return true;
}

static void SwapFrames(void)
{
    uint8_t *oldFront = g_front;
    g_front = g_back;
    g_back = oldFront;
}

#ifndef LAB3_HOST_TEST
static void Present(bool force)
{
    int y, x0, x1;
    for (y = 0; y < LCD_HEIGHT; ++y)
        if (ChangedSpan(y, force, &x0, &x1))
            LcdWriteSpan(y, x0, x1, g_back);
    SwapFrames();
}

int main(void)
{
    ClockSnapshot now;
    uint32_t lastFrame;
    IntMasterDisable();
    SysCtlClockSet(SYSCTL_SYSDIV_4 | SYSCTL_USE_PLL |
                   SYSCTL_OSC_MAIN | SYSCTL_XTAL_16MHZ);
    g_systemClock = SysCtlClockGet(); /* 50MHz with LaunchPad's 16MHz crystal */
    LcdInit();
    now = ClockRead();
    RenderClock(&now);
    Present(true);
    LcdCommand(0x29, 0, 0U);          /* DISPON, initial frame already complete */
    DelayMs(100U);
    lastFrame = 0U;
    ClockTimerInit();
    for (;;) {
        now = ClockRead();
        if ((uint32_t)(now.ticks - lastFrame) >= DISPLAY_PERIOD_TICKS) {
            lastFrame = now.ticks;
            RenderClock(&now);
            Present(false);
        }
        /* Display transfers leave IRQs enabled. Slow rendering may skip a
         * picture, but the timer still advances time independently. */
    }
}

#ifdef DEBUG
void __error__(char *filename, uint32_t line)
{
    (void)filename; (void)line;
    for (;;) { }
}
#endif
#endif
