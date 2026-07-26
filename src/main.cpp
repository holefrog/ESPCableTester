/**
 * @file main.cpp
 * @brief 主程序入口 —— 状态机调度、按键分发、测线循环
 *
 * 项目概览
 * --------
 * ESP32 单端网线测试仪。用户只需在远端插入物理短接水晶头，
 * 近端 OLED 屏幕即可显示：通断状态、错线拓扑图、线缆长度估算、历史记录。
 *
 * 模块依赖关系
 * ------------
 *
 *   main.cpp
 *   ├── ButtonHandler   按键事件（单击/双击/长按）→ 驱动状态转移
 *   ├── CableTester     执行 TDR 测量 → 返回 CableStatus 结构体
 *   ├── Display         将 CableStatus 渲染到 OLED 屏幕
 *   └── AppConfig       读写 NVS Flash（偏好设置、校准参数、历史记录）
 *
 * 双核分工
 * --------
 * Core 0（PRO CPU）: 暂未使用（FreeRTOS 系统任务）
 * Core 1（APP CPU）: 主循环 loop() + ButtonHandler 后台任务
 *   - ButtonHandler 任务轻量（仅 20ms 轮询一次 digitalRead），
 *     不会与主循环的 TDR 测量（寄存器直读）产生 APB 总线竞争。
 *
 * 主状态机
 * --------
 *
 *  ┌─────────────────────────┐
 *  │ STATE_UNCALIBRATED_     │  开机无校准数据时显示警告，8 秒或任意键后进入 NORMAL
 *  │ WARNING                 │
 *  └──────────┬──────────────┘
 *             │ 超时 / 任意键
 *             ▼
 *  ┌─────────────────────────┐  长按
 *  │ STATE_NORMAL            │──────────► STATE_SETTINGS
 *  │ 持续测线并刷新屏幕       │
 *  └─────────────────────────┘
 *
 *  STATE_SETTINGS ──单击──► 菜单循环（MENU_COUNT 项）
 *                 ──双击──► 执行菜单项（查看历史 / 校准 / 切换单位等）
 *                 ──长按──► 返回 STATE_NORMAL
 *
 *  STATE_HISTORY_VIEW ──单击──► 翻到下一条记录
 *                     ──长按/双击──► 返回 STATE_SETTINGS
 *
 *  STATE_CALIB_WAIT_EMPTY ──任意键──► 采样空载基准 → STATE_CALIB_WAIT_76INCH
 *  STATE_CALIB_WAIT_76INCH ──任意键──► 采样 76 英寸线 → 计算校准系数 → STATE_NORMAL
 *
 * 关键全局变量
 * ------------
 * appState      当前状态机状态
 * menuIndex     设置菜单当前选中项（0 ~ MENU_COUNT-1）
 * isFirstRun    进入 STATE_NORMAL 后是否为第一次测量（跳过防抖直接显示）
 * historyIndex  历史记录翻页指针（UI 导航状态，不属于 AppConfig）
 * lastStatus    上一次显示的测量结果（用于防止重复写历史）
 * tempBaseCycles 校准过程中暂存的第一步空载基准值
 */
#include "CableTester.h"
#include "Display.h"
#include "ButtonHandler.h"
#include "Buzzer.h"
#include "AppConfig.h"
#include <Arduino.h>

CableTester tester;
Display display;

CableStatus lastStatus;
bool isFirstRun = true;
int historyIndex = 0;  // UI 导航状态：当前显示的历史条目序号

const unsigned long SLEEP_TIMEOUT_MINUTES = 5;
const uint32_t UNCALIBRATED_WARNING_TIMEOUT_MS = 8000;
uint32_t warningStartTime = 0;
uint32_t tempBaseCycles[4] = {0, 0, 0, 0};

// 防抖机制：连续测得多少次相同的结果才视为状态稳定并保存历史记录
const int STABLE_READING_COUNT = 3;

// 主状态机主循环 (loop) 的间隔延时
const uint32_t MAIN_LOOP_DELAY_MS = 20;

// P0: 校准参数合理性边界 (CAT5e/6 VF ~0.64~0.67, ~400~600 cycles/m; 边界留余量)
const uint32_t CALIB_CYCLES_PER_M_MIN = 350;  // 低于此值说明校准线太短
const uint32_t CALIB_CYCLES_PER_M_MAX = 750;  // 高于此值说明校准线太长

// P3: 主状态机状态枚举
enum AppState {
  STATE_UNCALIBRATED_WARNING,
  STATE_NORMAL,
  STATE_HISTORY_VIEW,
  STATE_CALIB_WAIT_EMPTY,
  STATE_CALIB_WAIT_76INCH,
  STATE_SETTINGS
};

// P3: 设置菜单项枚举已移至 Display.h

AppState appState = STATE_NORMAL;
int menuIndex = 0;

// =========================================================================
// 状态处理函数前置声明
// =========================================================================
void handleNormalState();
void handleUncalibratedWarning(ButtonEvent btnEvt);
void handleSettings(ButtonEvent btnEvt);
void handleHistoryView(ButtonEvent btnEvt);
void handleCalibration(ButtonEvent btnEvt);

// =========================================================================
// setup
// =========================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  ButtonHandler::init();
  tester.init();
  display.init();
  buzzer.begin(4); // Initialize buzzer on GPIO 4

  appConfig.loadAll(tester);

  if (!appConfig.isCalibrated) {
    appState = STATE_UNCALIBRATED_WARNING;
    warningStartTime = millis();
    display.renderUncalibratedWarning(UNCALIBRATED_WARNING_TIMEOUT_MS / 1000);
    buzzer.play(BEEP_ERROR);
  } else {
    appState = STATE_NORMAL;
    display.renderReady();
    buzzer.play(BEEP_STARTUP);
    
    // 用非阻塞循环代替 delay(1000)，以保证开机音效正常播放
    uint32_t waitStart = millis();
    while (millis() - waitStart < 1000) {
      buzzer.update();
      delay(10);
    }
  }
}

// =========================================================================
// STATE_NORMAL：测线防抖 + 刷屏 + 保存历史（P1 已统一两套逻辑）
// =========================================================================
void handleNormalState() {
  CableStatus currentStatus = tester.runTest();

  // 防抖计数：连续多次测得相同状态才视为稳定
  static CableStatus debouncedStatus;
  static int stableCount = 0;

  if (CableTester::isStatusEqual(currentStatus, debouncedStatus)) {
    stableCount++;
  } else {
    stableCount = 0;
    debouncedStatus = currentStatus;
  }

  // P1: 统一触发条件——开机第一次立即显示，或状态连续稳定后更新
  // isFirstRun 时不经防抖直接显示，属于设计取舍（快速响应优先）
  bool shouldUpdate = isFirstRun || (stableCount == STABLE_READING_COUNT);

  if (shouldUpdate) {
    if (!CableTester::isNoCable(currentStatus)) {
      display.renderResult(currentStatus, appConfig.useFeet, appConfig.isCalibrated);

      // 检查是否有任何线对物理测通 (PASS)
      bool isAnyPass = (currentStatus.pair1 == TestResult::PASS || 
                        currentStatus.pair2 == TestResult::PASS || 
                        currentStatus.pair3 == TestResult::PASS || 
                        currentStatus.pair4 == TestResult::PASS);

      // 统一保存逻辑：如果有故障，或者有实际长度(OPEN)，或者是通的(PASS)
      if (CableTester::hasActualLength(currentStatus) || currentStatus.hasFault || isAnyPass) {
        if (!CableTester::isStatusEqual(currentStatus, lastStatus)) {
          lastStatus = currentStatus;
          appConfig.addHistory(currentStatus);
          
          if (currentStatus.hasFault) {
            buzzer.play(BEEP_ERROR);
          } else if (isAnyPass) {
            buzzer.play(BEEP_PASS);
          } else {
            buzzer.play(BEEP_LONG);
          }
        }
      }
    } else {
      display.renderReady();
    }

    isFirstRun = false;
  }
}

// =========================================================================
// STATE_UNCALIBRATED_WARNING：倒计时提示，任意键或超时后进入 NORMAL
// =========================================================================
void handleUncalibratedWarning(ButtonEvent btnEvt) {
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
}

// =========================================================================
// STATE_SETTINGS：菜单导航 + 菜单项响应（P3：使用 SettingsMenuItem enum）
// =========================================================================
void handleSettings(ButtonEvent btnEvt) {
  if (btnEvt == BTN_EVENT_SINGLE_CLICK) {
    menuIndex = (menuIndex + 1) % MENU_COUNT;
    display.renderSettings(menuIndex, appConfig.useFeet, appConfig.soundOn);

  } else if (btnEvt == BTN_EVENT_DOUBLE_CLICK) {
    switch (menuIndex) {
      case MENU_VIEW_HISTORY:
        if (appConfig.historyCount > 0) {
          appState = STATE_HISTORY_VIEW;
          historyIndex = 0;
          display.renderHistory(appConfig.historyLogs[historyIndex],
                                appConfig.useFeet, historyIndex,
                                appConfig.historyCount);
        } else {
          display.renderMessage("No History!");
          delay(1000);
          display.renderSettings(menuIndex, appConfig.useFeet, appConfig.soundOn);
        }
        break;

      case MENU_CLEAR_HISTORY:
        appConfig.clearHistory();
        printf("History Cleared!\n");
        display.renderMessage("History Cleared!");
        delay(1000);
        display.renderSettings(menuIndex, appConfig.useFeet, appConfig.soundOn);
        break;

      case MENU_TOGGLE_UNIT:
        appConfig.toggleUnit();
        display.renderSettings(menuIndex, appConfig.useFeet, appConfig.soundOn);
        break;

      case MENU_TOGGLE_SOUND:
        appConfig.toggleSound();
        display.renderSettings(menuIndex, appConfig.useFeet, appConfig.soundOn);
        break;

      case MENU_CALIBRATE:
        appState = STATE_CALIB_WAIT_EMPTY;
        display.renderCalibStep1();
        break;

      case MENU_EXIT:
        appState = STATE_NORMAL;
        isFirstRun = true;
        break;
    }

  } else if (btnEvt == BTN_EVENT_LONG_PRESS) {
    appState = STATE_NORMAL;
    isFirstRun = true;
  }
}

// =========================================================================
// STATE_HISTORY_VIEW：翻页浏览历史记录
// =========================================================================
void handleHistoryView(ButtonEvent btnEvt) {
  if (btnEvt == BTN_EVENT_SINGLE_CLICK) {
    historyIndex = (historyIndex + 1) % appConfig.historyCount;
    display.renderHistory(appConfig.historyLogs[historyIndex],
                          appConfig.useFeet, historyIndex,
                          appConfig.historyCount);
  } else if (btnEvt == BTN_EVENT_LONG_PRESS || btnEvt == BTN_EVENT_DOUBLE_CLICK) {
    historyIndex = 0;  // 退出时重置，下次进入历史从头开始
    appState = STATE_NORMAL;
    isFirstRun = true;
  }
}

// =========================================================================
// STATE_CALIB_WAIT_EMPTY / STATE_CALIB_WAIT_76INCH：两步校准流程
// =========================================================================
void handleCalibration(ButtonEvent btnEvt) {
  bool anyKey = (btnEvt == BTN_EVENT_SINGLE_CLICK ||
                 btnEvt == BTN_EVENT_DOUBLE_CLICK ||
                 btnEvt == BTN_EVENT_LONG_PRESS);

  if (!anyKey) return;

  if (appState == STATE_CALIB_WAIT_EMPTY) {
    display.renderMessage("Sampling...");
    tester.runCalibrationSample(tempBaseCycles);
    printf("Calib Step 1 done: base = %u, %u, %u, %u\n", tempBaseCycles[0],
           tempBaseCycles[1], tempBaseCycles[2], tempBaseCycles[3]);

    appState = STATE_CALIB_WAIT_76INCH;
    display.renderCalibStep2();

  } else if (appState == STATE_CALIB_WAIT_76INCH) {
    display.renderMessage("Sampling...");
    uint32_t step2Cycles[4];
    tester.runCalibrationSample(step2Cycles);

    // P0: 用独立枚举记录失败原因，避免访问未初始化的 perM
    enum CalibFailReason { CALIB_OK, CALIB_FAIL_DELTA, CALIB_FAIL_TOO_SHORT, CALIB_FAIL_TOO_LONG };
    CalibFailReason failReason = CALIB_OK;
    uint32_t perM[4] = {0, 0, 0, 0};

    for (int i = 0; i < 4; i++) {
      if (step2Cycles[i] <= tempBaseCycles[i]) {
        failReason = CALIB_FAIL_DELTA;
        printf("ERROR: calib step2[%d]=%u <= base[%d]=%u\n",
               i, step2Cycles[i], i, tempBaseCycles[i]);
        break;
      }
      float diff = (float)(step2Cycles[i] - tempBaseCycles[i]);
      perM[i] = (uint32_t)(diff / 1.9304f); // 76 inches = 1.9304 meters

      if (perM[i] < CALIB_CYCLES_PER_M_MIN) {
        failReason = CALIB_FAIL_TOO_SHORT;
        printf("ERROR: perM[%d]=%u < MIN(%u), cable too short\n",
               i, perM[i], CALIB_CYCLES_PER_M_MIN);
        break;
      }
      if (perM[i] > CALIB_CYCLES_PER_M_MAX) {
        failReason = CALIB_FAIL_TOO_LONG;
        printf("ERROR: perM[%d]=%u > MAX(%u), cable too long\n",
               i, perM[i], CALIB_CYCLES_PER_M_MAX);
        break;
      }
    }

    if (failReason == CALIB_OK) {
      appConfig.saveCalibration(tempBaseCycles, perM, tester);
      display.renderCalibDone();
      buzzer.play(BEEP_LONG);
      delay(1000);
      appState = STATE_NORMAL;
      isFirstRun = true;
    } else {
      const char* errDetail;
      switch (failReason) {
        case CALIB_FAIL_TOO_SHORT: errDetail = "Cable too short!"; break;
        case CALIB_FAIL_TOO_LONG:  errDetail = "Cable too long!";  break;
        default:                   errDetail = "Check connection"; break;
      }
      display.renderCalibError("Invalid Calib", errDetail);
      buzzer.play(BEEP_ERROR);
      delay(3000);
      appState = STATE_NORMAL;
      isFirstRun = true;
    }
  }
}

// =========================================================================
// loop：统一休眠检测 + switch 状态分发（P3 重构后核心逻辑 <20 行）
// =========================================================================
void loop() {
  buzzer.update();

  ButtonEvent btnEvt = ButtonHandler::getEvent();
  
  if (btnEvt != BTN_EVENT_NONE) {
    if (btnEvt == BTN_EVENT_SINGLE_CLICK) buzzer.play(BEEP_SHORT);
    else if (btnEvt == BTN_EVENT_DOUBLE_CLICK) buzzer.play(BEEP_DOUBLE);
    else if (btnEvt == BTN_EVENT_LONG_PRESS) buzzer.play(BEEP_LONG);
  }

  // 统一的前置检查：非警告状态下检测空闲超时并进入深度睡眠
  if (appState != STATE_UNCALIBRATED_WARNING) {
    if (ButtonHandler::isIdleTimeout(SLEEP_TIMEOUT_MINUTES)) {
      display.renderMessage("Sleeping...");
      buzzer.play(BEEP_SLEEP);
      
      uint32_t waitStart = millis();
      while (millis() - waitStart < 1000) {
        buzzer.update();
        delay(10);
      }
      
      display.sleep();
      esp_deep_sleep_start();
    }
  }

  // 状态分发：每个状态对应独立的 handle 函数
  switch (appState) {
    case STATE_UNCALIBRATED_WARNING:             handleUncalibratedWarning(btnEvt); break;
    case STATE_NORMAL:
      if (btnEvt == BTN_EVENT_LONG_PRESS) {
        appState = STATE_SETTINGS;
        menuIndex = 0;
        isFirstRun = true;
      }
      break;
    case STATE_SETTINGS:                         handleSettings(btnEvt);            break;
    case STATE_HISTORY_VIEW:                     handleHistoryView(btnEvt);         break;
    case STATE_CALIB_WAIT_EMPTY:
    case STATE_CALIB_WAIT_76INCH:                handleCalibration(btnEvt);         break;
  }

  // 只有在 NORMAL 状态下，主循环才会不断测试电缆并刷新屏幕
  if (appState == STATE_NORMAL) {
    handleNormalState();
  }

  // 设置菜单首次进入时渲染
  if (appState == STATE_SETTINGS && isFirstRun) {
    display.renderSettings(menuIndex, appConfig.useFeet, appConfig.soundOn);
    isFirstRun = false;
  }

  delay(MAIN_LOOP_DELAY_MS);
}
