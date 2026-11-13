#pragma once

#include <stdint.h>

// ======================= ADC 六路原始量 =======================
// 下标顺序 = CubeMX 里 ADC1 的 Rank 顺序，别改
enum JoyAdcIndex : uint8_t
{
    JOY_ADC_X1 = 0,   // 摇杆1 X   PA0 / ADC1_IN0
    JOY_ADC_Y1,       // 摇杆1 Y   PA1 / ADC1_IN1
    JOY_ADC_X2,       // 摇杆2 X   PA2 / ADC1_IN2
    JOY_ADC_Y2,       // 摇杆2 Y   PA3 / ADC1_IN3
    JOY_ADC_RP1,      // 电位器1   PA4 / ADC1_IN4（没有中点）RP1
    JOY_ADC_RP2,      // 电位器2   PA5 / ADC1_IN5（没有中点）RP2
    JOY_ADC_COUNT
};

// ======================= 五个按键 =======================
// 全部上拉输入，按下接地 -> 读到 0
enum JoyKeyIndex : uint8_t
{
    JOY_KEY_SW4 = 0,  // 摇杆1 按压  PB10 SW4
    JOY_KEY_SW5,      // 摇杆2 按压  PB12 SW5
    JOY_KEY_SW1,      // 按钮1       PB13 SW1
    JOY_KEY_SW2,      // 按钮2       PB14 SW2
    JOY_KEY_SW3,      // 按钮3       PB15 SW3
    JOY_KEY_COUNT
};

// 摇杆轴映射后的量程：-JOY_AXIS_MAX ~ +JOY_AXIS_MAX
#define JOY_AXIS_MAX 1000

// ======================= 调试快照（给 Live Watch 看） =======================
// 整个模块的状态都放这一个结构体里，Live Watch 里只写 g_joyDebug，展开就是全部：
//   6 路原始值 / 本帧快照 / 中点 / 映射结果 / 按键
// 用 extern "C" 是为了符号名不带 C++ 修饰，输入框里不用加引号，也不会变名字。
struct JoyDebug
{
    volatile uint16_t raw[JOY_ADC_COUNT];      // DMA 正在写的原始值 0~4095
    uint16_t frame[JOY_ADC_COUNT];             // 本次计算用的那一帧快照
    int32_t  mid[2][2];                        // 上电标定的中点 [摇杆][轴]
    int16_t  axis[2][2];                       // 映射结果 -1000~+1000
    uint8_t  keyRaw[JOY_KEY_COUNT];            // 未消抖电平，1=按下
    uint8_t  keyStable[JOY_KEY_COUNT];         // 消抖后状态，1=按下
    uint8_t  keyCount[JOY_KEY_COUNT];          // 连续相同次数，看消抖进度用
    uint32_t sampleCount;                      // 采样次数，1kHz 下会一直涨 = 主循环活着
    uint32_t keyScanCount;                     // 按键扫描次数，10ms 一次
};

extern "C" JoyDebug g_joyDebug;

// ======================= 观测变量（Live Watch 快速看） =======================
// 平铺命名字段，Live Watch 添加 g_joyWatch 就能一眼看到当前值，不用展开数组查下标；
// 要查细节（6 路原始值 / 中点 / 消抖进度）再展开上面的 g_joyDebug。
// 同样用 extern "C"，符号名不带 C++ 修饰。
struct JoyWatch
{
    int16_t  x1, y1;     // 摇杆1 映射结果 -1000~+1000（显示带 ±10 死区、回中即归 0，真值看 g_joyDebug）
    int16_t  x2, y2;     // 摇杆2 映射结果 -1000~+1000（同上）
    uint16_t rp1, rp2;   // 电位器 0~4095（滑动平均 + 显示死区，真值看 g_joyDebug）
    uint8_t  keys;       // 按键位图，1=按下：bit0=SW4(摇杆1按压) bit1=SW5(摇杆2按压) bit2=SW1 bit3=SW2 bit4=SW3
    uint8_t  initOk;     // 1=初始化成功；0=ADC/DMA 启动失败
};

extern "C" JoyWatch g_joyWatch;

// ======================= 对外接口 =======================
uint8_t  Joystick_Init(void);                        // 启动 ADC1 DMA 循环采集，返回 1=成功
void     Joystick_Update(void);                      // 主循环里周期调用：取值 -> 标定 -> 映射
uint16_t Joystick_GetRaw(JoyAdcIndex index);         // 0~4095 原始值
int16_t  Joystick_GetAxis(uint8_t stick, uint8_t axis); // stick:1/2，axis:0=X 1=Y，返回 -1000~1000
uint8_t  Joystick_IsKeyPressed(JoyKeyIndex index);   // 1=按下（已消抖）
