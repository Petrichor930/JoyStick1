#pragma once

/**
 * 应用层总入口。
 * main.c 里只调这两个函数，具体业务交给 APP 下各模块：
 *   USER CODE BEGIN Includes -> #include "AppManager.hpp"
 *   USER CODE BEGIN 2        -> App_Init();
 *   USER CODE BEGIN 3        -> App_Loop();
 *
 * 这里用 extern "C" 是为了让 C 写的 main.c 能直接调用 C++ 实现。
 */
#ifdef __cplusplus
extern "C" {
#endif

void App_Init(void);
void App_Loop(void);

#ifdef __cplusplus
}
#endif
