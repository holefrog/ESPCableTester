#ifndef BUZZER_H
#define BUZZER_H

#include <Arduino.h>

enum BeepPattern {
    BEEP_NONE = 0,
    BEEP_SHORT,       // 短促单次，用于按键反馈
    BEEP_LONG,        // 较长单次，用于成功提示
    BEEP_DOUBLE,      // 两次短鸣，用于特定状态确认或警告
    BEEP_ERROR,       // 急促报警音
    BEEP_PASS,        // 欢快的音调，用于测线通过
    BEEP_STARTUP,     // 开机提示音 (5个音符升调)
    BEEP_SLEEP        // 休眠提示音 (5个音符降调)
};

class Buzzer {
public:
    Buzzer();
    void begin(uint8_t pin);
    void update();
    void play(BeepPattern pattern);
    void stop();

private:
    uint8_t _pin;
    BeepPattern _currentPattern;
    uint32_t _stateStartTime;
    uint8_t _statePhase;
    bool _isOn;

    void _setTone(bool on, uint16_t freq = 2500);
};

extern Buzzer buzzer;

#endif // BUZZER_H
