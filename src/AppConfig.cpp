#include "AppConfig.h"
#include <Preferences.h>

AppConfig appConfig;
static Preferences prefs;

AppConfig::AppConfig() {
    useFeet = true;
    soundOn = true;
    isCalibrated = false;
    historyCount = 0;
}

void AppConfig::loadAll(CableTester& tester) {
    loadCalibration(tester);
    loadHistory();
}

void AppConfig::loadCalibration(CableTester& tester) {
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
    soundOn = prefs.getBool("soundOn", true);
    prefs.end();
    
    tester.setCalibrationData(base, perM);
    printf("Loaded Calib: 4-Pair OK, useFeet=%d, sound=%d, isCalib=%d\n", useFeet, soundOn, isCalibrated);
}

void AppConfig::saveCalibration(const uint32_t base[4], const uint32_t perM[4], CableTester& tester) {
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
    printf("Saved Calibration for 4 pairs\n");
}

void AppConfig::toggleUnit() {
    useFeet = !useFeet;
    prefs.begin("cable_test", false);
    prefs.putBool("useFeet", useFeet);
    prefs.end();
}

void AppConfig::toggleSound() {
    soundOn = !soundOn;
    prefs.begin("cable_test", false);
    prefs.putBool("soundOn", soundOn);
    prefs.end();
}

void AppConfig::loadHistory() {
    prefs.begin("cable_history", true);
    historyCount = prefs.getInt("hCount", 0);
    if (historyCount > 0 && historyCount <= MAX_HISTORY) {
        prefs.getBytes("hLogs", historyLogs, sizeof(historyLogs));
    } else {
        historyCount = 0;
    }
    prefs.end();
    printf("Loaded %d history records from Flash.\n", historyCount);
}

void AppConfig::saveHistoryToFlash() {
    prefs.begin("cable_history", false);
    prefs.putInt("hCount", historyCount);
    prefs.putBytes("hLogs", historyLogs, sizeof(historyLogs));
    prefs.end();
    printf("Saved history to Flash.\n");
}

void AppConfig::addHistory(const CableStatus& status) {
    if (historyCount < MAX_HISTORY) {
        historyCount++;
    } else {
        // Shift history down
        for (int i = 0; i < MAX_HISTORY - 1; i++) {
            historyLogs[i] = historyLogs[i + 1];
        }
    }
    historyLogs[historyCount - 1] = status;
    saveHistoryToFlash();
}

void AppConfig::clearHistory() {
    historyCount = 0;
    saveHistoryToFlash();
}
