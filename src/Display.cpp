#include "Display.h"

// 构造函数，U8G2_R0 表示不旋转屏幕
Display::Display() : u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE) {}

void Display::init() {
    // 为什么要这么做：强制指定 ESP32 的硬件 I2C 引脚
    // 业务目的：确保库底层的 Wire 使用我们配置的 D21 和 D22
    Wire.begin(21, 22);
    
    // 初始化屏幕对象并清空屏幕
    u8g2.begin();
    u8g2.clearBuffer();
}

const char* Display::statusToStr(TestResult res) {
    // 业务目的：为了在空间有限的 OLED 上整齐显示，将枚举转为简短的英文状态词
    switch(res) {
        case TestResult::PASS: return "PASS";
        case TestResult::OPEN: return "OPEN";
        case TestResult::SHORT_OR_CROSS: return "SHORT";
        default: return "---";
    }
}

void Display::renderReady() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_ncenB10_tr); // 使用较大字体显示标题
    u8g2.drawStr(5, 30, "ESP32 Tester");
    
    u8g2.setFont(u8g2_font_ncenB08_tr); // 使用小字体显示提示
    u8g2.drawStr(5, 50, "Ready. Plug cable...");
    u8g2.sendBuffer();
}

void Display::renderMeasuring() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_ncenB10_tr);
    u8g2.drawStr(5, 30, "Calibration");
    
    u8g2.setFont(u8g2_font_ncenB08_tr);
    u8g2.drawStr(5, 50, "Measuring...");
    u8g2.sendBuffer();
}

void Display::renderResult(const CableStatus& status, bool useFeet, bool isCalibrated) {
    u8g2.clearBuffer();
    
    // ==========================================
    // 1. 黄色区域 (顶端 16 像素，y: 0~15)
    // ==========================================
    u8g2.setFont(u8g2_font_ncenB08_tr); // 高度约 8 像素
    
    bool allOpen = (status.pair1 == TestResult::OPEN && 
                    status.pair2 == TestResult::OPEN && 
                    status.pair3 == TestResult::OPEN && 
                    status.pair4 == TestResult::OPEN);
                    
    char topBuf[32];
    if (allOpen) {
        if (!isCalibrated) {
            snprintf(topBuf, sizeof(topBuf), "Uncalibrated!");
        } else {
            // 计算 4 对线的平均长度，比单根线更稳定
            float avgLen = (status.len1 + status.len2 + status.len3 + status.len4) / 4.0f;
            
            if (avgLen >= 0.0f) {
                float displayLen = useFeet ? (avgLen * 3.28084f) : avgLen;
                const char* unit = useFeet ? "ft" : "m";
                snprintf(topBuf, sizeof(topBuf), "Length: %.1f%s", displayLen, unit);
            } else {
                snprintf(topBuf, sizeof(topBuf), "No Cable");
            }
        }
    } else {
        // 如果测通了(PASS)或者存在短路(SHORT)，说明线路上有电气闭环，无法测算物理电容
        snprintf(topBuf, sizeof(topBuf), "Loopback Mode");
    }
    
    // 黄区居中打印
    int w = u8g2.getStrWidth(topBuf);
    u8g2.drawStr((128 - w) / 2, 12, topBuf);
    
    // 在黄蓝交界处画一条分割线 (y=15是黄区的底边缘)
    u8g2.drawLine(0, 15, 127, 15);
    
    // 2. 蓝色区域 (底端 48 像素，y: 16~63)
    // ==========================================
    drawGraphicalWiremap(16, status, useFeet);
    
    u8g2.sendBuffer(); // 将内存缓冲一次性推送到 OLED 进行物理刷新
}

void Display::drawGraphicalWiremap(int yOffset, const CableStatus& status, bool useFeet) {
    if (status.hasFault) {
        // --- DRAW COMPLEX GRAPHICAL SHORT MAP ---
        // 采用极小字体（5像素宽，7像素高）
        u8g2.setFont(u8g2_font_5x7_tr);
        int startY = yOffset + 5; // 如果 yOffset=16，则第一根线 y=21
        int rowHeight = 6;
        
        for (int i = 0; i < 8; i++) {
            int y = startY + i * rowHeight;
            char buf[16];
            snprintf(buf, sizeof(buf), "Pin %d", i + 1);
            u8g2.drawStr(0, y, buf);
            
            // 引脚向右伸出的小横线
            int lineY = y - 3;
            u8g2.drawLine(25, lineY, 32, lineY);
            
            // 如果这个引脚悬空（没有短路），画一个小的未连接标记 'x'
            if (status.shortNets[i] == 0) {
                u8g2.drawStr(34, y, "x");
            }
        }
        
        // 绘制垂直的短路连线括号 (Brackets)
        int netX = 38; // 垂直线起始 X 坐标
        for (int net = 1; net <= 8; net++) {
            int firstPin = -1;
            int lastPin = -1;
            for (int i = 0; i < 8; i++) {
                if (status.shortNets[i] == net) {
                    if (firstPin == -1) firstPin = i;
                    lastPin = i;
                }
            }
            if (firstPin != -1 && firstPin != lastPin) {
                int y1 = startY + firstPin * rowHeight - 3;
                int y2 = startY + lastPin * rowHeight - 3;
                
                // 画垂直主干线
                u8g2.drawLine(netX, y1, netX, y2);
                
                // 画每一个引脚连接到主干线的水平线
                for (int i = 0; i < 8; i++) {
                    if (status.shortNets[i] == net) {
                        int y = startY + i * rowHeight - 3;
                        u8g2.drawLine(32, y, netX, y);
                        // 在交叉点画个小实心矩形强调节点
                        u8g2.drawBox(netX - 1, y - 1, 3, 3);
                    }
                }
                netX += 8; // 下一个连通网的垂直线往右挪 8 个像素，防止重叠
            }
        }
        // 智能交叉分析 (Smart Crossover Analysis)
        int pairOfPin[8] = {1, 1, 2, 3, 3, 2, 4, 4}; // Pin index to Pair number
        int crossPairs[2] = {0, 0};
        int crossPins[2][2];
        int crossCount = 0;
        
        for (int net = 1; net <= 8; net++) {
            int pinsInNet[8];
            int count = 0;
            for(int i = 0; i < 8; i++) {
                if(status.shortNets[i] == net) {
                    pinsInNet[count++] = i;
                }
            }
            if (count == 2) {
                int pA = pinsInNet[0];
                int pB = pinsInNet[1];
                if (pairOfPin[pA] != pairOfPin[pB]) {
                    if (crossCount < 2) {
                        crossPins[crossCount][0] = pA;
                        crossPins[crossCount][1] = pB;
                        crossPairs[crossCount] = (1 << pairOfPin[pA]) | (1 << pairOfPin[pB]);
                        crossCount++;
                    }
                }
            }
        }
        
        if (crossCount == 2 && crossPairs[0] == crossPairs[1]) {
            // We found exactly two cross-pair shorts between the SAME two pairs!
            int pA = crossPins[0][0];
            int pB = crossPins[0][1];
            int pC = crossPins[1][0];
            int pD = crossPins[1][1];
            
            // Ensure pA and pC are in the same pair
            if (pairOfPin[pA] != pairOfPin[pC]) {
                int temp = pC; pC = pD; pD = temp;
            }
            
            int pinA = pA + 1; int pinB = pB + 1;
            int pinC = pC + 1; int pinD = pD + 1;
            
            u8g2.setFont(u8g2_font_ncenB08_tr);
            u8g2.drawStr(netX + 6, yOffset + 15, "SPLIT PAIR");
            
            char buf[32];
            snprintf(buf, sizeof(buf), "P%d <-> P%d", pairOfPin[pA], pairOfPin[pB]);
            u8g2.drawStr(netX + 6, yOffset + 30, buf);
            
            u8g2.setFont(u8g2_font_5x7_tr);
            snprintf(buf, sizeof(buf), "%d-%d or %d-%d", pinA, pinD, pinC, pinB);
            u8g2.drawStr(netX + 6, yOffset + 45, buf);
        } else {
            // 在右侧空白区域打印常规提示信息
            u8g2.setFont(u8g2_font_ncenB08_tr);
            u8g2.drawStr(netX + 8, yOffset + 20, "MISWIRE");
            u8g2.drawStr(netX + 8, yOffset + 35, "CROSSED");
        }
        
        return;
    }

    // --- DRAW NORMAL PAIR DISPLAY ---
    u8g2.setFont(u8g2_font_ncenB08_tr);
    auto drawPair = [&](int y, const char* name, TestResult res, float len, uint8_t shortWire) {
        u8g2.setCursor(0, y);
        u8g2.print(name); 
        
        int lineX1 = 22;
        int lineX2 = 55;
        int lineY = y - 3;
        
        if (res == TestResult::PASS) {
            u8g2.drawLine(lineX1, lineY, lineX2, lineY);
            // Draw a loop back box
            u8g2.drawFrame(lineX2, lineY - 2, 4, 5);
            u8g2.drawStr(lineX2 + 8, y, "PASS");
        } 
        else if (res == TestResult::OPEN) {
            u8g2.drawLine(lineX1, lineY, lineX1 + 10, lineY);
            u8g2.drawStr(lineX1 + 13, y+1, "x");
            
            char buf[16];
            float displayLen = useFeet ? (len * 3.28084f) : len;
            snprintf(buf, sizeof(buf), "%.1f%s", displayLen, useFeet ? "ft" : "m");
            u8g2.drawStr(lineX2 + 8, y, buf);
        }
        else if (res == TestResult::SHORT_OR_CROSS) {
            u8g2.drawLine(lineX1, lineY, lineX2, lineY);
            // Draw a short to ground symbol
            u8g2.drawLine(lineX1 + 15, lineY, lineX1 + 15, lineY + 4);
            u8g2.drawLine(lineX1 + 12, lineY + 4, lineX1 + 18, lineY + 4);
            
            char buf[16];
            if (shortWire == 0) snprintf(buf, sizeof(buf), "SHT(?)");
            else snprintf(buf, sizeof(buf), "SHT(%d)", shortWire);
            u8g2.drawStr(lineX2 + 8, y, buf);
        }
    };
    
    drawPair(yOffset + 11, "1-2", status.pair1, status.len1, status.shortWire1);
    drawPair(yOffset + 23, "3-6", status.pair2, status.len2, status.shortWire2);
    drawPair(yOffset + 35, "4-5", status.pair3, status.len3, status.shortWire3);
    drawPair(yOffset + 47, "7-8", status.pair4, status.len4, status.shortWire4);
}

void Display::renderHistory(const CableStatus& status, bool useFeet, int index, int total) {
    u8g2.clearBuffer();
    
    u8g2.setFont(u8g2_font_ncenB08_tr);
    char topBuf[32];
    snprintf(topBuf, sizeof(topBuf), "History [%d/%d]", index + 1, total);
    
    int w = u8g2.getStrWidth(topBuf);
    u8g2.drawStr((128 - w) / 2, 12, topBuf);
    
    u8g2.drawLine(0, 15, 127, 15);
    
    drawGraphicalWiremap(16, status, useFeet);
    
    u8g2.sendBuffer();
}

void Display::renderCalibStep1() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_ncenB10_tr);
    u8g2.drawStr(5, 15, "Calibration");
    u8g2.setFont(u8g2_font_ncenB08_tr);
    u8g2.drawStr(0, 35, "1. Insert empty plug");
    u8g2.drawStr(0, 50, "2. Press BOOT btn");
    u8g2.sendBuffer();
}

void Display::renderCalibStep2() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_ncenB10_tr);
    u8g2.drawStr(5, 15, "Calibration");
    u8g2.setFont(u8g2_font_ncenB08_tr);
    char buf[32];
    snprintf(buf, sizeof(buf), "1. Insert %s wire", CableTester::CALIBRATION_CABLE_NAME);
    u8g2.drawStr(0, 35, buf);
    u8g2.drawStr(0, 50, "2. Press BOOT btn");
    u8g2.sendBuffer();
}

void Display::renderCalibDone() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_ncenB14_tr);
    u8g2.drawStr(30, 40, "Saved!");
    u8g2.sendBuffer();
}

void Display::renderCalibFailed() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_ncenB10_tr);
    u8g2.drawStr(10, 25, "FAILED!");
    u8g2.setFont(u8g2_font_ncenB08_tr);
    u8g2.drawStr(0, 45, "No cable detected.");
    u8g2.drawStr(0, 60, "Value <= Base.");
    u8g2.sendBuffer();
}

void Display::renderCalibError(const char* line1, const char* line2) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_ncenB10_tr);
    u8g2.drawStr(5, 25, "ERROR!");
    u8g2.setFont(u8g2_font_ncenB08_tr);
    u8g2.drawStr(0, 45, line1);
    u8g2.drawStr(0, 60, line2);
    u8g2.sendBuffer();
}

void Display::renderUncalibratedWarning(uint32_t timeoutSec) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_ncenB08_tr);
    
    // 居中打印警告标题
    const char* title = "UNCALIBRATED!";
    int w = u8g2.getStrWidth(title);
    u8g2.drawStr((128 - w) / 2, 12, title);
    
    u8g2.drawLine(0, 15, 127, 15);
    
    u8g2.drawStr(0, 30, "Length is disabled.");
    u8g2.drawStr(0, 46, "Hold BOOT to calib.");
    
    char buf[32];
    snprintf(buf, sizeof(buf), "Or wait %us to skip.", timeoutSec);
    u8g2.drawStr(0, 62, buf);
    
    u8g2.sendBuffer();
}

void Display::sleep() {
    u8g2.clearBuffer();
    u8g2.sendBuffer();
    u8g2.setPowerSave(1); // Turn off display to save power
}
