#include "Buzzer.h"
#include "AppConfig.h"

Buzzer buzzer;

Buzzer::Buzzer() : _pin(255), _currentPattern(BEEP_NONE), _stateStartTime(0), _statePhase(0), _isOn(false) {
}

void Buzzer::begin(uint8_t pin) {
    _pin = pin;
    pinMode(_pin, OUTPUT);
    _setTone(false);
}

void Buzzer::_setTone(bool on, uint16_t freq) {
    if (_pin == 255) return;
    
    // 只有在配置中开启声音时才真正输出 PWM 方波
    if (on && appConfig.soundOn) {
        tone(_pin, freq); // 使用指定频率发声
        _isOn = true;
    } else {
        noTone(_pin); // 停止 PWM 方波
        digitalWrite(_pin, LOW);
        _isOn = false;
    }
}

void Buzzer::play(BeepPattern pattern) {
    if (!appConfig.soundOn) return; // 如果声音关闭，不打断当前逻辑，但直接丢弃新请求

    _currentPattern = pattern;
    _stateStartTime = millis();
    _statePhase = 0;

    // 根据模式初始状态发声
    switch (_currentPattern) {
        case BEEP_NONE:
            _setTone(false);
            break;
        case BEEP_SHORT:
        case BEEP_LONG:
        case BEEP_DOUBLE:
            _setTone(true);
            break;
        case BEEP_ERROR:
            _setTone(true, 2500); // 报警音从高音开始
            break;
        case BEEP_PASS:
            _setTone(true, 1200); // 欢快音调起点
            break;
        case BEEP_STARTUP:
            _setTone(true, 1000); // 开机音从较低频率开始
            break;
        case BEEP_SLEEP:
            _setTone(true, 3000); // 休眠音从较高频率开始
            break;
    }
}

void Buzzer::stop() {
    _currentPattern = BEEP_NONE;
    _statePhase = 0;
    _setTone(false);
}

void Buzzer::update() {
    if (_currentPattern == BEEP_NONE) return;

    uint32_t elapsed = millis() - _stateStartTime;

    switch (_currentPattern) {
        case BEEP_SHORT:
            if (elapsed >= 50) {
                stop();
            }
            break;
            
        case BEEP_LONG:
            if (elapsed >= 300) {
                stop();
            }
            break;

        case BEEP_DOUBLE:
            if (_statePhase == 0 && elapsed >= 80) {
                _setTone(false);
                _statePhase = 1;
                _stateStartTime = millis();
            } else if (_statePhase == 1 && elapsed >= 80) {
                _setTone(true);
                _statePhase = 2;
                _stateStartTime = millis();
            } else if (_statePhase == 2 && elapsed >= 80) {
                stop();
            }
            break;

        case BEEP_ERROR:
            // 报警模式：高低音交替的警报声 (Siren)，持续 1.8 秒
            if (_statePhase == 0 && elapsed >= 300) {
                _setTone(true, 1500);
                _statePhase = 1;
                _stateStartTime = millis();
            } else if (_statePhase == 1 && elapsed >= 300) {
                _setTone(true, 2500);
                _statePhase = 2;
                _stateStartTime = millis();
            } else if (_statePhase == 2 && elapsed >= 300) {
                _setTone(true, 1500);
                _statePhase = 3;
                _stateStartTime = millis();
            } else if (_statePhase == 3 && elapsed >= 300) {
                _setTone(true, 2500);
                _statePhase = 4;
                _stateStartTime = millis();
            } else if (_statePhase == 4 && elapsed >= 300) {
                _setTone(true, 1500);
                _statePhase = 5;
                _stateStartTime = millis();
            } else if (_statePhase == 5 && elapsed >= 300) {
                stop();
            }
            break;

        case BEEP_PASS:
            // 欢快音调 (类似玛丽欧吃金币)：四个快速升高的音符
            if (_statePhase == 0 && elapsed >= 60) {
                _setTone(true, 1600);
                _statePhase = 1;
                _stateStartTime = millis();
            } else if (_statePhase == 1 && elapsed >= 60) {
                _setTone(true, 2000);
                _statePhase = 2;
                _stateStartTime = millis();
            } else if (_statePhase == 2 && elapsed >= 60) {
                _setTone(true, 2400);
                _statePhase = 3;
                _stateStartTime = millis();
            } else if (_statePhase == 3 && elapsed >= 150) {
                stop();
            }
            break;
            
        case BEEP_STARTUP:
            // 升调开机音乐：1000Hz -> 1500Hz -> 2000Hz -> 2500Hz -> 3000Hz
            if (_statePhase == 0 && elapsed >= 80) {
                _setTone(true, 1500);
                _statePhase = 1;
                _stateStartTime = millis();
            } else if (_statePhase == 1 && elapsed >= 80) {
                _setTone(true, 2000);
                _statePhase = 2;
                _stateStartTime = millis();
            } else if (_statePhase == 2 && elapsed >= 80) {
                _setTone(true, 2500);
                _statePhase = 3;
                _stateStartTime = millis();
            } else if (_statePhase == 3 && elapsed >= 80) {
                _setTone(true, 3000);
                _statePhase = 4;
                _stateStartTime = millis();
            } else if (_statePhase == 4 && elapsed >= 120) {
                stop();
            }
            break;
            
        case BEEP_SLEEP:
            // 降调休眠音乐：3000Hz -> 2500Hz -> 2000Hz -> 1500Hz -> 1000Hz
            if (_statePhase == 0 && elapsed >= 80) {
                _setTone(true, 2500);
                _statePhase = 1;
                _stateStartTime = millis();
            } else if (_statePhase == 1 && elapsed >= 80) {
                _setTone(true, 2000);
                _statePhase = 2;
                _stateStartTime = millis();
            } else if (_statePhase == 2 && elapsed >= 80) {
                _setTone(true, 1500);
                _statePhase = 3;
                _stateStartTime = millis();
            } else if (_statePhase == 3 && elapsed >= 80) {
                _setTone(true, 1000);
                _statePhase = 4;
                _stateStartTime = millis();
            } else if (_statePhase == 4 && elapsed >= 120) {
                stop();
            }
            break;
            
        default:
            break;
    }
}
