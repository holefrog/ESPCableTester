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

void Buzzer::_setTone(bool on) {
    if (_pin == 255) return;
    
    // 只有在配置中开启声音时才真正输出 PWM 方波
    if (on && appConfig.soundOn) {
        tone(_pin, 2500); // 使用 2500Hz 的频率发声（适合大多数被动蜂鸣器的谐振频率）
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
        case BEEP_ERROR:
        case BEEP_STARTUP:
            _setTone(true);
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
            // 报警模式：短促的三次鸣叫 (Beep-pause-Beep-pause-Beep)
            if (_statePhase == 0 && elapsed >= 80) {
                _setTone(false);
                _statePhase = 1;
                _stateStartTime = millis();
            } else if (_statePhase == 1 && elapsed >= 80) {
                _setTone(true);
                _statePhase = 2;
                _stateStartTime = millis();
            } else if (_statePhase == 2 && elapsed >= 80) {
                _setTone(false);
                _statePhase = 3;
                _stateStartTime = millis();
            } else if (_statePhase == 3 && elapsed >= 80) {
                _setTone(true);
                _statePhase = 4;
                _stateStartTime = millis();
            } else if (_statePhase == 4 && elapsed >= 80) {
                stop();
            }
            break;

        case BEEP_STARTUP:
            // 开机音：一次中等长度鸣叫，稍微带点间隔 (也可以写成其它旋律，但由于是单频蜂鸣器，只能靠节奏)
            if (_statePhase == 0 && elapsed >= 150) {
                _setTone(false);
                _statePhase = 1;
                _stateStartTime = millis();
            } else if (_statePhase == 1 && elapsed >= 50) {
                _setTone(true);
                _statePhase = 2;
                _stateStartTime = millis();
            } else if (_statePhase == 2 && elapsed >= 200) {
                stop();
            }
            break;
            
        default:
            break;
    }
}
