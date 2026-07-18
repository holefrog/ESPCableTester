#ifndef CABLE_TESTER_H
#define CABLE_TESTER_H

#include <Arduino.h>

// 业务目的：定义线缆测试可能出现的结果，便于上层逻辑进行 UI 渲染
enum class TestResult {
    PASS,           // 通过（匹配对导通，且无短路）
    OPEN,           // 断路（匹配对未导通）
    SHORT_OR_CROSS, // 短路或错线（非匹配对导通）
    NOT_TESTED      // 未测试状态
};

// 业务目的：保存一条 8 芯网线 4 个线对的整体测试结果及可能的断点长度估算
struct CableStatus {
    TestResult pair1; // 1-2 (橙)
    TestResult pair2; // 3-6 (绿)
    TestResult pair3; // 4-5 (蓝)
    TestResult pair4; // 7-8 (棕)
    
    // 断路时的长度估算（单位：米）
    float len1;
    float len2;
    float len3;
    float len4;

    // 原始周期数（用于精准校准）
    uint32_t cycles1;
    uint32_t cycles2;
    uint32_t cycles3;
    uint32_t cycles4;
};

class CableTester {
public:
    CableTester();
    
    // 初始化引脚，设置为默认的安全状态
    void init();
    
    // 执行一次完整的扫描测试，并返回状态结构体
    CableStatus runTest();

    // 校准常量：76英寸（约1.9304米）
    static constexpr float CALIBRATION_CABLE_LENGTH_M = 1.9304f;
    static constexpr const char* CALIBRATION_CABLE_NAME = "76-inch";

    // 设置校准数据（由外层通过 Preferences 加载后注入）
    void setCalibrationData(const uint32_t base[4], const uint32_t perMeter[4]);
    
    // 获取/进行单次电容测量，用于校准模式（求平均值需要多次测量）
    uint32_t measureCapacitanceCycles(uint8_t txPin, uint8_t rxPin);

private:
    // 测试具体的某一个线对
    // 参数 txPin: 发射脉冲的引脚
    // 参数 expectedRxPin: 预期收到脉冲的接收引脚
    // 参数 pairIndex: 对应的线对索引 (0-3)
    // 参数 outCycles: 输出该线对的原始充电周期数
    TestResult testSinglePair(uint8_t txPin, uint8_t expectedRxPin, float& outLength, int pairIndex, uint32_t& outCycles);
    
    // 重置所有测试引脚为下拉输入（安全且防止悬空干扰）
    void resetAllPins();
    
    // 将电容充电周期数转换为长度
    float cyclesToMeters(uint32_t cycles, int pairIndex);

    // 校准参数
    uint32_t baseCycles[4];
    uint32_t cyclesPerMeter[4];
    
    // 所有的网线测试引脚
    static const uint8_t PINS[8];
};

#endif // CABLE_TESTER_H
