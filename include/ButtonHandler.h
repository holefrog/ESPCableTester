/**
 * @file ButtonHandler.h
 * @brief 按键处理模块 —— 单按键多功能识别（单击 / 双击 / 长按）
 *
 * 设计要点
 * --------
 * 1. **双核隔离**：按键检测任务固定运行在 Core 1（APP CPU），与主循环（也在 Core 1）
 *    共享同一个核心，但按键轮询本身很轻量，不会占用显著 CPU 时间。
 *    之所以不能放在 Core 0（PRO CPU）：Core 0 运行 digitalRead，会与 Core 1 的
 *    寄存器直读（GPIO_IN_REG）在 APB 总线上产生竞争，导致周期计数偶发 +85 周期
 *    的异常（约 0.2m 的测量误差）。
 *
 * 2. **事件标志位**：检测到事件后写入 volatile bool 标志位，主循环通过 getEvent()
 *    轮询消费。标志位被消费后立即清零，保证每个事件只触发一次响应。
 *
 * 3. **双击超时窗口**：松开按键后启动 400ms 计时，在此窗口内再次按下即为双击；
 *    窗口过期后只剩一次点击则判为单击。
 *
 * 4. **长按优先**：按下超过 2000ms 且尚未松手时立即触发长按，松手后不再产生单击。
 *
 * 使用方式
 * --------
 * @code
 *   ButtonHandler::init();          // 在 setup() 中调用一次
 *   ButtonEvent e = ButtonHandler::getEvent();  // 在 loop() 中轮询
 *   if (e == BTN_EVENT_LONG_PRESS) { ... }
 * @endcode
 */

#ifndef BUTTON_HANDLER_H
#define BUTTON_HANDLER_H

#include <Arduino.h>

/**
 * @brief 按键事件枚举
 *
 * getEvent() 每次只返回一个事件，消费后自动清零。
 * 优先级：长按 > 单击 / 双击（长按触发后松手不产生单击）。
 */
enum ButtonEvent {
    BTN_EVENT_NONE,          ///< 无事件（本轮轮询未检测到动作）
    BTN_EVENT_SINGLE_CLICK,  ///< 单击（按下后在双击窗口内松开，且无第二次按下）
    BTN_EVENT_DOUBLE_CLICK,  ///< 双击（双击窗口内连续按下两次）
    BTN_EVENT_LONG_PRESS     ///< 长按（持续按下超过 BOOT_LONG_PRESS_MS）
};

/**
 * @brief 按键处理器（静态工具类，无需实例化）
 *
 * 所有状态保存在 .cpp 文件级的 static 变量中，整个项目只有一个按键实例。
 */
class ButtonHandler {
public:
    /**
     * @brief 初始化 BOOT 按键引脚并在 Core 1 启动后台检测任务
     *
     * 必须在 setup() 中调用，且只调用一次。
     * 内部会调用 xTaskCreatePinnedToCore，将任务固定在 Core 1 防止 APB 总线竞争。
     */
    static void init();

    /**
     * @brief 轮询并消费一个按键事件
     *
     * @return 当前待处理的事件，消费后标志位自动清零；无事件时返回 BTN_EVENT_NONE。
     *
     * @note 每次调用最多返回一个事件（单击 / 双击 / 长按按优先级顺序消费）。
     *       主循环中每帧调用一次即可。
     */
    static ButtonEvent getEvent();

    /**
     * @brief 重置空闲计时器
     *
     * 在用户有按键操作时调用，防止立即触发深度睡眠。
     * ButtonHandler 内部在检测到按下时会自动调用一次；
     * 外部也可以在屏幕刷新等非按键操作后手动调用。
     */
    static void resetActivityTimer();

    /**
     * @brief 判断是否已超过空闲超时时间
     *
     * @param timeoutMinutes 空闲超时阈值（分钟），传入 0 表示禁用超时检测
     * @return true 表示已超时，调用方应触发深度睡眠
     */
    static bool isIdleTimeout(uint32_t timeoutMinutes);

private:
    /**
     * @brief FreeRTOS 后台按键检测任务（固定在 Core 1）
     *
     * 每 BTN_POLL_INTERVAL_MS 毫秒轮询一次 BOOT_BTN_PIN 的电平，
     * 通过四态状态机识别单击、双击、长按并写入对应标志位。
     *
     * 四态：
     *   按下沿（LOW↓）→ 记录 pressStart
     *   松开沿（HIGH↑）→ 累加 clickCount，记录 releaseTime
     *   持续按下 → 检查是否超过长按阈值
     *   持续松开 → 检查双击窗口是否过期，过期则提交事件
     */
    static void task(void *pvParameters);
};

#endif // BUTTON_HANDLER_H

