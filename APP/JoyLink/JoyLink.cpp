#include "JoyLink.hpp"

#include "Joystick.hpp"

#include "main.h" // HAL、USART1 寄存器

/**
 * JoyLink 上报链路实现：USART1 + 中断发送，把 g_joyWatch 每 5ms 发一帧。
 *
 * 为什么是寄存器级而不是 HAL_UART：
 *   1. 晶振/HSE 故障期间不能用 CubeMX 重新生成代码，而 UART 的 HAL 驱动
 *      （stm32f4xx_hal_uart.c/h）没被拷进 cubemax/Drivers，补齐它、改
 *      hal_conf.h 都要动 cubemax 生成层，违反分层原则；
 *   2. 这里只需要 TX，寄存器级实现不到 100 行，全部收在 APP 层，
 *      以后 CubeMX 正式开 USART1 也不冲突（见文件末尾说明）。
 * GPIO 复用配置仍走 HAL_GPIO（已编译），波特率分频按当前 PCLK2 计算，
 * HSI/HSE 时钟下都正确。
 */
namespace {

// ---------- 可调参数 ----------
constexpr uint32_t kSendPeriodMs = 5;  // 发送周期 5ms -> 200Hz（18 字节 @921600 约 0.2ms 发完）
constexpr uint32_t kBaudRate = 921600; // 与 PinyCustom 侧 JOY1_UART(huart10) 同频（该口共用自定义控制上行链路）

// ---------- 模块状态 ----------
volatile uint8_t s_txBusy = 0; // 1=上一帧还在中断发送中
volatile uint8_t s_txPos = 0;  // 当前帧已写入 DR 的字节数
uint8_t s_txFrame[JOY_FRAME_LEN];
uint8_t s_seq = 0;
uint32_t s_lastSendTick = 0;

// CRC8：多项式 0x07、初值 0x00（和 PinyCustom 侧 JoyStick1::crc8 一致）
uint8_t crc8(const uint8_t *data, uint32_t len)
{
    uint8_t crc = 0;
    while (len--) {
        crc ^= *data++;
        for (uint8_t i = 0; i < 8; i++) {
            crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x07) : static_cast<uint8_t>(crc << 1);
        }
    }
    return crc;
}

// 小端写入 16bit（有符号值先按位转成无符号）
void putU16(uint8_t *p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>(v >> 8);
}

}

uint8_t JoyLink_Init(void)
{
    // ---- GPIO：PA9=USART1_TX PA10=USART1_RX，复用推挽 ----
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_USART1_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {};
    gpio.Pin = GPIO_PIN_9 | GPIO_PIN_10;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF7_USART1;
    HAL_GPIO_Init(GPIOA, &gpio);

    // ---- USART1：921600 8N1，只用 TX ----
    // BRR = round(PCLK2 / 波特率)（16 倍过采样，RM0390：BRR 即 f_CK/baud 的定点值）
    const uint32_t pclk = HAL_RCC_GetPCLK2Freq();
    USART1->CR1 = 0;
    USART1->CR2 = 0;
    USART1->CR3 = 0;
    USART1->BRR = (pclk + kBaudRate / 2) / kBaudRate;
    USART1->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;

    HAL_NVIC_SetPriority(USART1_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(USART1_IRQn);

    s_lastSendTick = HAL_GetTick();
    return 1;
}

void JoyLink_Update(void)
{
    const uint32_t now = HAL_GetTick();
    if (now - s_lastSendTick < kSendPeriodMs) {
        return;
    }
    s_lastSendTick = now;

    if (s_txBusy) {
        return; // 上一帧还没发完（不该发生：1.56ms << 5ms），丢掉本帧
    }

    // ---- 打包 g_joyWatch（字段布局见 JoyLink.hpp）----
    s_txFrame[0] = JOY_FRAME_HEAD0;
    s_txFrame[1] = JOY_FRAME_HEAD1;
    putU16(&s_txFrame[2], static_cast<uint16_t>(g_joyWatch.x1));
    putU16(&s_txFrame[4], static_cast<uint16_t>(g_joyWatch.y1));
    putU16(&s_txFrame[6], static_cast<uint16_t>(g_joyWatch.x2));
    putU16(&s_txFrame[8], static_cast<uint16_t>(g_joyWatch.y2));
    putU16(&s_txFrame[10], g_joyWatch.rp1);
    putU16(&s_txFrame[12], g_joyWatch.rp2);
    s_txFrame[14] = g_joyWatch.keys;
    s_txFrame[15] = g_joyWatch.initOk;
    s_txFrame[16] = s_seq++;
    s_txFrame[17] = crc8(s_txFrame, JOY_FRAME_LEN - 1);

    // 按字节送 DR，TXE 中断里发完一帧后自动收尾
    s_txPos = 0;
    s_txBusy = 1;
    USART1->CR1 |= USART_CR1_TXEIE;
}

// USART1 中断：CubeMX 生成的 it.c 里没定义这个弱符号，在这里接管。
// 只开了 TXEIE，别的事件不会进中断。
extern "C" void USART1_IRQHandler(void)
{
    if (USART1->CR1 & USART_CR1_TXEIE) {
        if (s_txPos < JOY_FRAME_LEN) {
            const uint8_t pos = s_txPos;
            USART1->DR = s_txFrame[pos];
            s_txPos = static_cast<uint8_t>(pos + 1);
        } else {
            USART1->CR1 &= ~USART_CR1_TXEIE;
            s_txBusy = 0;
        }
    }
}

// 备忘：JoyStick1.ioc 已登记 USART1（PA9/PA10，921600，NVIC 开中断）。晶振修好后用
// CubeMX 重新生成代码时：生成的 stm32f4xx_it.c 会带同名 USART1_IRQHandler（重复定义，
// 链接报错），到时把本文件的 IRQHandler 删掉、初始化换 MX_USART1_UART_Init（或保留寄存器版），
// 帧协议和打包逻辑不用动。重新生成前先看 README 5.1 的时钟警告。
