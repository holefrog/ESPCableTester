#include "CableTester.h"
#include "Display.h"
#include "ButtonHandler.h"
#include "AppConfig.h"
#include <Arduino.h>

CableTester tester;
Display display;

CableStatus lastStatus;
bool isFirstRun = true;

const unsigned long SLEEP_TIMEOUT_MINUTES = 5;
const uint32_t UNCALIBRATED_WARNING_TIMEOUT_MS = 8000;
uint32_t warningStartTime = 0;
uint32_t tempBaseCycles[4] = {0, 0, 0, 0};

// 防抖机制：连续测得多少次相同的结果才视为状态稳定并保存历史记录
const int STABLE_READING_COUNT = 3;

// 主状态机主循环 (loop) 的间隔延时
const uint32_t MAIN_LOOP_DELAY_MS = 20;

enum AppState {
  STATE_UNCALIBRATED_WARNING,
  STATE_NORMAL,
  STATE_HISTORY_VIEW,
  STATE_CALIB_WAIT_EMPTY,
  STATE_CALIB_WAIT_76INCH,
  STATE_SETTINGS
};

AppState appState = STATE_NORMAL;
int menuIndex = 0;

void handleNormalState();

void setup() {
  Serial.begin(115200);
  delay(1000);

  ButtonHandler::init();
  tester.init();
  display.init();

  appConfig.loadAll(tester);

  if (!appConfig.isCalibrated) {
    appState = STATE_UNCALIBRATED_WARNING;
    warningStartTime = millis();
    display.renderUncalibratedWarning(UNCALIBRATED_WARNING_TIMEOUT_MS / 1000);
  } else {
    appState = STATE_NORMAL;
    display.renderReady();
    delay(1000);
  }
}

void handleNormalState() {
  CableStatus currentStatus = tester.runTest();

  // 1. 如果连续多次检测到状态没有发生实质性改变，说明电缆接触稳定了
  static CableStatus debouncedStatus;
  static int stableCount = 0;

  if (CableTester::isStatusEqual(currentStatus, debouncedStatus)) {
    stableCount++;
  } else {
    stableCount = 0;
    debouncedStatus = currentStatus;
  }

  // 2. 状态稳定并且不是“没插线”的空载状态，我们才需要刷新 UI 和保存历史
  if (stableCount == STABLE_READING_COUNT) {
    if (!CableTester::isNoCable(currentStatus)) {
      display.renderResult(currentStatus, appConfig.useFeet, appConfig.isCalibrated);

      // 只将有意义的测量结果（测到了具体长度，或者有短路故障）保存到历史记录
      if (CableTester::hasActualLength(currentStatus) || currentStatus.hasFault) {
        if (!CableTester::isStatusEqual(currentStatus, lastStatus)) {
          lastStatus = currentStatus;
          appConfig.addHistory(currentStatus);
        }
      }
    } else {
      display.renderReady();
    }
  }

  // 如果这是开机后第一次运行，我们立即显示当前状态，不等待防抖
  if (isFirstRun) {
    if (!CableTester::isNoCable(currentStatus)) {
      display.renderResult(currentStatus, appConfig.useFeet, appConfig.isCalibrated);
    } else {
      display.renderReady();
    }
    isFirstRun = false;
  }
}

void loop() {
  ButtonEvent btnEvt = ButtonHandler::getEvent();
  bool singleClick = (btnEvt == BTN_EVENT_SINGLE_CLICK);
  bool doubleClick = (btnEvt == BTN_EVENT_DOUBLE_CLICK);
  bool longPress   = (btnEvt == BTN_EVENT_LONG_PRESS);

  // 检查是否空闲过长导致进入睡眠
  if (appState != STATE_UNCALIBRATED_WARNING) {
    if (ButtonHandler::isIdleTimeout(SLEEP_TIMEOUT_MINUTES)) {
      display.renderMessage("Sleeping...");
      delay(2000);
      display.sleep();
      esp_deep_sleep_start();
    }
  }

  if (appState == STATE_UNCALIBRATED_WARNING) {
    uint32_t elapsed = millis() - warningStartTime;
    uint32_t remain = (UNCALIBRATED_WARNING_TIMEOUT_MS > elapsed)
                          ? (UNCALIBRATED_WARNING_TIMEOUT_MS - elapsed) / 1000
                          : 0;

    display.renderUncalibratedWarning(remain);

    if (elapsed > UNCALIBRATED_WARNING_TIMEOUT_MS || btnEvt != BTN_EVENT_NONE) {
      if (btnEvt != BTN_EVENT_NONE) ButtonHandler::resetActivityTimer();
      appState = STATE_NORMAL;
      display.renderReady();
      delay(500); // 避免按键穿透
      isFirstRun = true;
    }
  } else if (appState == STATE_NORMAL) {
    if (longPress) {
      appState = STATE_SETTINGS;
      menuIndex = 0;
      isFirstRun = true;
    }
  } else if (appState == STATE_SETTINGS) {
    if (singleClick) {
      menuIndex = (menuIndex + 1) % 6;
      display.renderSettings(menuIndex, appConfig.useFeet, appConfig.soundOn);
    } else if (doubleClick) {
      if (menuIndex == 0) {
        if (appConfig.historyCount > 0) {
          appState = STATE_HISTORY_VIEW;
          appConfig.historyIndex = 0;
          display.renderHistory(appConfig.historyLogs[appConfig.historyIndex], appConfig.useFeet, appConfig.historyIndex, appConfig.historyCount);
        } else {
          display.renderMessage("No History!");
          delay(1000);
          display.renderSettings(menuIndex, appConfig.useFeet, appConfig.soundOn);
        }
      } else if (menuIndex == 1) {
        appConfig.clearHistory();
        printf("History Cleared!\n");
        display.renderMessage("History Cleared!");
        delay(1000);
        display.renderSettings(menuIndex, appConfig.useFeet, appConfig.soundOn);
      } else if (menuIndex == 2) {
        appConfig.toggleUnit();
        display.renderSettings(menuIndex, appConfig.useFeet, appConfig.soundOn);
      } else if (menuIndex == 3) {
        appConfig.toggleSound();
        display.renderSettings(menuIndex, appConfig.useFeet, appConfig.soundOn);
      } else if (menuIndex == 4) {
        appState = STATE_CALIB_WAIT_EMPTY;
        display.renderCalibStep1();
      } else if (menuIndex == 5) {
        appState = STATE_NORMAL;
        isFirstRun = true;
      }
    } else if (longPress) {
      appState = STATE_NORMAL;
      isFirstRun = true;
    }
  } else if (appState == STATE_HISTORY_VIEW) {
    if (singleClick) {
      appConfig.historyIndex = (appConfig.historyIndex + 1) % appConfig.historyCount;
      display.renderHistory(appConfig.historyLogs[appConfig.historyIndex], appConfig.useFeet, appConfig.historyIndex, appConfig.historyCount);
    } else if (longPress || doubleClick) {
      appState = STATE_NORMAL;
      isFirstRun = true;
    }
  } else if (appState == STATE_CALIB_WAIT_EMPTY) {
    if (singleClick || doubleClick || longPress) {
      display.renderMessage("Sampling...");
      tester.runCalibrationSample(tempBaseCycles);
      printf("Calib Step 1 done: base = %u, %u, %u, %u\n", tempBaseCycles[0],
             tempBaseCycles[1], tempBaseCycles[2], tempBaseCycles[3]);

      appState = STATE_CALIB_WAIT_76INCH;
      display.renderCalibStep2();
    }
  } else if (appState == STATE_CALIB_WAIT_76INCH) {
    if (singleClick || doubleClick || longPress) {
      display.renderMessage("Sampling...");
      uint32_t step2Cycles[4];
      tester.runCalibrationSample(step2Cycles);

      uint32_t perM[4];
      bool ok = true;
      for (int i = 0; i < 4; i++) {
        if (step2Cycles[i] <= tempBaseCycles[i]) {
          ok = false;
          break;
        }
        float diff = step2Cycles[i] - tempBaseCycles[i];
        perM[i] = (uint32_t)(diff / 1.9304f); // 76 inches = 1.9304 meters
      }

      if (ok) {
        appConfig.saveCalibration(tempBaseCycles, perM, tester);
        display.renderCalibDone();
        delay(1000);
        appState = STATE_NORMAL;
        isFirstRun = true;
      } else {
        display.renderCalibError("Invalid Cable!", "Cap < Base");
        delay(3000);
        appState = STATE_NORMAL;
        isFirstRun = true;
      }
    }
  }

  // 只有在 NORMAL 状态下，主循环才会去不断测试电缆并刷新屏幕
  if (appState == STATE_NORMAL) {
    handleNormalState();
  }

  // 渲染设置菜单的初次绘制
  if (appState == STATE_SETTINGS && isFirstRun) {
    display.renderSettings(menuIndex, appConfig.useFeet, appConfig.soundOn);
    isFirstRun = false;
  }

  delay(MAIN_LOOP_DELAY_MS);
}
