#ifndef DISPLAY_H
#define DISPLAY_H

#include <U8g2lib.h>
#include <Wire.h>
#include "CableTester.h"

class Display {
public:
    // 构造函数，初始化 U8g2 对象
    Display();
    
    // 为什么要这么做：封装 OLED 的硬件初始化细节
    // 业务目的：在主程序启动时调用，配置 I2C 总线并点亮屏幕
    void init();
    
    // 为什么要这么做：接收底层测试模块输出的状态数据，并将其图形化展示
    // 业务目的：解耦测试逻辑与显示逻辑，测试结果一旦更新就调用此函数刷新屏幕
    // 参数 useFeet: 是否使用英尺(feet)作为长度单位，false 则使用米(m)
    void renderResult(const CableStatus& status, bool useFeet, bool isCalibrated);
    
    // 渲染历史记录界面
    void renderHistory(const CableStatus& status, bool useFeet, int index, int total);
    
    // 渲染待机或欢迎界面
    void renderReady();

    // 渲染正在测量的加载界面
    void renderMeasuring();

    // 校准过程界面
    void renderCalibStep1();
    void renderCalibStep2();
    void renderCalibDone();
    void renderCalibFailed();
    void renderCalibError(const char* line1, const char* line2);
    
    // 显示通用提示信息
    void renderMessage(const char* msg);
    
    // 渲染设置菜单
    void renderSettings(int selectedIndex, bool useFeet, bool soundOn);
    
    // 未校准提示界面
    void renderUncalibratedWarning(uint32_t timeoutSec);

    // 关闭屏幕电源进入休眠
    void sleep();

private:
    // 使用 U8g2 库定义的 I2C 128x64 OLED 对象（ESP32 硬件 I2C）
    U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2;
    
    // 将枚举结果转为对应的可读字符串
    const char* statusToStr(TestResult res);
    
    // 辅助函数：绘制图形化线序（顶层分发）
    void drawGraphicalWiremap(int yOffset, const CableStatus& status, bool useFeet);
    // 故障模式：贪心映射 + 交叉走线图
    void drawFaultWiremap(int startY, const CableStatus& status);
    // 正常模式：逐对状态文字行
    void drawNormalWiremap(int startY, const CableStatus& status, bool useFeet);
    // 单对线状态行绘制（从 lambda 提取，便于复用）
    void drawPairRow(int y, const char* name, TestResult res,
                    float len, uint8_t shortWire, bool useFeet);
};

#endif // DISPLAY_H
