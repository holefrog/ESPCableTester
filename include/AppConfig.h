#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <Arduino.h>
#include "CableTester.h"

class AppConfig {
public:
    static const int MAX_HISTORY = 10;
    
    // 全局设置
    bool useFeet;
    bool soundOn;
    bool isCalibrated;
    
    // 历史记录
    CableStatus historyLogs[MAX_HISTORY];
    int historyCount;
    int historyIndex;

    AppConfig();

    // 初始化并加载配置
    void loadAll(CableTester& tester);

    // 校准操作
    void saveCalibration(const uint32_t base[4], const uint32_t perM[4], CableTester& tester);

    // 偏好设置操作
    void toggleUnit();
    void toggleSound();

    // 历史记录操作
    void addHistory(const CableStatus& status);
    void clearHistory();
    void saveHistoryToFlash();

private:
    void loadCalibration(CableTester& tester);
    void loadHistory();
};

// 暴露全局实例
extern AppConfig appConfig;

#endif // APP_CONFIG_H
