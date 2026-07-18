#include "CableTester.h"
#include "Display.h"
#include <Arduino.h>
#include <Preferences.h>

// 实例化功能模块
CableTester tester;
Display display;
Preferences prefs;

// 状态缓存
CableStatus lastStatus;
bool isFirstRun = true;
const unsigned long SLEEP_TIMEOUT_MINUTES = 5;
volatile uint32_t lastActivityTime = 0;

volatile bool flagSingleClick = false;
volatile bool flagDoubleClick = false;
volatile bool flagLongPress = false;

const int MAX_HISTORY = 10;
CableStatus historyLogs[MAX_HISTORY];
int historyCount = 0;
int historyIndex = 0;

// 校准状态机
enum AppState {
  STATE_UNCALIBRATED_WARNING,
  STATE_NORMAL,
  STATE_HISTORY_VIEW,
  STATE_CALIB_WAIT_EMPTY,
  STATE_CALIB_WAIT_76INCH
};
AppState appState = STATE_NORMAL;
uint32_t btnPressStart = 0;
bool btnWasPressed = false;
uint32_t tempBaseCycles[4] = {0, 0, 0, 0};
bool useFeet = true;       // 默认单位为 feet
bool isCalibrated = false; // 是否已经校准过

const uint32_t UNCALIBRATED_WARNING_TIMEOUT_MS = 8000;
const uint32_t BOOT_LONG_PRESS_MS = 2000; // 长按判断阈值
uint32_t warningStartTime = 0;

const int BOOT_BTN_PIN = 0;

// ========================
// 常量定义区
// ========================
const float LENGTH_CHANGE_THRESHOLD_M = 0.2f;
const float NO_CABLE_LENGTH_THRESHOLD_M = 0.5f;
const int CALIB_PREWARM_LOOPS = 10;
const uint32_t CALIB_PREWARM_DELAY_MS = 50;
const int CALIB_SAMPLES = 16;
const uint32_t CALIB_SAMPLE_DELAY_MS = 50;
const uint32_t BTN_DOUBLE_CLICK_TIMEOUT_MS = 400;
const uint32_t BTN_POLL_INTERVAL_MS = 20;
const int STABLE_READING_COUNT = 3;
const uint32_t MAIN_LOOP_DELAY_MS = 20;

// 辅助函数：比对前后两次状态是否发生实质性变化（包括长度变化 > 0.2m）
bool isStatusEqual(const CableStatus &a, const CableStatus &b) {
  if (a.pair1 != b.pair1 || a.pair2 != b.pair2 || a.pair3 != b.pair3 ||
      a.pair4 != b.pair4)
    return false;
  if (a.shortWire1 != b.shortWire1 || a.shortWire2 != b.shortWire2 ||
      a.shortWire3 != b.shortWire3 || a.shortWire4 != b.shortWire4)
    return false;

  // 如果是断路，检查长度是否有明显变化 (>0.2m 防止数值轻微抖动引起频繁刷屏)
  auto checkLen = [](TestResult res, float l1, float l2) {
    if (res == TestResult::OPEN) {
      if (abs(l1 - l2) > LENGTH_CHANGE_THRESHOLD_M)
        return false;
    }
    return true;
  };

  if (!checkLen(a.pair1, a.len1, b.len1))
    return false;
  if (!checkLen(a.pair2, a.len2, b.len2))
    return false;
  if (!checkLen(a.pair3, a.len3, b.len3))
    return false;
  if (!checkLen(a.pair4, a.len4, b.len4))
    return false;

  if (a.hasFault != b.hasFault)
    return false;
  for (int i = 0; i < 8; i++) {
    if (a.shortNets[i] != b.shortNets[i])
      return false;
  }

  return true;
}

// 辅助函数：判断是否空载（所有线都是 OPEN，且长度都接近 0）
bool isAllOpen(const CableStatus &status) {
  return (status.pair1 == TestResult::OPEN &&
          status.pair2 == TestResult::OPEN &&
          status.pair3 == TestResult::OPEN && status.pair4 == TestResult::OPEN);
}

// 辅助函数：判断是否真的没插线（全断且长度小于 0.5m）
bool isNoCable(const CableStatus &status) {
  if (!isAllOpen(status))
    return false;
  float maxL = 0;
  if (status.len1 > maxL)
    maxL = status.len1;
  if (status.len2 > maxL)
    maxL = status.len2;
  if (status.len3 > maxL)
    maxL = status.len3;
  if (status.len4 > maxL)
    maxL = status.len4;
  return (maxL < NO_CABLE_LENGTH_THRESHOLD_M);
}

// 辅助函数：判断该状态是否包含有效的“实际长度”
bool hasActualLength(const CableStatus &status) {
  float maxL = 0;
  if (status.pair1 == TestResult::OPEN && status.len1 > maxL)
    maxL = status.len1;
  if (status.pair2 == TestResult::OPEN && status.len2 > maxL)
    maxL = status.len2;
  if (status.pair3 == TestResult::OPEN && status.len3 > maxL)
    maxL = status.len3;
  if (status.pair4 == TestResult::OPEN && status.len4 > maxL)
    maxL = status.len4;
  return (maxL >= NO_CABLE_LENGTH_THRESHOLD_M);
}

void loadCalibration() {
  prefs.begin("cable_test", true);  // 只读模式
  isCalibrated = prefs.isKey("b0"); // 检查是否存在校准数据
  uint32_t base[4];
  uint32_t perM[4];
  for (int i = 0; i < 4; i++) {
    char bKey[4], pKey[4];
    sprintf(bKey, "b%d", i);
    sprintf(pKey, "p%d", i);
    base[i] = prefs.getUInt(bKey, CableTester::DEFAULT_BASE_CYCLES);
    perM[i] = prefs.getUInt(pKey, CableTester::DEFAULT_CYCLES_PER_M);
  }
  useFeet = prefs.getBool("useFeet", true); // 读取单位偏好
  prefs.end();

  tester.setCalibrationData(base, perM);
  Serial.printf("Loaded Calib: 4-Pair OK, useFeet=%d, isCalib=%d\n", useFeet,
                isCalibrated);
}

void saveCalibration(const uint32_t base[4], const uint32_t perM[4]) {
  prefs.begin("cable_test", false); // 读写模式
  for (int i = 0; i < 4; i++) {
    char bKey[4], pKey[4];
    sprintf(bKey, "b%d", i);
    sprintf(pKey, "p%d", i);
    prefs.putUInt(bKey, base[i]);
    prefs.putUInt(pKey, perM[i]);
  }
  prefs.end();

  isCalibrated = true;
  tester.setCalibrationData(base, perM);
  Serial.printf("Saved Calibration for 4 pairs\n");
}

void loadHistory() {
  prefs.begin("cable_history", true);
  historyCount = prefs.getInt("hCount", 0);
  if (historyCount > 0 && historyCount <= MAX_HISTORY) {
    prefs.getBytes("hLogs", historyLogs, sizeof(historyLogs));
  } else {
    historyCount = 0;
  }
  prefs.end();
  Serial.printf("Loaded %d history records from Flash.\n", historyCount);
}

void saveHistoryToFlash() {
  prefs.begin("cable_history", false);
  prefs.putInt("hCount", historyCount);
  prefs.putBytes("hLogs", historyLogs, sizeof(historyLogs));
  prefs.end();
  Serial.println("Saved history to Flash.");
}

// 使用完全相同的测量序列和状态机，提取电容周期数，消除“连续读”和“跳读”造成的介质极化差异
void sampleCapacitance(uint32_t results[4]) {
  // 1. 介质预热 (Dielectric Warm-up)
  // 连续测线时，电缆一直处于被高频脉冲轰击的状态（极化饱和）。
  // 校准时，电缆是“冷”的。为了让校准数据与连续测线的数据完美吻合，必须先空跑几圈预热电缆！
  // 应用户要求，延长一倍预热时间以确保绝缘介质彻底极化
  for (int i = 0; i < CALIB_PREWARM_LOOPS; i++) {
    tester.runTest();
    delay(CALIB_PREWARM_DELAY_MS);
  }

  // 2. 正式采集 (取 16 次平均)
  // 应用户要求，增加采样次数以获得更极致的平滑效果
  for (int i = 0; i < 4; i++)
    results[i] = 0;
  for (int i = 0; i < CALIB_SAMPLES; i++) {
    CableStatus s = tester.runTest();
    results[0] += s.cycles1;
    results[1] += s.cycles2;
    results[2] += s.cycles3;
    results[3] += s.cycles4;
    delay(CALIB_SAMPLE_DELAY_MS); // 模拟主循环的间隔
  }
  for (int i = 0; i < 4; i++)
    results[i] /= CALIB_SAMPLES;
}

void buttonTask(void *pvParameters) {
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
      if (clickCount > 0 && (millis() - releaseTime > BTN_DOUBLE_CLICK_TIMEOUT_MS)) {
        if (clickCount == 1) {
          flagSingleClick = true;
          Serial.println("Button: Single Click");
        } else if (clickCount >= 2) {
          flagDoubleClick = true;
          Serial.println("Button: Double Click");
        }
        clickCount = 0;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(BTN_POLL_INTERVAL_MS)); // 轮询一次按键
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(BOOT_BTN_PIN, INPUT_PULLUP);

  // 注意：必须放在 Core 1！如果放在 Core 0，会导致 Core 0 的 digitalRead 和
  // Core 1 的寄存器直读 在极小概率下发生 APB 总线竞争（Bus Contention），导致
  // Core 1 测得的 CPU 周期数突然多出 85 个周期（约 0.2 米的误差波动）。
  xTaskCreatePinnedToCore(buttonTask, "BtnTask", 2048, NULL, 1, NULL, 1);

  tester.init();
  display.init();

  loadCalibration();
  loadHistory();

  if (!isCalibrated) {
    appState = STATE_UNCALIBRATED_WARNING;
    warningStartTime = millis();
    display.renderUncalibratedWarning(UNCALIBRATED_WARNING_TIMEOUT_MS / 1000);
  } else {
    appState = STATE_NORMAL;
    display.renderReady();
    delay(1000);
  }
  lastActivityTime = millis();
}

void handleNormalState() {
  CableStatus currentStatus = tester.runTest();

  // 为了防止拔插瞬间的针脚接触不良导致读取到乱码状态，
  // 我们采用“稳定防抖保存”策略：只要一个状态连续稳定 3 次（约 1.2
  // 秒），且不是空载，就自动保存！
  static CableStatus stableStatus;
  static int stableCount = 0;
  static CableStatus lastSavedStatus;
  static bool hasSavedInitial = false;

  if (!hasSavedInitial) {
    lastSavedStatus = currentStatus; // 避免开机立刻把默认状态存进去
    hasSavedInitial = true;
  }

  if (isStatusEqual(currentStatus, stableStatus)) {
    stableCount++;
    if (stableCount == STABLE_READING_COUNT) {
      // 稳定了 3
      // 个周期，且包含实际物理长度（>0.5m），且跟上一次保存的记录不同，就存入历史
      // 这样就完美过滤掉了全 PASS 的测试头（长度为 0）以及空载的端口
      if (hasActualLength(currentStatus) &&
          !isStatusEqual(currentStatus, lastSavedStatus)) {
        if (historyCount < MAX_HISTORY) {
          for (int i = historyCount; i > 0; i--)
            historyLogs[i] = historyLogs[i - 1];
          historyLogs[0] = currentStatus;
          historyCount++;
        } else {
          for (int i = MAX_HISTORY - 1; i > 0; i--)
            historyLogs[i] = historyLogs[i - 1];
          historyLogs[0] = currentStatus;
        }
        saveHistoryToFlash();
        lastSavedStatus = currentStatus;
        Serial.println("Auto-saved stable cable to history!");
      }
    }
  } else {
    stableStatus = currentStatus;
    stableCount = 1;
  }

  // 屏幕显示依然是实时刷新，保证 UI 响应快
  if (isFirstRun || !isStatusEqual(currentStatus, lastStatus)) {
    display.renderResult(currentStatus, useFeet, isCalibrated);
    lastStatus = currentStatus;
    isFirstRun = false;
    lastActivityTime = millis();
  }
}

void loop() {
  bool singleClick = flagSingleClick;
  flagSingleClick = false;
  bool doubleClick = flagDoubleClick;
  flagDoubleClick = false;
  bool longPress = flagLongPress;
  flagLongPress = false;

  if (appState == STATE_UNCALIBRATED_WARNING) {
    if (millis() - warningStartTime >= UNCALIBRATED_WARNING_TIMEOUT_MS) {
      appState = STATE_NORMAL;
      isFirstRun = true;
      Serial.println("Skipped calibration warning (timeout).");
    }
  }

  if (singleClick || doubleClick || longPress) {
    if (appState == STATE_UNCALIBRATED_WARNING) {
      if (longPress) {
        appState = STATE_CALIB_WAIT_EMPTY;
        display.renderCalibStep1();
      } else if (singleClick) {
        appState = STATE_NORMAL;
        isFirstRun = true;
      }
    } else if (appState == STATE_NORMAL) {
      if (longPress) {
        appState = STATE_CALIB_WAIT_EMPTY;
        display.renderCalibStep1();
      } else if (singleClick) {
        useFeet = !useFeet;
        prefs.begin("cable_test", false);
        prefs.putBool("useFeet", useFeet);
        prefs.end();
        isFirstRun = true;
      } else if (doubleClick) {
        if (historyCount > 0) {
          appState = STATE_HISTORY_VIEW;
          historyIndex = 0;
          display.renderHistory(historyLogs[historyIndex], useFeet,
                                historyIndex, historyCount);
        } else {
          display.renderCalibError("No History!", "Test a cable first.");
          delay(1000);
          isFirstRun = true; // Force redraw normal screen
        }
      }
    } else if (appState == STATE_HISTORY_VIEW) {
      if (singleClick) {
        historyIndex = (historyIndex + 1) % historyCount;
        display.renderHistory(historyLogs[historyIndex], useFeet, historyIndex,
                              historyCount);
      } else if (doubleClick || longPress) {
        appState = STATE_NORMAL;
        isFirstRun = true;
      }
    } else if (appState == STATE_CALIB_WAIT_EMPTY) {
      if (singleClick) {
        CableStatus s = tester.runTest();
        if (!isAllOpen(s)) {
          display.renderCalibError("Short Detected!", "Insert EMPTY plug.");
          delay(2000);
          display.renderCalibStep1();
        } else {
          display.renderMeasuring();
          delay(100);
          sampleCapacitance(tempBaseCycles);
          appState = STATE_CALIB_WAIT_76INCH;
          display.renderCalibStep2();
        }
      }
    } else if (appState == STATE_CALIB_WAIT_76INCH) {
      if (singleClick) {
        display.renderMeasuring();
        delay(100);
        uint32_t testCycles[4];
        sampleCapacitance(testCycles);

        bool valid = true;
        uint32_t cyclesPerM[4];
        for (int i = 0; i < 4; i++) {
          if (testCycles[i] <= tempBaseCycles[i]) {
            valid = false;
            break;
          }
          uint32_t delta = testCycles[i] - tempBaseCycles[i];
          cyclesPerM[i] =
              (uint32_t)(delta / CableTester::CALIBRATION_CABLE_LENGTH_M);
        }

        if (valid) {
          saveCalibration(tempBaseCycles, cyclesPerM);
          display.renderCalibDone();
        } else {
          display.renderCalibFailed();
        }
        delay(2000);
        appState = STATE_NORMAL;
        isFirstRun = true;
      }
    }
  }

  if (appState == STATE_NORMAL) {
    handleNormalState();
  }

  if (millis() - lastActivityTime > SLEEP_TIMEOUT_MINUTES * 60 * 1000UL) {
    Serial.println("Inactivity timeout. Entering deep sleep...");
    display.sleep();
    delay(100);
    esp_deep_sleep_start();
  }

  delay(MAIN_LOOP_DELAY_MS);
}
