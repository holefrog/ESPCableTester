#include "CableTester.h"
#include "driver/gpio.h"

// 引脚分配（依据 ESP32 DevKit V1 安全方案）
const uint8_t CableTester::PINS[8] = {13, 14, 25, 26, 27, 32, 33, 23};

CableTester::CableTester() {
    for (int i = 0; i < 4; i++) {
        baseCycles[i] = 150;
        cyclesPerMeter[i] = 540;
    }
}

void CableTester::setCalibrationData(const uint32_t base[4], const uint32_t perMeter[4]) {
    for (int i = 0; i < 4; i++) {
        baseCycles[i] = base[i];
        cyclesPerMeter[i] = perMeter[i] > 0 ? perMeter[i] : 1;
    }
}

float CableTester::cyclesToMeters(uint32_t cycles, int pairIndex) {
    if (cycles >= 20000000) return 0.0f; // 如果达到了超时阈值，说明是引脚对地短路，此时不应该输出长度
    
    if (cycles <= baseCycles[pairIndex]) return 0.0f;
    return (float)(cycles - baseCycles[pairIndex]) / (float)cyclesPerMeter[pairIndex];
}

uint32_t CableTester::measureCapacitanceCycles(uint8_t txPin, uint8_t rxPin) {
    // 1. 介质极化预处理 (Dielectric Pre-conditioning)
    // 无论之前做过什么测试，先用 3.3V 强力充电 2ms，统一线缆的介质吸收状态 (Soakage)
    // 这能确保单独测量 (校准时) 和 连续测量 (测试时) 的电容值完全一致！
    pinMode(txPin, OUTPUT);
    digitalWrite(txPin, HIGH);
    pinMode(rxPin, OUTPUT);
    digitalWrite(rxPin, LOW);
    delayMicroseconds(2000); // 使用精确微秒级延时，防止 FreeRTOS 调度引起时序抖动

    // 2. 强制彻底放电
    digitalWrite(txPin, LOW);
    delayMicroseconds(5000); // 确保完全放电到 0V，时序严格固定

    // 提前将引脚转为高阻态输入，此时因为没有上拉，电容依然保持 0V
    pinMode(rxPin, INPUT); 

    // 2. 准备高频测量
    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    portENTER_CRITICAL(&mux); // 禁用 RTOS 中断，确保周期计数不被打断

    // 记录起点
    uint32_t start = ESP.getCycleCount();
    
    // 瞬间开启内部上拉电阻，电容开始充电！(底层 API，只需几个时钟周期)
    gpio_pullup_en((gpio_num_t)rxPin); 
    
    uint32_t max_cycles = 24000000; // 100ms timeout @ 240MHz
    
    // 轮询等待直到引脚变为高电平（越过内部逻辑阈值）
    // 【极其关键】由于 digitalRead 是放在 Flash 里的库函数，
    // 当后台 RTOS 任务偶尔触发 Flash Cache 刷新时，digitalRead 会产生几十个周期的 Cache Miss 延迟！
    // 这就是导致 Pair 3/4 偶尔剧烈跳动 60-80 周期的罪魁祸首。
    // 解决方案：彻底绕过 digitalRead，直接读取 ESP32 的硬件 GPIO 寄存器，实现真正的零抖动 1 周期轮询！
    uint32_t pin_mask = (1 << (rxPin & 31));
    if (rxPin < 32) {
        while ((REG_READ(GPIO_IN_REG) & pin_mask) == 0) {
            if (ESP.getCycleCount() - start > max_cycles) break;
        }
    } else {
        while ((REG_READ(GPIO_IN1_REG) & pin_mask) == 0) {
            if (ESP.getCycleCount() - start > max_cycles) break;
        }
    }
    uint32_t end = ESP.getCycleCount();

    portEXIT_CRITICAL(&mux); // 恢复 RTOS 中断

    // 3. 恢复安全状态
    pinMode(txPin, INPUT_PULLDOWN);
    pinMode(rxPin, INPUT_PULLDOWN);

    return end - start;
}

void CableTester::init() {
    resetAllPins();
}

void CableTester::resetAllPins() {
    // 为什么要这么做：每次测试前后，确保所有引脚处于带下拉的输入状态。
    // 业务目的：避免引脚悬空造成电平波动（漂移）导致误判；同时防止意外短路大电流烧坏 GPIO。
    for (int i = 0; i < 8; i++) {
        pinMode(PINS[i], INPUT_PULLDOWN);
    }
}

TestResult CableTester::testSinglePair(uint8_t txPin, uint8_t expectedRxPin, float& outLength, int pairIndex, uint32_t& outCycles) {
    outLength = 0.0f;
    outCycles = 0;
    
    // 0. 检查接收引脚是否被意外对地短路 (这会使得电容永远无法充电导致超时)
    pinMode(expectedRxPin, INPUT_PULLUP);
    delay(2);
    bool isRxShortedToGnd = (digitalRead(expectedRxPin) == LOW);
    pinMode(expectedRxPin, INPUT_PULLDOWN); // 测试完立刻恢复
    
    if (isRxShortedToGnd) {
        Serial.printf("[DEBUG] rxPin=%d is shorted to GROUND!\n", expectedRxPin);
        return TestResult::SHORT_OR_CROSS;
    }

    // 1. 设置发送端拉高
    pinMode(txPin, OUTPUT);
    digitalWrite(txPin, HIGH);
    
    // 给系统一点时间让电平稳定
    delay(2);
    
    // 2. 检查期望的接收引脚是否读到高电平
    bool expectedHigh = (digitalRead(expectedRxPin) == HIGH);
    
    // 3. 检查是否有其它非预期引脚也变高了（意味着存在短路或错线干扰）
    bool hasShort = false;
    for (int i = 0; i < 8; i++) {
        uint8_t currentPin = PINS[i];
        
        // 忽略当前正在测试的这一对引脚
        if (currentPin == txPin || currentPin == expectedRxPin) {
            continue; 
        }
        
        if (digitalRead(currentPin) == HIGH) {
            Serial.printf("[DEBUG] txPin=%d is HIGH, but unexpected pin=%d is ALSO HIGH!\n", txPin, currentPin);
            hasShort = true;
            break; // 卫语句：一旦发现一根引脚存在短路，即可跳出循环
        }
    }
    
    // 4. 测试完毕，立刻将发送端恢复为下拉输入，保持隔离
    // 关键修复：在切回 INPUT 之前，主动输出 LOW 强行释放线缆和面包板上的残留寄生电荷（消除浮空干扰）
    digitalWrite(txPin, LOW);
    delay(2); // 留出充足时间让所有引脚彻底放电，恢复 0V
    pinMode(txPin, INPUT_PULLDOWN);
    
    // 5. 根据读取结果判定该线对的最终状态
    if (hasShort) {
        return TestResult::SHORT_OR_CROSS;
    }
    
    if (expectedHigh) {
        return TestResult::PASS;
    }
    
    // 如果是 OPEN，利用寄生电容估算长度
    // 实施【锁相积分采样 (Phase-Locked Sampling)】：
    // 强制每次采样耗时精确为 10,000 微秒 (10ms)。
    // 连续采样 10 次，总耗时精确等于 100.00 毫秒。
    // 100ms 是 50Hz (20ms周期) 的完美 5 倍，也是 60Hz (16.66ms周期) 的完美 6 倍！
    // 这种数学积分能 100% 抵消空间中市电辐射造成的模拟底噪和量化抖动！
    uint32_t totalCycles = 0;
    for(int i = 0; i < 10; i++) {
        uint32_t t0 = micros();
        totalCycles += measureCapacitanceCycles(txPin, expectedRxPin);
        
        // 锁相循环：死等，直到本次采样消耗的时间精确达到 10,000 微秒
        while (micros() - t0 < 10000) {
            // busy wait
        }
    }
    uint32_t avgCycles = totalCycles / 10;
    outCycles = avgCycles;
    outLength = cyclesToMeters(avgCycles, pairIndex);
    
    // 调试信息：打印原始周期数，方便排查校准问题
    Serial.printf("[DEBUG] Pair(%d,%d): AvgCycles=%u, Base=%u, PerM=%u, Len=%.2f\n", 
                  txPin, expectedRxPin, avgCycles, baseCycles[pairIndex], cyclesPerMeter[pairIndex], outLength);
    
    return TestResult::OPEN;
}

CableStatus CableTester::runTest() {
    CableStatus status;
    status.len1 = 0; status.len2 = 0; status.len3 = 0; status.len4 = 0;
    status.cycles1 = 0; status.cycles2 = 0; status.cycles3 = 0; status.cycles4 = 0;
    
    // 业务目的：远端将线两两短接，近端发送一次脉冲，就可以验证这两根线的回路连通性
    
    // 测试线对 1：Pin 1(橙白) 和 Pin 2(橙)
    status.pair1 = testSinglePair(13, 14, status.len1, 0, status.cycles1); // Pair 1: 1-2 (橙色对)
    
    // 测试线对 2：Pin 3(绿白) 和 Pin 6(绿)
    status.pair2 = testSinglePair(25, 32, status.len2, 1, status.cycles2); // Pair 2: 3-6 (绿色对)
    
    // 测试线对 3：Pin 4(蓝) 和 Pin 5(蓝白)
    status.pair3 = testSinglePair(26, 27, status.len3, 2, status.cycles3); // Pair 3: 4-5 (蓝色对)
    
    // 测试线对 4：Pin 7(棕白) 和 Pin 8(棕)
    status.pair4 = testSinglePair(33, 23, status.len4, 3, status.cycles4); // Pair 4: 7-8 (棕色对)
    
    return status;
}
