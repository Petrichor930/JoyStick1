#include "Joystick.hpp"

#include "adc.h"   // hadc1
#include "main.h"  // 引脚宏 SW1~SW5、HAL

/**
 * 摇杆模块实现。
 *
 * 数据流：ADC1 连续转换（6 通道扫描）-> DMA2 Stream0 循环搬运 -> g_joyDebug.raw[6]
 *        -> 每 1ms 取一次快照 -> 中点/死区/线性映射 -> g_joyDebug.axis[2][2]
 *        -> 上层读数；按键每 5ms 扫一次，连续 3 次一致才认。
 *
 * 所有状态都在 g_joyDebug 里，快速观测看平铺的 g_joyWatch，Live Watch 各加一个名字即可。
 */
extern "C" {
JoyDebug g_joyDebug = {};
JoyWatch g_joyWatch = {};
}

namespace
{
    // 模块状态统一走这个引用，代码里不用到处写 g_joyDebug
    JoyDebug &dbg = g_joyDebug;

    // ---------- 可调参数 ----------
    constexpr uint32_t kSamplePeriodMs = 1;    // 摇杆采样周期，1ms -> 1kHz
    constexpr uint32_t kKeyPeriodMs    = 5;    // 按键扫描周期，5ms 一次让短按也抓得住
    constexpr uint8_t  kKeyStableCount = 3;    // 连续 3 次（15ms）一致才算稳定；按键特别抖可加到 4

    constexpr int32_t kDeadzoneCounts = 60;    // 死区，单位：ADC counts（按实测噪音调）
    constexpr int32_t kAdcFullScale   = 4095;  // 12bit 满量程
    constexpr uint16_t kCalibFrames   = 100;   // 上电自标定取的帧数

    // 硬件资源表：和 JoyKeyIndex 顺序一一对应，只能按顺序改
    GPIO_TypeDef *const kKeyPorts[JOY_KEY_COUNT] = {
        SW4_GPIO_Port,  // 摇杆1 按压
        SW5_GPIO_Port,  // 摇杆2 按压
        SW1_GPIO_Port,  // 按钮1
        SW2_GPIO_Port,  // 按钮2
        SW3_GPIO_Port,  // 按钮3
    };
    const uint16_t kKeyPins[JOY_KEY_COUNT] = {
        SW4_Pin, SW5_Pin, SW1_Pin, SW2_Pin, SW3_Pin,
    };

    uint32_t lastSampleTick = 0;
    uint32_t lastKeyTick    = 0;

    // ---------- 内部工具 ----------
    void takeFrame(void)
    {
        // DMA 一帧 6 通道约 23us（25MHz ADC 时钟、84 周期采样），1ms 采样一次基本不会撕裂；
        // 摇杆本身是慢变量，个别通道错位一帧不影响手感
        for (uint8_t i = 0; i < JOY_ADC_COUNT; i++)
        {
            dbg.frame[i] = dbg.raw[i];
        }
    }

    // 原始值 -> -JOY_AXIS_MAX ~ +JOY_AXIS_MAX
    // 全程整数运算：先做 int32 差值，再按实际半量程缩放，避免 float<->int 反复转换
    // 造成"正方向调不动、负方向调太快"的问题
    int16_t rawToAxis(uint16_t raw, int32_t mid)
    {
        const int32_t diff = static_cast<int32_t>(raw) - mid;

        if (diff >= -kDeadzoneCounts && diff <= kDeadzoneCounts)
        {
            return 0;  // 死区内当 0，挡住手放开时的抖动
        }

        // 中点不一定正好 2048，两侧量程分别算，才不会一边提前饱和
        const int32_t halfSpan = (diff > 0) ? (kAdcFullScale - mid) : mid;
        const int32_t liveSpan = halfSpan - kDeadzoneCounts;
        if (liveSpan <= 0)
        {
            return 0;
        }

        // 扣掉死区再线性缩放：刚出死区时从 0 起，不会有跳变
        const int32_t live = (diff > 0) ? (diff - kDeadzoneCounts) : (diff + kDeadzoneCounts);
        int32_t value = live * JOY_AXIS_MAX / liveSpan;

        if (value > JOY_AXIS_MAX)  { value = JOY_AXIS_MAX; }
        if (value < -JOY_AXIS_MAX) { value = -JOY_AXIS_MAX; }
        return static_cast<int16_t>(value);
    }

    // ---------- 自适应中点跟踪 ----------
    // 上电标定只做一次，电位器温漂/老化会让静止值慢慢离开中点，超出 ±60 死区后
    // 就出现"回中不归 0"。这里在摇杆持续处于死区内（输出恒为 0，绝不影响操作）
    // 一段时间后，让中点以每 kCenterStepMs 步进 1 count 的速度向静止值靠拢。
    constexpr uint16_t kCenterHoldMs = 2000;  // 进死区持续多久才开始跟踪（防瞬时过中点被误吸收）
    constexpr uint16_t kCenterStepMs = 100;   // 跟踪步进周期，100ms 走 1 count = 10 counts/s
    uint16_t centerHold[2][2] = {{0}};        // 各轴连续处于死区的毫秒数

    void trackCenter(uint8_t stick, uint8_t axis, uint16_t raw)
    {
        int32_t &mid  = dbg.mid[stick][axis];
        uint16_t &hold = centerHold[stick][axis];
        const int32_t diff = static_cast<int32_t>(raw) - mid;

        if (diff >= -kDeadzoneCounts && diff <= kDeadzoneCounts)
        {
            if (hold < 60000)
            {
                hold++;
            }
            if (hold >= kCenterHoldMs && ((hold - kCenterHoldMs) % kCenterStepMs) == 0)
            {
                if (diff > 0)       { mid++; }
                else if (diff < 0)  { mid--; }
            }
        }
        else
        {
            hold = 0;  // 一动就停，跟踪只发生在死区内
        }
    }

    uint8_t readKeyRaw(uint8_t index)
    {
        // 上拉输入，按下接地 -> 读到 RESET 就是按下
        return (HAL_GPIO_ReadPin(kKeyPorts[index], kKeyPins[index]) == GPIO_PIN_RESET) ? 1 : 0;
    }

    void scanKeys(void)
    {
        dbg.keyScanCount++;
        for (uint8_t i = 0; i < JOY_KEY_COUNT; i++)
        {
            const uint8_t now = readKeyRaw(i);
            if (now == dbg.keyRaw[i])
            {
                if (dbg.keyCount[i] < kKeyStableCount)
                {
                    dbg.keyCount[i]++;
                }
            }
            else
            {
                dbg.keyRaw[i] = now;
                dbg.keyCount[i] = 1;  // 电平变化这一下就是新电平的第 1 次采样
            }

            // 连续 kKeyStableCount 次一致才更新稳定状态：5ms 扫一次，3 次 = 15ms
            if (dbg.keyCount[i] >= kKeyStableCount)
            {
                dbg.keyStable[i] = dbg.keyRaw[i];
            }
        }
    }

    // rp 显示用滑动平均：电位器是高阻未缓冲源，±几 counts 模拟噪声是常态，
    // 平均只为了 Live Watch 显示不跳；g_joyDebug.frame/GetRaw 仍是原始值
    constexpr uint8_t kRpFilterLen     = 32;  // 平均窗口（2 的幂）
    constexpr uint8_t kRpFilterLenMask = static_cast<uint8_t>(kRpFilterLen - 1);
    constexpr int32_t kRpDispBand      = 3;   // rp 显示死区：±3 counts 内显示锁死
    constexpr int32_t kAxisDispBand    = 10;  // 轴显示死区：±1000 的 1%，挡噪声不挡操作
    uint16_t rpFilterBuf[2][kRpFilterLen] = {{0}};
    uint8_t  rpFilterIdx                  = 0;

    // 显示死区更新：静止噪声被死区挡住、数字不动；真实变化超过死区立即跟随
    template <typename T>
    void deadbandUpdate(T &shown, int32_t target, int32_t band)
    {
        const int32_t cur = shown;
        if (target - cur > band || cur - target > band)
        {
            shown = static_cast<T>(target);
        }
    }

    // 摇杆轴专用：真值回 0（rawToAxis 死区内）时显示直接归 0。
    // 普通 deadbandUpdate 有保持特性——回中后真值=0 而显示值停在 ±band 内时
    // 偏差不够大、永远不再更新，留下"回中不归 0"的残值
    template <typename T>
    void deadbandUpdateCentered(T &shown, int32_t target, int32_t band)
    {
        if (target == 0)
        {
            shown = 0;
        }
        else
        {
            deadbandUpdate(shown, target, band);
        }
    }

    // 把当前结果同步到 g_joyWatch：Live Watch 看它一眼就够，细节去 g_joyDebug
    void syncWatch(void)
    {
        deadbandUpdateCentered(g_joyWatch.x1, dbg.axis[0][0], kAxisDispBand);
        deadbandUpdateCentered(g_joyWatch.y1, dbg.axis[0][1], kAxisDispBand);
        deadbandUpdateCentered(g_joyWatch.x2, dbg.axis[1][0], kAxisDispBand);
        deadbandUpdateCentered(g_joyWatch.y2, dbg.axis[1][1], kAxisDispBand);

        rpFilterBuf[0][rpFilterIdx] = dbg.frame[JOY_ADC_RP1];
        rpFilterBuf[1][rpFilterIdx] = dbg.frame[JOY_ADC_RP2];
        rpFilterIdx                 = static_cast<uint8_t>((rpFilterIdx + 1) & kRpFilterLenMask);
        uint32_t sum1 = 0;
        uint32_t sum2 = 0;
        for (uint8_t k = 0; k < kRpFilterLen; k++)
        {
            sum1 += rpFilterBuf[0][k];
            sum2 += rpFilterBuf[1][k];
        }
        deadbandUpdate(g_joyWatch.rp1, static_cast<int32_t>(sum1 / kRpFilterLen), kRpDispBand);
        deadbandUpdate(g_joyWatch.rp2, static_cast<int32_t>(sum2 / kRpFilterLen), kRpDispBand);

        uint8_t keys = 0;
        for (uint8_t i = 0; i < JOY_KEY_COUNT; i++)
        {
            keys |= static_cast<uint8_t>(dbg.keyStable[i] << i);
        }
        g_joyWatch.keys = keys;
    }
}

uint8_t Joystick_Init(void)
{
    // 1. 打开 ADC 连续转换
    //    CubeMX 里 ContinuousConvMode 是 DISABLE：那样一圈 6 通道扫完 ADC 就自己停，
    //    循环 DMA 只能空等，采样只会有上电那一帧。连续采样必须开 CONT。
    //    改在 APP 层而不是 cubemax 层，是为了 .ioc 重新生成也不会被覆盖。
    hadc1.Init.ContinuousConvMode = ENABLE;
    if (HAL_ADC_Init(&hadc1) != HAL_OK)
    {
        return 0;
    }

    // 2. 启动循环 DMA：6 个半字，DMA 自己回卷，之后不用再管
    //    HAL 的接口只收 uint32_t*：raw 是 volatile，先去掉 volatile 再按 HAL 的惯例强转
    //    （实际宽度仍是 Half Word，由 hadc1 的 DMA 配置决定）
    if (HAL_ADC_Start_DMA(&hadc1,
                          reinterpret_cast<uint32_t *>(const_cast<uint16_t *>(dbg.raw)),
                          JOY_ADC_COUNT) != HAL_OK)
    {
        return 0;
    }

    // 3. 上电自标定两个摇杆的中点（这段时间摇杆要停在中位，别推着）
    HAL_Delay(2);  // 等第一帧落地
    int32_t sum[JOY_ADC_COUNT] = {0};
    for (uint16_t frame = 0; frame < kCalibFrames; frame++)
    {
        takeFrame();
        for (uint8_t ch = 0; ch < JOY_ADC_COUNT; ch++)
        {
            sum[ch] += static_cast<int32_t>(dbg.frame[ch]);
        }
        HAL_Delay(1);
    }

    dbg.mid[0][0] = sum[JOY_ADC_X1] / kCalibFrames;
    dbg.mid[0][1] = sum[JOY_ADC_Y1] / kCalibFrames;
    dbg.mid[1][0] = sum[JOY_ADC_X2] / kCalibFrames;
    dbg.mid[1][1] = sum[JOY_ADC_Y2] / kCalibFrames;

    lastSampleTick = HAL_GetTick();
    lastKeyTick    = lastSampleTick;

    syncWatch();
    g_joyWatch.initOk = 1;
    return 1;
}

void Joystick_Update(void)
{
    const uint32_t now = HAL_GetTick();

    if (now - lastSampleTick >= kSamplePeriodMs)
    {
        lastSampleTick = now;
        takeFrame();
        dbg.sampleCount++;

        // 摇杆静置在死区内时缓慢跟踪中点，吸收温漂/标定偏差（不影响正常操作）
        trackCenter(0, 0, dbg.frame[JOY_ADC_X1]);
        trackCenter(0, 1, dbg.frame[JOY_ADC_Y1]);
        trackCenter(1, 0, dbg.frame[JOY_ADC_X2]);
        trackCenter(1, 1, dbg.frame[JOY_ADC_Y2]);

        dbg.axis[0][0] = rawToAxis(dbg.frame[JOY_ADC_X1], dbg.mid[0][0]);
        dbg.axis[0][1] = rawToAxis(dbg.frame[JOY_ADC_Y1], dbg.mid[0][1]);
        dbg.axis[1][0] = rawToAxis(dbg.frame[JOY_ADC_X2], dbg.mid[1][0]);
        dbg.axis[1][1] = rawToAxis(dbg.frame[JOY_ADC_Y2], dbg.mid[1][1]);

        syncWatch();
    }

    if (now - lastKeyTick >= kKeyPeriodMs)
    {
        lastKeyTick = now;
        scanKeys();
    }
}

uint16_t Joystick_GetRaw(JoyAdcIndex index)
{
    if (index >= JOY_ADC_COUNT)
    {
        return 0;
    }
    return dbg.frame[index];
}

int16_t Joystick_GetAxis(uint8_t stick, uint8_t axis)
{
    if (stick < 1 || stick > 2 || axis > 1)
    {
        return 0;
    }
    return dbg.axis[stick - 1][axis];
}

uint8_t Joystick_IsKeyPressed(JoyKeyIndex index)
{
    if (index >= JOY_KEY_COUNT)
    {
        return 0;
    }
    return dbg.keyStable[index];
}
