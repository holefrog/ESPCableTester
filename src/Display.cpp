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
    
    // ==========================================
    // 2. 蓝色区域 (底端 48 像素，y: 16~63)
    // ==========================================
    // 辅助闭包函数
    auto drawLine = [&](int y, const char* label, TestResult res, float len) {
        u8g2.setCursor(0, y);
        u8g2.print(label);
        
        char buf[16];
        if (res == TestResult::OPEN && isCalibrated) {
            float displayLen = useFeet ? (len * 3.28084f) : len;
            const char* unit = useFeet ? "ft" : "m";
            snprintf(buf, sizeof(buf), "%.1f%s", displayLen, unit);
        } else {
            snprintf(buf, sizeof(buf), "%s", statusToStr(res));
        }
        
        u8g2.drawStr(128 - u8g2.getStrWidth(buf), y, buf);
    };
    
    // 均分蓝区的 48 像素高度：四根基准线分别放在 27, 39, 51, 63
    drawLine(27, "1-2 (Org):", status.pair1, status.len1); // 缩写颜色避免越界
    drawLine(39, "3-6 (Grn):", status.pair2, status.len2);
    drawLine(51, "4-5 (Blu):", status.pair3, status.len3);
    drawLine(63, "7-8 (Brn):", status.pair4, status.len4);
    
    u8g2.sendBuffer(); // 将内存缓冲一次性推送到 OLED 进行物理刷新
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
