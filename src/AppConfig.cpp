/**
 * @file AppConfig.cpp
 * @brief AppConfig 实现——NVS 读写与历史 FIFO 管理
 *
 * NVS Key 映射
 * ------------
 * "cable_test" 命名空间：
 *   b0~b3    uint32  各线对基准充电周期数（空载基准）
 *   p0~p3    uint32  各线对每米充电周期增量（校准系数）
 *   useFeet  bool    长度单位偏好
 *   soundOn  bool    蜂鸣器开关偏好
 *
 * "cable_history" 命名空间：
 *   hCount   int32   当前有效历史记录条数
 *   hLogs    bytes   CableStatus 数组的二进制序列化（sizeof(historyLogs) 字节）
 *
 * FIFO 逻辑
 * ---------
 * - 未满时：historyLogs[historyCount++] = 新记录
 * - 已满时：向左平移一位（[i] = [i+1]），再写入 [MAX_HISTORY-1]
 *   结果：下标 0 始终是最旧记录，下标 historyCount-1 始终是最新记录
 */
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
