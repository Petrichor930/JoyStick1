#include "AppManager.hpp"

#include "Joystick.hpp"
#include "JoyLink.hpp"

/**
 * 应用层实现：负责把各个业务模块的初始化/周期任务串起来。
 * 摇杆模块 + JoyLink 串口上报，后面加模块（比如 OLED）时在这里挂一行即可。
 */
void App_Init(void)
{
    Joystick_Init();
    JoyLink_Init();
}

void App_Loop(void)
{
    Joystick_Update();
    JoyLink_Update();
}
