#ifndef BUTTON_HANDLER_H
#define BUTTON_HANDLER_H

#include <Arduino.h>

enum ButtonEvent {
    BTN_EVENT_NONE,
    BTN_EVENT_SINGLE_CLICK,
    BTN_EVENT_DOUBLE_CLICK,
    BTN_EVENT_LONG_PRESS
};

class ButtonHandler {
public:
    // 初始化按键针脚并启动后台消抖任务
    static void init();

    // 轮询获取当前按键事件（如果有的话），获取后会自动清除标志
    static ButtonEvent getEvent();
    
    // 更新最后活动时间
    static void resetActivityTimer();
    
    // 检查是否空闲超时
    static bool isIdleTimeout(uint32_t timeoutMinutes);

private:
    static void task(void *pvParameters);
};

#endif // BUTTON_HANDLER_H
