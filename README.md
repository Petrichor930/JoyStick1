# JoyStick1

STM32F411 双摇杆输入模块固件：2 个带按压摇杆、2 个电位器、3 个按键。ADC+DMA 采集，轴值映射 ±1000，按键 15ms 消抖，Live Watch 观测（`g_joyWatch` / `g_joyDebug`），JoyLink 串口 200Hz 上报给 PinyCustom。

> **⚠️ 固件暂跑 HSI 时钟**（板上晶振/HSE 硬件故障未修，见 [排错](#排错)）。**晶振修好前不要用 CubeMX 重新生成代码**——会把时钟树还原回 HSE，固件立刻复发"运行即失联"。

---

## 快速开始

**环境**：`arm-none-eabi-gcc`、CMake ≥ 3.20、Ninja（均在 PATH）。调试用 VS Code + cortex-debug + OpenOCD。

**1. 构建**

```bash
cmake -B build -G Ninja
ninja -C build
```

产物：`build/cubemax/JoyStick1.elf` / `.hex` / `.bin`（VS Code 任务：`configure` / `build` / `clean`）。

**2. 烧录**：VS Code 任务 `flash: stlink`（ST-Link）或 `flash: daplink`（CMSIS-DAP）。

**3. 验证**：F5 进调试，Live Watch 加 `g_joyWatch`——拨摇杆、拧电位器、按按键，数值跟手即成功；`g_joyDebug.sampleCount` 持续上涨 = 主循环活着。

**4. 接 PinyCustom（JoyLink 上报，可选）**：PA9(TX) → PinyCustom PE2(UART10 RX)，两板共地，921600 8N1。详见 [JoyLink](#joylink-串口上报pinycustom-链路)。

> 改了代码没生效？本工程 `APP_LIB` 用 OBJECT 库就是为了防杀软干扰 `ar` 归档导致的"旧逻辑静默残留"；正常 `ninja -C build` 即可，无需手工清理。

## 硬件与引脚

| 外设 | 引脚 | 说明 |
|------|------|------|
| 摇杆1 X / Y | PA0 / PA1 | ADC1_IN0/1，带中点，映射 -1000~+1000 |
| 摇杆2 X / Y | PA2 / PA3 | ADC1_IN2/3，同上 |
| 电位器 RP1 / RP2 | PA4 / PA5 | ADC1_IN4/5，无中点，读数 0~4095 |
| SW4 / SW5 | PB10 / PB12 | 摇杆1 / 摇杆2 按压键 |
| SW1 / SW2 / SW3 | PB13 / PB14 / PB15 | 独立按键（上拉输入，按下接地） |
| USART1 TX / RX | PA9 / PA10 | JoyLink 上报，921600 8N1 |

MCU：STM32F411CEU6（Cortex-M4F，100MHz）。调试/烧录：SWD。

## 固件说明

### 工程架构

```
JoyStick1/
├── APP/                  业务层（唯一手写逻辑）
│   ├── AppManager.*      入口：App_Init / App_Loop，供 main.c 调用
│   ├── Joystick/         摇杆模块：采集/标定/映射/消抖/观测
│   └── JoyLink/          串口上报：USART1 寄存器级 TX，200Hz 发 g_joyWatch
├── cubemax/              CubeMX 生成层（不要手改，见顶部警告）
├── tools/                构建辅助 + PC 工具（joy_watch_uart.py）
├── openocd/  .vscode/    探针配置 / 构建调试任务
└── CMakeLists.txt        总控
```

**分层**：`main.c` 只调 `App_Init()` / `App_Loop()`，业务全在 `APP/`。新增模块 = 建 `APP/<模块名>/`，在 `APP/CMakeLists.txt` 的 GLOB 加一行，`AppManager` 挂一行。

**数据流**：

```
ADC1 六通道连续扫描(PA0~PA5) → DMA2 Stream0 → g_joyDebug.raw[6]
  → 1ms 取帧 → 上电 100 帧标定中点（摇杆须居中）→ 死区内静置 2s 自适应微调中点
  → 死区(±60 counts)+两侧线性映射 → axis[2][2]（±1000）
  → 5ms 按键扫描，3 次一致消抖 → syncWatch() 汇总进 g_joyWatch（回中即归 0）
  → 5ms JoyLink 打包 18 字节帧 → USART1 中断发送
```

### 对外接口

| 接口 | 说明 |
|------|------|
| `Joystick_Init()` | 启动 ADC+DMA 采集并标定中点，返回 1=成功 |
| `Joystick_Update()` | 主循环周期调用：取帧→映射→消抖→同步观测 |
| `Joystick_GetRaw(index)` | 通道原始值 0~4095 |
| `Joystick_GetAxis(stick, axis)` | stick:1/2，axis:0=X 1=Y，返回 ±1000（真值） |
| `Joystick_IsKeyPressed(index)` | 1=按下（已消抖） |
| `JoyLink_Init()` | 启动 USART1（921600）+ 发送中断，返回 1=成功 |
| `JoyLink_Update()` | 主循环周期调用：每 5ms 打包 `g_joyWatch` 发一帧 |

### 观测

| 变量 | 内容 |
|------|------|
| `g_joyWatch` | **仪表盘**：x1/y1/x2/y2（±1000，显示死区±10、回中归0）、rp1/rp2（0~4095）、keys 位图（bit0=SW4…bit4=SW3）、initOk |
| `g_joyDebug` | **细节**：raw/frame/mid/axis、按键消抖进度、sampleCount（判活） |

> `g_joyWatch` 经显示平滑/死区处理，只保证显示稳定；真值用 `GetRaw` / `GetAxis` / `g_joyDebug`。

### JoyLink 串口上报（PinyCustom 链路）

每 5ms（200Hz）把 `g_joyWatch` 打包成 18 字节帧发出，PinyCustom 侧按 DT7 同款方式接收（`RemoteControl/JoyStick1`），在 `RcMsgHandler<JOY1>` 打进 `CustomCtrlPacket::TxData_s.clientData`，经自定义控制链路上行给 service。

| 项 | 内容 |
|----|------|
| 接线 | PA9(TX) → PinyCustom PE2(UART10 RX)，PA10(RX) ← PE3（可选），共地 |
| 参数 | **921600** 8N1（对端 huart10 与自定义控制上行共用，必须同频） |
| 帧格式 | `55 AA` + x1/y1/x2/y2(int16 LE) + rp1/rp2(u16 LE) + keys + initOk + seq + CRC8(poly 0x07)，详见 `JoyLink.hpp` |
| PC 工具 | `python tools/joy_watch_uart.py COMx`；`--hex` 看原始帧，`--tcp 8000` 给 VOFA 画曲线 |
| CubeMX | `.ioc` 已登记 USART1；重新生成前先看 [排错](#排错) 的 5.1 警告，生成后删 JoyLink 的 `USART1_IRQHandler`、换 `MX_USART1_UART_Init`（见 JoyLink.cpp 备忘） |

### 关键参数（`Joystick.cpp` 常量区）

| 参数 | 值 | 含义 |
|------|----|------|
| `kSamplePeriodMs` | 1 | 采样/映射周期 |
| `kKeyPeriodMs` / `kKeyStableCount` | 5 / 3 | 按键消抖（15ms） |
| `kDeadzoneCounts` | 60 | 摇杆中位死区（ADC counts） |
| `kCenterHoldMs` / `kCenterStepMs` | 2000 / 100 | 静置 2s 后中点 10 counts/s 自适应 |
| `kCalibFrames` | 100 | 上电标定帧数（期间摇杆居中） |
| `JOY_AXIS_MAX` | 1000 | 轴值满量程 |

## 排错

### 5.1 固件"一运行就失联" → 晶振/HSE 硬件故障（最重要，未完）

烧录成功但程序一运行调试口立刻失联；停在 ROM 时正常。HSI 版完全稳定、HSE 版毫秒内失联 → **根因在晶振/HSE 硬件**（标称频率、不起振、负载电容或虚焊）。现状：固件跑 HSI（`main.c` 的 `SystemClock_Config` 里 `[诊断临时]` 块）。待办：换晶振或按实际频率改 PLLM 后切回 HSE。**⚠️ 修好前不要用 CubeMX 重新生成代码。**

### 其他常见问题

| 症状 | 原因 | 处置 |
|------|------|------|
| 复位后 PC 停在 `0x1FFF_xxxx` | 进了 ST ROM Bootloader（BOOT0 悬空/拉高） | BOOT0 经 10k 下拉；按 Bootloader 键复位进 ISP 属正常 |
| 烧录报 `DEV_CONNECT_ERR` / `LIBUSB_ERROR_IO` | openocd 残留进程占探针 | 任务「清理残留 openocd」，或管理员 `taskkill /IM openocd.exe /F` |
| 摇杆静止时读数跳动 ±8 counts | 电位器高阻未缓冲源的正常模拟噪声 | 已由显示平滑+死区压制；真值看 `g_joyDebug`；根治可在 W 脚并 10nF~100nF |
| 按键"不灵敏"（Live Watch 看） | Live Watch 默认 1Hz 采样，快按落在间隙里 | `launch.json` 的 `samplesPerSecond` 提到 10 |
| 摇杆回中不归 0 | 历史 bug：显示死区卡残值 + 中点漂移（已修复） | 更新固件；上电标定时摇杆须居中，标定质量看 `g_joyDebug.mid` 与静止 `raw` 差（<60） |
| PinyCustom 收不到 JoyLink 帧 | 曾在 PA9/PA10 上遇到过（软件全验证正确，物理落点问题） | 现用 UART10(PE2) 已通；新接线先用 `joy_watch_uart.py` 验证发送端，再查交叉/共地 |

## 已知事项 / 待办

- [ ] **修晶振硬件**（5.1），然后切回 HSE 时钟配置
- [ ] 上位机中继（PinyCustom `Relay/custom_ctrl_unix_relay.py`）支持 69 字节 `TxData_s`、转发 `clientData` 给 service
- [x] 按键消抖、观测体系、显示平滑、JoyLink 链路、摇杆回中归零（2026-11-14 完成）
