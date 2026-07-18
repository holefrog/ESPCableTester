#include <Arduino.h>
#include <Preferences.h>
#include "CableTester.h"
#include "Display.h"

// 实例化功能模块
CableTester tester;
Display display;
Preferences prefs;

// 状态缓存
CableStatus lastStatus;
bool isFirstRun = true;

// 校准状态机
enum AppState {
    STATE_UNCALIBRATED_WARNING,
    STATE_NORMAL,
    STATE_CALIB_WAIT_EMPTY,
    STATE_CALIB_WAIT_76INCH
};
AppState appState = STATE_NORMAL;
uint32_t btnPressStart = 0;
bool btnWasPressed = false;
uint32_t tempBaseCycles[4] = {0, 0, 0, 0};
bool useFeet = true; // 默认单位为 feet
bool isCalibrated = false; // 是否已经校准过

const uint32_t UNCALIBRATED_WARNING_TIMEOUT_MS = 8000;
const uint32_t BOOT_LONG_PRESS_MS = 2000; // 长按判断阈值
uint32_t warningStartTime = 0;

const int BOOT_BTN_PIN = 0;

// 辅助函数：比对前后两次状态是否发生实质性变化（包括长度变化 > 0.2m）
bool isStatusEqual(const CableStatus& a, const CableStatus& b) {
    if (a.pair1 != b.pair1 || a.pair2 != b.pair2 || a.pair3 != b.pair3 || a.pair4 != b.pair4) return false;
    if (a.shortWire1 != b.shortWire1 || a.shortWire2 != b.shortWire2 || a.shortWire3 != b.shortWire3 || a.shortWire4 != b.shortWire4) return false;
    
    // 如果是断路，检查长度是否有明显变化 (>0.2m 防止数值轻微抖动引起频繁刷屏)
    auto checkLen = [](TestResult res, float l1, float l2) {
        if (res == TestResult::OPEN) {
            if (abs(l1 - l2) > 0.2f) return false;
        }
        return true;
    };
    
    if (!checkLen(a.pair1, a.len1, b.len1)) return false;
    if (!checkLen(a.pair2, a.len2, b.len2)) return false;
    if (!checkLen(a.pair3, a.len3, b.len3)) return false;
    if (!checkLen(a.pair4, a.len4, b.len4)) return false;
    
    return true;
}

// 辅助函数：判断是否空载（考虑到长度）
bool isAllOpen(const CableStatus& status) {
    return (status.pair1 == TestResult::OPEN && 
            status.pair2 == TestResult::OPEN && 
            status.pair3 == TestResult::OPEN && 
            status.pair4 == TestResult::OPEN);
}

void loadCalibration() {
    prefs.begin("cable_test", true); // 只读模式
    isCalibrated = prefs.isKey("b0"); // 检查是否存在校准数据
    uint32_t base[4];
    uint32_t perM[4];
    for (int i = 0; i < 4; i++) {
        char bKey[4], pKey[4];
        sprintf(bKey, "b%d", i);
        sprintf(pKey, "p%d", i);
        base[i] = prefs.getUInt(bKey, 150);
        perM[i] = prefs.getUInt(pKey, 540);
    }
    useFeet = prefs.getBool("useFeet", true); // 读取单位偏好
    prefs.end();
    
    tester.setCalibrationData(base, perM);
    Serial.printf("Loaded Calib: 4-Pair OK, useFeet=%d, isCalib=%d\n", useFeet, isCalibrated);
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

// 使用完全相同的测量序列和状态机，提取电容周期数，消除“连续读”和“跳读”造成的介质极化差异
void sampleCapacitance(uint32_t results[4]) {
    // 1. 介质预热 (Dielectric Warm-up)
    // 连续测线时，电缆一直处于被高频脉冲轰击的状态（极化饱和）。
    // 校准时，电缆是“冷”的。为了让校准数据与连续测线的数据完美吻合，必须先空跑几圈预热电缆！
    // 应用户要求，延长一倍预热时间以确保绝缘介质彻底极化
    for (int i = 0; i < 10; i++) {
        tester.runTest();
        delay(50);
    }
    
    // 2. 正式采集 (取 16 次平均)
    // 应用户要求，增加采样次数以获得更极致的平滑效果
    for (int i = 0; i < 4; i++) results[i] = 0;
    const int SAMPLES = 16;
    for (int i = 0; i < SAMPLES; i++) {
        CableStatus s = tester.runTest();
        results[0] += s.cycles1;
        results[1] += s.cycles2;
        results[2] += s.cycles3;
        results[3] += s.cycles4;
        delay(50); // 模拟主循环的间隔
    }
    for (int i = 0; i < 4; i++) results[i] /= SAMPLES;
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    pinMode(BOOT_BTN_PIN, INPUT_PULLUP);
    
    tester.init();
    display.init();
    
    loadCalibration();
    
    if (!isCalibrated) {
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
    
    if (isFirstRun || !isStatusEqual(currentStatus, lastStatus)) {
        display.renderResult(currentStatus, useFeet, isCalibrated);
        lastStatus = currentStatus;
        isFirstRun = false;
    }
}

void loop() {
    // 读取 BOOT 按键
    bool btnIsPressed = (digitalRead(BOOT_BTN_PIN) == LOW);
    
    if (btnIsPressed && !btnWasPressed) {
        btnPressStart = millis();
    }
    
    // 优先处理超时逻辑
    if (appState == STATE_UNCALIBRATED_WARNING) {
        if (!btnIsPressed && !btnWasPressed && (millis() - warningStartTime >= UNCALIBRATED_WARNING_TIMEOUT_MS)) {
            appState = STATE_NORMAL;
            isFirstRun = true;
            Serial.println("Skipped calibration warning (timeout).");
        }
    }
    
    // 按键松开检测
    if (!btnIsPressed && btnWasPressed) {
        uint32_t pressDuration = millis() - btnPressStart;
        
        if (appState == STATE_UNCALIBRATED_WARNING) {
            if (pressDuration >= BOOT_LONG_PRESS_MS) {
                // 长按进入校准
                appState = STATE_CALIB_WAIT_EMPTY;
                display.renderCalibStep1();
                Serial.println("Enter Calibration from Warning: Wait for empty plug...");
            } else if (pressDuration > 50) {
                // 短按跳过警告，进入正常测线模式（无长度）
                appState = STATE_NORMAL;
                isFirstRun = true;
                Serial.println("Skipped calibration warning (button).");
            }
        }
        else if (appState == STATE_NORMAL) {
            if (pressDuration >= BOOT_LONG_PRESS_MS) {
                // 长按 2 秒进入校准
                appState = STATE_CALIB_WAIT_EMPTY;
                display.renderCalibStep1();
                Serial.println("Enter Calibration: Wait for empty plug...");
            } else if (pressDuration > 50) {
                // 短按切换单位
                useFeet = !useFeet;
                prefs.begin("cable_test", false);
                prefs.putBool("useFeet", useFeet);
                prefs.end();
                isFirstRun = true; // 强制刷新界面
                Serial.printf("Toggled unit to %s\n", useFeet ? "feet" : "meters");
            }
        } 
        else if (appState == STATE_CALIB_WAIT_EMPTY && pressDuration < 1000) {
            // 短按：尝试完成第一步
            CableStatus s = tester.runTest();
            if (!isAllOpen(s)) {
                // 如果存在短路，大概率是插座里的开关没被顶开，说明没插空头
                display.renderCalibError("Short Detected!", "Insert EMPTY plug.");
                Serial.println("Calibration Error: Short detected in Step 1. Please insert a bare empty plug.");
                delay(2000);
                display.renderCalibStep1(); // 恢复提示
                return; // 拒绝进入下一步
            }

            display.renderMeasuring(); // 显示正在测量...
            delay(100);
            
            sampleCapacitance(tempBaseCycles);
            Serial.printf("Base cycles: %u, %u, %u, %u\n", tempBaseCycles[0], tempBaseCycles[1], tempBaseCycles[2], tempBaseCycles[3]);
            
            appState = STATE_CALIB_WAIT_76INCH;
            display.renderCalibStep2();
        }
        else if (appState == STATE_CALIB_WAIT_76INCH && pressDuration < 1000) {
            // 短按：完成第二步
            display.renderMeasuring(); // 显示正在测量...
            delay(100);
            
            uint32_t testCycles[4];
            sampleCapacitance(testCycles);
            Serial.printf("76-inch cycles: %u, %u, %u, %u\n", testCycles[0], testCycles[1], testCycles[2], testCycles[3]);
            
            bool valid = true;
            uint32_t cyclesPerM[4];
            for (int i = 0; i < 4; i++) {
                if (testCycles[i] <= tempBaseCycles[i]) {
                    valid = false;
                    break;
                }
                uint32_t delta = testCycles[i] - tempBaseCycles[i];
                cyclesPerM[i] = (uint32_t)(delta / CableTester::CALIBRATION_CABLE_LENGTH_M);
            }
            
            if (valid) {
                saveCalibration(tempBaseCycles, cyclesPerM);
                display.renderCalibDone();
            } else {
                display.renderCalibFailed();
                Serial.println("Calibration Failed: Some 76-inch cycles <= Base cycles.");
            }
            
            delay(2000);
            
            appState = STATE_NORMAL;
            isFirstRun = true; // 强制刷新主界面
        }
    }
    
    btnWasPressed = btnIsPressed;
    
    // 只有在 NORMAL 状态下才跑正常的测线逻辑
    if (appState == STATE_NORMAL) {
        handleNormalState();
    }
    
    delay(50); // 主循环略微减速，让按键手感好点
}
