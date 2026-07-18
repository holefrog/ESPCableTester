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

    // 记录如果发生短路，是和哪个引脚短路的 (0=GND, 1-8=芯号)
    uint8_t shortWire1;
    uint8_t shortWire2;
    uint8_t shortWire3;
    uint8_t shortWire4;

    // 原始周期数（用于精准校准）
    uint32_t cycles1;
    uint32_t cycles2;
    uint32_t cycles3;
    uint32_t cycles4;

    // 完整的短路网络图，0=无短路，>0 代表同一个短接网络的 Net ID
    uint8_t shortNets[8];

    // 是否存在非法的短路（用于决定是否全屏显示图形化Wiremap）
    bool hasFault;
};

class CableTester {
public:
    CableTester();
    
    // 初始化引脚，设置为默认的安全状态
    void init();
    
    // 执行一次完整的扫描测试，并返回状态结构体
    CableStatus runTest();

    static constexpr float CALIBRATION_CABLE_LENGTH_M = 1.9304f;
    static constexpr const char* CALIBRATION_CABLE_NAME = "76-inch";

    // 测试与校准相关常量
    
    // 首次开机未经校准时，假设电缆未接（空载）的基准充电周期数
    // [建议值: 150 ~ 200] 取决于 ESP32 引脚的内部寄生电容大小
    static constexpr uint32_t DEFAULT_BASE_CYCLES = 150;
    
    // 首次开机未经校准时，假设电缆每增加1米，电容充电所需增加的周期数
    // [建议值: 450 ~ 600] 典型 CAT5e/CAT6 网线的值
    static constexpr uint32_t DEFAULT_CYCLES_PER_M = 540;
    
    // 如果充电周期超过此值，判定为发生“对地短路”故障（电容无法充满）
    // [建议值: 15000000 ~ 20000000] 在 240MHz 下，2000万周期约为 83 毫秒
    static constexpr uint32_t TIMEOUT_CYCLES = 20000000;
    
    // 硬件轮询电容电压升高的最大超时时间
    // [建议值: 24000000] 在 240MHz 频率下精确等于 100 毫秒，超时则终止死循环
    static constexpr uint32_t MAX_POLL_CYCLES = 24000000;
    
    // 锁相积分采样的死区时间 (10000微秒即 10ms)。
    // 用于 100% 抵消 50Hz (20ms) 和 60Hz (16.6ms) 市电造成的交流寄生电容干扰
    // [建议值: 10000] 10ms 是 50Hz 半周期，这是消除市电干扰的完美公约数！
    static constexpr uint32_t PHASE_LOCK_INTERVAL_US = 10000;
    
    // 每次单根线段测量的锁相积分采样次数
    // [建议值: 10] 10 次 10ms 刚好凑够 100 毫秒的整周期积分时间
    static constexpr int PHASE_LOCK_SAMPLES = 10;

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
    // 参数 outShortWire: 输出具体的短路引脚 (0=GND, 1-8=芯号)
    TestResult testSinglePair(uint8_t txPin, uint8_t expectedRxPin, float& outLength, int pairIndex, uint32_t& outCycles, uint8_t& outShortWire);
    
    // 重置所有测试引脚为下拉输入（安全且防止悬空干扰）
    void resetAllPins();
    
    // 将电容充电周期数转换为长度
    float cyclesToMeters(uint32_t cycles, int pairIndex);

    // 全矩阵扫盲探测短路网
    void detectFullWiremap(uint8_t shortNets[8]);

    // 校准参数
    uint32_t baseCycles[4];
    uint32_t cyclesPerMeter[4];
    
    // 所有的网线测试引脚
    static const uint8_t PINS[8];
};

#endif // CABLE_TESTER_H
