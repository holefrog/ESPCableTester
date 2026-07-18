#include "CableTester.h"
#include <Arduino.h>
#include "driver/gpio.h"

// 引脚分配（依据 ESP32 DevKit V1 安全方案）
const uint8_t CableTester::PINS[8] = {13, 14, 25, 26, 27, 32, 33, 23};

CableTester::CableTester() {
  for (int i = 0; i < 4; i++) {
    baseCycles[i] = DEFAULT_BASE_CYCLES;
    cyclesPerMeter[i] = DEFAULT_CYCLES_PER_M;
  }
}

void CableTester::setCalibrationData(const uint32_t base[4],
                                     const uint32_t perMeter[4]) {
  for (int i = 0; i < 4; i++) {
    baseCycles[i] = base[i];
    cyclesPerMeter[i] = perMeter[i] > 0 ? perMeter[i] : 1;
  }
}

float CableTester::cyclesToMeters(uint32_t cycles, int pairIndex) {
  if (cycles >= TIMEOUT_CYCLES)
    return 0.0f; // 如果达到了超时阈值，说明是引脚对地短路，此时不应该输出长度

  if (cycles <= baseCycles[pairIndex])
    return 0.0f;
  return (float)(cycles - baseCycles[pairIndex]) /
         (float)cyclesPerMeter[pairIndex];
}

uint32_t IRAM_ATTR CableTester::measureCapacitanceCycles(uint8_t txPin,
                                                         uint8_t rxPin) {
  // 1. 介质极化预处理 (Dielectric Pre-conditioning)
  // 强制使用与测量时【完全相同】的电压极性！
  // 测量时 txPin 是 LOW，rxPin 被拉高到 HIGH。
  // 所以预热时必须也是 txPin LOW, rxPin HIGH，防止 PVC
  // 介质偶极子反复翻转导致电吸收延迟（Soakage）！
  pinMode(txPin, OUTPUT);
  digitalWrite(txPin, LOW);
  pinMode(rxPin, OUTPUT);
  digitalWrite(rxPin, HIGH);
  delayMicroseconds(2000); // 使用精确微秒级延时，防止 FreeRTOS 调度引起时序抖动

  // 2. 强制彻底放电
  digitalWrite(txPin, LOW);
  digitalWrite(rxPin, LOW); // 刚才就是漏了这句，导致根本没放电！
  delayMicroseconds(5000);  // 确保完全放电到 0V，时序严格固定

  // 提前将引脚转为高阻态输入，此时因为没有上拉，电容依然保持 0V
  pinMode(rxPin, INPUT);

  // 2. 准备高频测量
  portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
  portENTER_CRITICAL(&mux); // 禁用 RTOS 中断，确保周期计数不被打断

  // 瞬间开启内部上拉电阻，电容开始充电！
  // 极其关键的防抖优化：gpio_pullup_en 内部有函数调用，且 RTC
  // 引脚和普通引脚执行时间相差数百周期，甚至触发 Cache Miss。
  // 必须把它放在记录起点之前，才能将这几百个周期的执行误差彻底排除在计数之外！
  gpio_pullup_en((gpio_num_t)rxPin);

  // 记录起点（严格在此处打点！）
  uint32_t start = ESP.getCycleCount();

  uint32_t max_cycles = MAX_POLL_CYCLES; // timeout @ 240MHz

  // 轮询等待直到引脚变为高电平（越过内部逻辑阈值）
  // 【极其关键】由于 digitalRead 是放在 Flash 里的库函数，
  // 当后台 RTOS 任务偶尔触发 Flash Cache 刷新时，digitalRead 会产生几十个周期的
  // Cache Miss 延迟！ 这就是导致 Pair 3/4 偶尔剧烈跳动 60-80 周期的罪魁祸首。
  // 解决方案：彻底绕过 digitalRead，直接读取 ESP32 的硬件 GPIO
  // 寄存器，实现真正的零抖动 1 周期轮询！
  uint32_t pin_mask = (1 << (rxPin & 31));
  if (rxPin < 32) {
    while (((*(volatile uint32_t *)(GPIO_IN_REG)) & pin_mask) == 0) {
      if (ESP.getCycleCount() - start > max_cycles)
        break;
    }
  } else {
    while (((*(volatile uint32_t *)(GPIO_IN1_REG)) & pin_mask) == 0) {
      if (ESP.getCycleCount() - start > max_cycles)
        break;
    }
  }
  uint32_t end = ESP.getCycleCount();

  portEXIT_CRITICAL(&mux); // 恢复 RTOS 中断

  // 3. 恢复安全状态
  pinMode(txPin, INPUT_PULLDOWN);
  pinMode(rxPin, INPUT_PULLDOWN);

  return end - start;
}

void CableTester::init() { resetAllPins(); }

void CableTester::resetAllPins() {
  // 为什么要这么做：每次测试前后，确保所有引脚处于带下拉的输入状态。
  // 业务目的：避免引脚悬空造成电平波动（漂移）导致误判；同时防止意外短路大电流烧坏
  // GPIO。
  for (int i = 0; i < 8; i++) {
    pinMode(PINS[i], INPUT_PULLDOWN);
  }
}

TestResult CableTester::testSinglePair(uint8_t txPin, uint8_t expectedRxPin,
                                       float &outLength, int pairIndex,
                                       uint32_t &outCycles,
                                       uint8_t &outShortWire) {
  outLength = 0.0f;
  outCycles = 0;
  outShortWire = 255;

  // 0. 检查接收引脚是否被意外对地短路 (这会使得电容永远无法充电导致超时)
  // 关键修复：为了防止交叉短路时其他引脚的下拉电阻产生分压干扰，暂时将所有引脚设为高阻态(INPUT)
  for (int i = 0; i < 8; i++) {
    pinMode(PINS[i], INPUT);
  }
  pinMode(expectedRxPin, INPUT_PULLUP);
  delay(2);
  bool isRxShortedToGnd = (digitalRead(expectedRxPin) == LOW);

  // 测试完立刻恢复所有引脚为下拉输入
  for (int i = 0; i < 8; i++) {
    pinMode(PINS[i], INPUT_PULLDOWN);
  }

  if (isRxShortedToGnd) {
    printf("[DEBUG] rxPin=%d is shorted to GROUND!\n", expectedRxPin);
    outShortWire = 0; // 0 代表对地短路
    return TestResult::SHORT_OR_CROSS;
  }

  // 1. 检查 txPin 是否短路到这 8 根线中的任何一根
  pinMode(txPin, OUTPUT);
  digitalWrite(txPin, HIGH);
  delay(2);
  bool hasShort = false;
  for (int i = 0; i < 8; i++) {
    if (PINS[i] == txPin)
      continue;
    if (digitalRead(PINS[i]) == HIGH) {
      hasShort = true;
      outShortWire = i + 1; // 1-8 代表具体的物理引脚
      break;
    }
  }
  digitalWrite(txPin, LOW);
  pinMode(txPin, INPUT_PULLDOWN);

  // 2. 如果 txPin 没发现短路，继续检查 rxPin 是否短路到这 8 根线中的任何一根
  if (!hasShort) {
    pinMode(expectedRxPin, OUTPUT);
    digitalWrite(expectedRxPin, HIGH);
    delay(2);
    for (int i = 0; i < 8; i++) {
      if (PINS[i] == expectedRxPin)
        continue;
      if (digitalRead(PINS[i]) == HIGH) {
        hasShort = true;
        outShortWire = i + 1;
        break;
      }
    }
    digitalWrite(expectedRxPin, LOW);
    pinMode(expectedRxPin, INPUT_PULLDOWN);
  }

  // 3. 根据读取结果判定该线对的最终状态
  if (hasShort) {
    // 高级 TDR 故障定位尝试：将所有非测试引脚设为悬空（高阻态）
    // 这样如果 rxPin 短路到了其它开路的线上，我们可以测量它们并联的寄生电容
    for (int i = 0; i < 8; i++) {
      pinMode(PINS[i], INPUT);
    }
    
    uint32_t totalCycles = 0;
    for (int i = 0; i < PHASE_LOCK_SAMPLES; i++) {
      uint32_t t0 = micros();
      totalCycles += measureCapacitanceCycles(txPin, expectedRxPin);
      while (micros() - t0 < PHASE_LOCK_INTERVAL_US) {}
    }
    uint32_t avgCycles = totalCycles / PHASE_LOCK_SAMPLES;
    
    // 恢复下拉，防止影响后续测试
    for (int i = 0; i < 8; i++) {
      pinMode(PINS[i], INPUT_PULLDOWN);
    }
    
    if (avgCycles < TIMEOUT_CYCLES) {
      outCycles = avgCycles;
      // 经验法则：短接导致两根线的电容并联，因此测出来的长度翻倍。
      // 除以 2 往往能得到非常接近实际故障点（短接点）的物理距离！
      outLength = cyclesToMeters(avgCycles, pairIndex) / 2.0f;
    }
    
    return TestResult::SHORT_OR_CROSS;
  }

  // 如果没短路也没通，说明是正常的悬空开路状态，利用寄生电容估算长度
  // 实施【锁相积分采样 (Phase-Locked Sampling)】：
  // 强制每次采样耗时精确为 10,000 微秒 (10ms)。
  // 连续采样 10 次，总耗时精确等于 100.00 毫秒。
  // 100ms 是 50Hz (20ms周期) 的完美 5 倍，也是 60Hz (16.66ms周期) 的完美 6 倍！
  // 这种数学积分能 100% 抵消空间中市电辐射造成的模拟底噪和量化抖动！
  uint32_t totalCycles = 0;
  for (int i = 0; i < PHASE_LOCK_SAMPLES; i++) {
    uint32_t t0 = micros();
    totalCycles += measureCapacitanceCycles(txPin, expectedRxPin);

    // 锁相循环：死等，直到本次采样消耗的时间精确达到 10,000 微秒
    while (micros() - t0 < PHASE_LOCK_INTERVAL_US) {
      // busy wait
    }
  }
  uint32_t avgCycles = totalCycles / PHASE_LOCK_SAMPLES;
  outCycles = avgCycles;

  // 如果出现了超时，说明是对地严重漏电或未知死短路
  if (outCycles >= TIMEOUT_CYCLES) {
    outShortWire = 0; // 0 代表对地或未知死短路
    return TestResult::SHORT_OR_CROSS;
  }

  outLength = cyclesToMeters(avgCycles, pairIndex);

  // 调试信息：打印原始周期数，方便排查校准问题
  printf("[DEBUG] Pair(%d,%d): AvgCycles=%lu, Base=%lu, PerM=%lu, Len=%.2f\n",
         txPin, expectedRxPin, (unsigned long)avgCycles,
         (unsigned long)baseCycles[pairIndex],
         (unsigned long)cyclesPerMeter[pairIndex], outLength);

  return TestResult::OPEN;
}

CableStatus CableTester::runTest() {
  CableStatus status;
  status.len1 = 0;
  status.len2 = 0;
  status.len3 = 0;
  status.len4 = 0;
  status.cycles1 = 0;
  status.cycles2 = 0;
  status.cycles3 = 0;
  status.cycles4 = 0;
  status.shortWire1 = 255;
  status.shortWire2 = 255;
  status.shortWire3 = 255;
  status.shortWire4 = 255;

  // 1. 全局短路网络扫描
  detectFullWiremap(status.shortNets);

  // 2. 识别合法的 "PASS" (即正常的远端线对物理环回)
  // 条件：某对线的 TX 和 RX 位于同一个短路网中，并且这个网里没有第三者
  auto isPairPass = [&](int txIdx, int rxIdx) {
    if (status.shortNets[txIdx] == 0)
      return false;
    if (status.shortNets[txIdx] != status.shortNets[rxIdx])
      return false;

    int count = 0;
    for (int i = 0; i < 8; i++) {
      if (status.shortNets[i] == status.shortNets[txIdx])
        count++;
    }
    return count == 2;
  };

  bool passPair[4];
  passPair[0] = isPairPass(0, 1); // 1-2
  passPair[1] = isPairPass(2, 5); // 3-6 (PINS[2]=25, PINS[5]=32)
  passPair[2] = isPairPass(3, 4); // 4-5 (PINS[3]=26, PINS[4]=27)
  passPair[3] = isPairPass(6, 7); // 7-8

  printf("[DEBUG] shortNets: %d %d %d %d %d %d %d %d | passPair: %d %d %d %d\n",
         status.shortNets[0], status.shortNets[1], status.shortNets[2],
         status.shortNets[3], status.shortNets[4], status.shortNets[5],
         status.shortNets[6], status.shortNets[7], passPair[0], passPair[1],
         passPair[2], passPair[3]);

  // 3. 判断是否需要全屏绘制 Graphical Wiremap (存在非法的短路)
  // 我们不再清空 shortNets，保留它以便全屏画图时能完整展示所有连通关系
  status.hasFault = false;
  for (int i = 0; i < 8; i++) {
    if (status.shortNets[i] != 0) {
      bool inPassPair = false;
      for (int p = 0; p < 4; p++) {
        if (passPair[p]) {
          int txIdx = 0, rxIdx = 0;
          if (p == 0) {
            txIdx = 0;
            rxIdx = 1;
          } else if (p == 1) {
            txIdx = 2;
            rxIdx = 5;
          } else if (p == 2) {
            txIdx = 3;
            rxIdx = 4;
          } else {
            txIdx = 6;
            rxIdx = 7;
          }

          if (i == txIdx || i == rxIdx) {
            inPassPair = true;
            break;
          }
        }
      }
      if (!inPassPair) {
        status.hasFault = true;
        break;
      }
    }
  }

  // 4. 逐个线对进行最终判定和测长
  auto testOrSkip = [&](uint8_t txPin, uint8_t rxPin, float &outLen,
                        int pairIdx, uint32_t &outCycles, uint8_t &outShort) {
    // 如果是合法的远端环回，直接返回 PASS
    if (passPair[pairIdx]) {
      outShort = 255;
      outLen = 0.0f;
      outCycles = 0;
      return TestResult::PASS;
    }

    int txIdx = 0, rxIdx = 0;
    if (pairIdx == 0) {
      txIdx = 0;
      rxIdx = 1;
    } else if (pairIdx == 1) {
      txIdx = 2;
      rxIdx = 5;
    } else if (pairIdx == 2) {
      txIdx = 3;
      rxIdx = 4;
    } else {
      txIdx = 6;
      rxIdx = 7;
    }

    // 如果引脚参与了非法的短路网络（被别人短接了），或者就是个错误的短路，直接跳过测长以防超时
    if (status.shortNets[txIdx] != 0 || status.shortNets[rxIdx] != 0) {
      outShort = 255;
      return TestResult::SHORT_OR_CROSS;
    }

    return testSinglePair(txPin, rxPin, outLen, pairIdx, outCycles, outShort);
  };

  status.pair1 = testOrSkip(13, 14, status.len1, 0, status.cycles1,
                            status.shortWire1); // Pair 1: 1-2 (橙色对)
  status.pair2 = testOrSkip(25, 32, status.len2, 1, status.cycles2,
                            status.shortWire2); // Pair 2: 3-6 (绿色对)
  status.pair3 = testOrSkip(26, 27, status.len3, 2, status.cycles3,
                            status.shortWire3); // Pair 3: 4-5 (蓝色对)
  status.pair4 = testOrSkip(33, 23, status.len4, 3, status.cycles4,
                            status.shortWire4); // Pair 4: 7-8 (棕色对)

  return status;
}

void CableTester::detectFullWiremap(uint8_t shortNets[8]) {
  // 强制所有引脚进入下拉输入状态，防止前一次测长后留下 Output 或悬空浮高
  for (int i = 0; i < 8; i++) {
    pinMode(PINS[i], INPUT_PULLDOWN);
  }

  for (int i = 0; i < 8; i++) {
    shortNets[i] = 0;
  }

  resetAllPins();

  uint8_t currentNetId = 1;

  // 采用全遍历：依次将每个引脚拉高，检查其余引脚
  for (int i = 0; i < 8; i++) {
    pinMode(PINS[i], OUTPUT);
    digitalWrite(PINS[i], HIGH);
    delay(2);

    for (int j = i + 1; j < 8; j++) {
      if (digitalRead(PINS[j]) == HIGH) {
        // 发现物理连通
        if (shortNets[i] == 0 && shortNets[j] == 0) {
          shortNets[i] = currentNetId;
          shortNets[j] = currentNetId;
          currentNetId++;
        } else if (shortNets[i] != 0 && shortNets[j] == 0) {
          shortNets[j] = shortNets[i];
        } else if (shortNets[i] == 0 && shortNets[j] != 0) {
          shortNets[i] = shortNets[j];
        } else if (shortNets[i] != shortNets[j]) {
          // 合并两个连通网
          uint8_t targetNet = shortNets[i];
          uint8_t oldNet = shortNets[j];
          for (int k = 0; k < 8; k++) {
            if (shortNets[k] == oldNet) {
              shortNets[k] = targetNet;
            }
          }
        }
      }
    }

    digitalWrite(PINS[i], LOW);
    pinMode(PINS[i], INPUT_PULLDOWN);
  }
}

// 辅助函数：比对前后两次状态是否发生实质性变化（包括长度变化 > 0.2m）
bool CableTester::isStatusEqual(const CableStatus &a, const CableStatus &b) {
  const float LENGTH_CHANGE_THRESHOLD_M = 0.2f;
  if (a.pair1 != b.pair1 || a.pair2 != b.pair2 || a.pair3 != b.pair3 ||
      a.pair4 != b.pair4)
    return false;
  if (a.shortWire1 != b.shortWire1 || a.shortWire2 != b.shortWire2 ||
      a.shortWire3 != b.shortWire3 || a.shortWire4 != b.shortWire4)
    return false;

  auto checkLen = [&](TestResult res, float l1, float l2) {
    if (res == TestResult::OPEN) {
      if (abs(l1 - l2) > LENGTH_CHANGE_THRESHOLD_M)
        return false;
    }
    return true;
  };

  if (!checkLen(a.pair1, a.len1, b.len1)) return false;
  if (!checkLen(a.pair2, a.len2, b.len2)) return false;
  if (!checkLen(a.pair3, a.len3, b.len3)) return false;
  if (!checkLen(a.pair4, a.len4, b.len4)) return false;

  if (a.hasFault != b.hasFault)
    return false;
  for (int i = 0; i < 8; i++) {
    if (a.shortNets[i] != b.shortNets[i])
      return false;
  }
  return true;
}

// 辅助函数：判断是否空载（所有线都是 OPEN，且长度都接近 0）
bool CableTester::isAllOpen(const CableStatus &status) {
  return (status.pair1 == TestResult::OPEN &&
          status.pair2 == TestResult::OPEN &&
          status.pair3 == TestResult::OPEN && status.pair4 == TestResult::OPEN);
}

// 辅助函数：判断是否真的没插线（全断且长度小于 0.5m）
bool CableTester::isNoCable(const CableStatus &status) {
  const float NO_CABLE_LENGTH_THRESHOLD_M = 0.5f;
  if (!isAllOpen(status))
    return false;
  float maxL = 0;
  if (status.len1 > maxL) maxL = status.len1;
  if (status.len2 > maxL) maxL = status.len2;
  if (status.len3 > maxL) maxL = status.len3;
  if (status.len4 > maxL) maxL = status.len4;
  return (maxL < NO_CABLE_LENGTH_THRESHOLD_M);
}

// 辅助函数：判断该状态是否包含有效的“实际长度”
bool CableTester::hasActualLength(const CableStatus &status) {
  const float NO_CABLE_LENGTH_THRESHOLD_M = 0.5f;
  float maxL = 0;
  if (status.pair1 == TestResult::OPEN && status.len1 > maxL) maxL = status.len1;
  if (status.pair2 == TestResult::OPEN && status.len2 > maxL) maxL = status.len2;
  if (status.pair3 == TestResult::OPEN && status.len3 > maxL) maxL = status.len3;
  if (status.pair4 == TestResult::OPEN && status.len4 > maxL) maxL = status.len4;
  return (maxL >= NO_CABLE_LENGTH_THRESHOLD_M);
}

void CableTester::runCalibrationSample(uint32_t results[4]) {
  const int CALIB_PREWARM_LOOPS = 10;
  const uint32_t CALIB_PREWARM_DELAY_MS = 50;
  const int CALIB_SAMPLES = 16;
  const uint32_t CALIB_SAMPLE_DELAY_MS = 50;

  for (int i = 0; i < CALIB_PREWARM_LOOPS; i++) {
    runTest();
    delay(CALIB_PREWARM_DELAY_MS);
  }

  for (int i = 0; i < 4; i++) results[i] = 0;
  for (int i = 0; i < CALIB_SAMPLES; i++) {
    CableStatus s = runTest();
    results[0] += s.cycles1;
    results[1] += s.cycles2;
    results[2] += s.cycles3;
    results[3] += s.cycles4;
    delay(CALIB_SAMPLE_DELAY_MS); 
  }
  for (int i = 0; i < 4; i++) results[i] /= CALIB_SAMPLES;
}
