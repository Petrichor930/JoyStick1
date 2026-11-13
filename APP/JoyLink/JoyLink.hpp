#pragma once

#include <stdint.h>

// ======================= JoyLink 上报链路 =======================
// 把 g_joyWatch 周期打包成 18 字节定长帧，从 USART1 发给 PinyCustom。
// 帧协议（和 PinyCustom 的 Src/Common/Extensions/RemoteControl/JoyStick1 保持一致，
// 改一处必须同步改另一处）：
//
//   [0]     0x55 帧头
//   [1]     0xAA 帧头
//   [2..3]  x1   int16 小端  摇杆1 X -1000~+1000
//   [4..5]  y1   int16 小端  摇杆1 Y
//   [6..7]  x2   int16 小端  摇杆2 X
//   [8..9]  y2   int16 小端  摇杆2 Y
//   [10..11] rp1 uint16 小端 电位器1 0~4095
//   [12..13] rp2 uint16 小端 电位器2
//   [14]    keys   uint8 按键位图，1=按下：bit0=SW4 bit1=SW5 bit2=SW1 bit3=SW2 bit4=SW3
//   [15]    initOk uint8 1=初始化成功
//   [16]    seq    uint8 滚动序号，对端用来看丢帧
//   [17]    crc8   uint8 对 [0..16] 计算，多项式 0x07、初值 0x00
//
// 物理接线：USART1 PA9(TX)/PA10(RX)，921600 8N1，与 PinyCustom 侧 JOY1_UART 交叉相连，
//          两板共地。发送周期 5ms（200Hz），中断发送不阻塞主循环。
// 对端处理：PinyCustom 的 RcMsgHandler<JOY1>（DT7 式 DMA 双缓冲接收）把本帧载荷
//          原样打包进 CustomCtrlPacket::TxData_s.clientData，经自定义控制链路
//          （CustomCtrlPublisher / huart10 / 921600）发给上位机 service。
//          注意：对端 JOY1_UART 与自定义控制上行共用 huart10，一个 USART 只有一个
//          波特率，所以本端必须用 921600 对齐。

#define JOY_FRAME_LEN   18
#define JOY_FRAME_HEAD0 0x55
#define JOY_FRAME_HEAD1 0xAA

// 启动 USART1（PA9/PA10）+ 发送中断，返回 1=成功
uint8_t JoyLink_Init(void);

// 主循环里周期调用：每 5ms 把 g_joyWatch 打包发一帧（上一帧没发完就跳过本帧）
void JoyLink_Update(void);
