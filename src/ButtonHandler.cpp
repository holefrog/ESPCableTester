#include "ButtonHandler.h"

static const int BOOT_BTN_PIN = 0;
static const uint32_t BOOT_LONG_PRESS_MS = 2000;
static const uint32_t BTN_DOUBLE_CLICK_TIMEOUT_MS = 400;
static const uint32_t BTN_POLL_INTERVAL_MS = 20;

static volatile bool flagSingleClick = false;
static volatile bool flagDoubleClick = false;
static volatile bool flagLongPress = false;
static volatile uint32_t lastActivityTime = 0;

void ButtonHandler::init() {
    pinMode(BOOT_BTN_PIN, INPUT_PULLUP);
    
    // 注意：必须放在 Core 1！如果放在 Core 0，会导致 Core 0 的 digitalRead 和
    // Core 1 的寄存器直读 在极小概率下发生 APB 总线竞争（Bus Contention），导致
    // Core 1 测得的 CPU 周期数突然多出 85 个周期（约 0.2 米的误差波动）。
    xTaskCreatePinnedToCore(task, "BtnTask", 2048, NULL, 1, NULL, 1);
    
    lastActivityTime = millis();
}

ButtonEvent ButtonHandler::getEvent() {
    if (flagSingleClick) {
        flagSingleClick = false;
        return BTN_EVENT_SINGLE_CLICK;
    }
    if (flagDoubleClick) {
        flagDoubleClick = false;
        return BTN_EVENT_DOUBLE_CLICK;
    }
    if (flagLongPress) {
        flagLongPress = false;
        return BTN_EVENT_LONG_PRESS;
    }
    return BTN_EVENT_NONE;
}

void ButtonHandler::resetActivityTimer() {
    lastActivityTime = millis();
}

bool ButtonHandler::isIdleTimeout(uint32_t timeoutMinutes) {
    if (timeoutMinutes == 0) return false;
    return (millis() - lastActivityTime > timeoutMinutes * 60000);
}

void ButtonHandler::task(void *pvParameters) {
    static uint32_t pressStart = 0;
    static uint32_t releaseTime = 0;
    static bool isPressed = false;
    static int clickCount = 0;
    static bool longPressFired = false;

    while (1) {
        bool btnIsPressed = (digitalRead(BOOT_BTN_PIN) == LOW);

        if (btnIsPressed && !isPressed) {
            pressStart = millis();
            isPressed = true;
            longPressFired = false;
            lastActivityTime = millis();
        } else if (!btnIsPressed && isPressed) {
            isPressed = false;
            if (!longPressFired) {
                clickCount++;
                releaseTime = millis();
            }
        } else if (btnIsPressed && isPressed) {
            if (!longPressFired && (millis() - pressStart >= BOOT_LONG_PRESS_MS)) {
                flagLongPress = true;
                longPressFired = true;
                clickCount = 0;
            }
        } else if (!btnIsPressed && !isPressed) {
            if (clickCount > 0 &&
                (millis() - releaseTime > BTN_DOUBLE_CLICK_TIMEOUT_MS)) {
                if (clickCount == 1) {
                    flagSingleClick = true;
                    printf("Button: Single Click\n");
                } else if (clickCount >= 2) {
                    flagDoubleClick = true;
                    printf("Button: Double Click\n");
                }
                clickCount = 0;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BTN_POLL_INTERVAL_MS)); // 轮询一次按键
    }
}
