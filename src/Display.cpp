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
    if (status.hasFault) {
        snprintf(topBuf, sizeof(topBuf), "FAULT DETECTED");
    } else if (allOpen) {
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

// 顶层分发：根据是否有错线故障选择不同的绘制模式
void Display::drawGraphicalWiremap(int startY, const CableStatus& status, bool useFeet) {
    if (status.hasFault) {
        drawFaultWiremap(startY, status);
    } else {
        drawNormalWiremap(startY, status, useFeet);
    }
}

// 故障模式：贪心算法推导短路网络 → 绘制交叉走线图
void Display::drawFaultWiremap(int startY, const CableStatus& status) {
    int xPositions[8] = {10, 25, 40, 55, 70, 85, 100, 115};
    int topY = startY + 8;  // start wires at y=24
    int botY = startY + 30; // end wires at y=46

    // 数字行
    u8g2.setFont(u8g2_font_5x7_tr);
    for (int i = 0; i < 8; i++) {
        u8g2.setCursor(xPositions[i] - 2, startY + 6); // top numbers at y=22
        u8g2.print(i + 1);
        u8g2.setCursor(xPositions[i] - 2, startY + 38); // bottom numbers at y=54
        u8g2.print(i + 1);
    }

    // 远端物理短接环回固定标识
    int loopbacks[4][2] = {{0,1}, {2,5}, {3,4}, {6,7}};
    for (int l = 0; l < 4; l++) {
        int x1 = xPositions[loopbacks[l][0]];
        int x2 = xPositions[loopbacks[l][1]];
        int loopY = startY + 40; // loopbacks from y=56 to 59
        u8g2.drawLine(x1, loopY, x1, loopY + 3);
        u8g2.drawLine(x2, loopY, x2, loopY + 3);
        u8g2.drawLine(x1, loopY + 3, x2, loopY + 3);
    }

    // 贪心算法：将测得的连通网映射到远端环回，推导出最合理的真实物理交叉线图
    int farAssigned[8];
    for (int i = 0; i < 8; i++) farAssigned[i] = -1;
    bool loopbackUsed[4] = {false, false, false, false};

    // 第一轮：完美匹配的正常线对
    for (int net = 1; net <= 8; net++) {
        int pinsInNet[8];
        int count = 0;
        for (int i = 0; i < 8; i++) {
            if (status.shortNets[i] == net) pinsInNet[count++] = i;
        }
        if (count == 2) {
            int pA = pinsInNet[0], pB = pinsInNet[1];
            for (int l = 0; l < 4; l++) {
                if (!loopbackUsed[l]) {
                    if ((pA == loopbacks[l][0] && pB == loopbacks[l][1]) ||
                        (pA == loopbacks[l][1] && pB == loopbacks[l][0])) {
                        farAssigned[pA] = pA;
                        farAssigned[pB] = pB;
                        loopbackUsed[l] = true;
                        break;
                    }
                }
            }
        }
    }

    // 第二轮：处理错线、交叉、串扰网络
    for (int net = 1; net <= 8; net++) {
        int pinsInNet[8];
        int count = 0;
        for (int i = 0; i < 8; i++) {
            if (status.shortNets[i] == net && farAssigned[i] == -1) pinsInNet[count++] = i;
        }
        if (count == 0) continue;

        int lbIdx = -1;
        // 优先寻找一个共享引脚的远端环回，这样能让图画出来最直观（减少杂乱的交叉）
        for (int l = 0; l < 4; l++) {
            if (!loopbackUsed[l]) {
                if (lbIdx == -1) lbIdx = l;
                for (int c = 0; c < count; c++) {
                    if (pinsInNet[c] == loopbacks[l][0] || pinsInNet[c] == loopbacks[l][1]) {
                        lbIdx = l; break;
                    }
                }
            }
        }

        if (lbIdx != -1) {
            loopbackUsed[lbIdx] = true;
            farAssigned[pinsInNet[0]] = loopbacks[lbIdx][0];
            if (count > 1) farAssigned[pinsInNet[1]] = loopbacks[lbIdx][1];
            // 超过两根线短路，全部合并到远端的一个引脚上，形成 Y 型合并交叉
            for (int c = 2; c < count; c++) {
                farAssigned[pinsInNet[c]] = loopbacks[lbIdx][0];
            }
        } else {
            // 没有可用的远端环回（理论上只有发生极其严重的乱接才会触发）
            for (int c = 0; c < count; c++) {
                farAssigned[pinsInNet[c]] = pinsInNet[c];
            }
        }
    }

    // 绘制线缆走线
    for (int i = 0; i < 8; i++) {
        if (farAssigned[i] != -1) {
            // 画出漂亮的连线（交叉时会自动呈现 X 型）
            u8g2.drawLine(xPositions[i], topY, xPositions[farAssigned[i]], botY);
        } else {
            // 开路状态：画一条悬空的短截线打个叉
            u8g2.drawLine(xPositions[i], topY, xPositions[i], topY + 8);
            u8g2.drawLine(xPositions[i] - 2, topY + 10, xPositions[i] + 2, topY + 14);
            u8g2.drawLine(xPositions[i] - 2, topY + 14, xPositions[i] + 2, topY + 10);
        }
    }

    u8g2.sendBuffer();
}

// 辅助：绘制单对线状态行（从 lambda 提取为私有静态函数，便于独立阅读）
void Display::drawPairRow(int y, const char* name, TestResult res,
                          float len, uint8_t shortWire, bool useFeet) {
    u8g2.setCursor(0, y);
    u8g2.print(name);

    int lineX1 = 22;
    int lineX2 = 55;
    int lineY  = y - 3;

    if (res == TestResult::PASS) {
        u8g2.drawLine(lineX1, lineY, lineX2, lineY);
        u8g2.drawFrame(lineX2, lineY - 2, 4, 5); // 远端环回小方框
        u8g2.drawStr(lineX2 + 8, y, "PASS");

    } else if (res == TestResult::OPEN) {
        u8g2.drawLine(lineX1, lineY, lineX1 + 10, lineY);
        u8g2.drawStr(lineX1 + 13, y + 1, "x");

        char buf[16];
        float displayLen = useFeet ? (len * 3.28084f) : len;
        snprintf(buf, sizeof(buf), "%.1f%s", displayLen, useFeet ? "ft" : "m");
        u8g2.drawStr(lineX2 + 8, y, buf);

    } else if (res == TestResult::SHORT_OR_CROSS) {
        u8g2.drawLine(lineX1, lineY, lineX2, lineY);
        // 对地短路符号
        u8g2.drawLine(lineX1 + 15, lineY, lineX1 + 15, lineY + 4);
        u8g2.drawLine(lineX1 + 12, lineY + 4, lineX1 + 18, lineY + 4);

        char buf[32];
        if (shortWire == 0) {
            snprintf(buf, sizeof(buf), "SHT(?)");
        } else if (len > 0.0f) {
            float displayLen = useFeet ? (len * 3.28084f) : len;
            snprintf(buf, sizeof(buf), "SHT@%.1f%s", displayLen, useFeet ? "ft" : "m");
        } else {
            snprintf(buf, sizeof(buf), "SHT(%d)", shortWire);
        }
        u8g2.drawStr(lineX2 + 8, y, buf);
    }
}

// 正常模式：逐对显示通断状态、长度、短路位置
void Display::drawNormalWiremap(int startY, const CableStatus& status, bool useFeet) {
    u8g2.setFont(u8g2_font_ncenB08_tr);
    drawPairRow(startY + 11, "1-2", status.pair1, status.len1, status.shortWire1, useFeet);
    drawPairRow(startY + 23, "3-6", status.pair2, status.len2, status.shortWire2, useFeet);
    drawPairRow(startY + 35, "4-5", status.pair3, status.len3, status.shortWire3, useFeet);
    drawPairRow(startY + 47, "7-8", status.pair4, status.len4, status.shortWire4, useFeet);
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

void Display::renderCalibError(const char* line1, const char* line2) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_ncenB10_tr);
    u8g2.drawStr(5, 25, "ERROR!");
    u8g2.setFont(u8g2_font_ncenB08_tr);
    u8g2.drawStr(0, 45, line1);
    u8g2.drawStr(0, 60, line2);
    u8g2.sendBuffer();
}

void Display::renderMessage(const char* msg) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_ncenB10_tr);
    int w = u8g2.getStrWidth(msg);
    u8g2.drawStr((128 - w) / 2, 35, msg);
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

void Display::renderSettings(int selectedIndex, bool useFeet, bool soundOn) {
    u8g2.clearBuffer();
    
    // Header
    u8g2.setFont(u8g2_font_ncenB08_tr);
    const char* title = "--- Settings ---";
    int w = u8g2.getStrWidth(title);
    u8g2.drawStr((128 - w) / 2, 12, title);
    u8g2.drawLine(0, 15, 127, 15);
    
    // Menu items
    const int maxVisible = 3;
    int scrollOffset = 0;
    if (selectedIndex >= maxVisible) {
        scrollOffset = selectedIndex - maxVisible + 1;
    }
    
    for (int i = 0; i < maxVisible; i++) {
        int itemIndex = scrollOffset + i;
        if (itemIndex >= MENU_COUNT) break;
        
        int yPos = 30 + i * 15;
        
        // Highlight cursor
        if (itemIndex == selectedIndex) {
            u8g2.drawBox(0, yPos - 11, 128, 14);
            u8g2.setDrawColor(0); // Text color black on white
        }
        
        char buf[32];
        switch(itemIndex) {
            case MENU_VIEW_HISTORY: snprintf(buf, sizeof(buf), "View History"); break;
            case MENU_CLEAR_HISTORY: snprintf(buf, sizeof(buf), "Clear History"); break;
            case MENU_TOGGLE_UNIT: snprintf(buf, sizeof(buf), "Unit: %s", useFeet ? "ft" : "m"); break;
            case MENU_TOGGLE_SOUND: snprintf(buf, sizeof(buf), "Sound: %s", soundOn ? "ON" : "OFF"); break;
            case MENU_CALIBRATE: snprintf(buf, sizeof(buf), "Calibrate"); break;
            case MENU_EXIT: snprintf(buf, sizeof(buf), "Exit"); break;
        }
        
        u8g2.drawStr(10, yPos, buf);
        u8g2.setDrawColor(1); // Restore text color
    }
    
    u8g2.sendBuffer();
}
