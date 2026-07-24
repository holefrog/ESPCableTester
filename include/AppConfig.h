/**
 * @file AppConfig.h
 * @brief 应用配置与历史记录管理模块
 *
 * 职责说明
 * --------
 * 本模块负责：
 * 1. **用户偏好设置**（单位、声音）的 NVS Flash 持久化读写
 * 2. **校准参数**（baseCycles、cyclesPerMeter）的 NVS Flash 存储与加载
 * 3. **历史记录**（最近 10 条测试结果）的 FIFO 队列管理和 Flash 持久化
 *
 * NVS 命名空间
 * ------------
 * - "cable_test"    存储校准参数（b0~b3, p0~p3）和偏好设置（useFeet, soundOn）
 * - "cable_history" 存储历史记录数量（hCount）和记录数据（hLogs）
 *
 * Flash 写入策略
 * --------------
 * 偏好设置（toggleUnit / toggleSound）在用户操作时**立即**写入 Flash；
 * 历史记录（addHistory）在每次新增后**立即**写入 Flash。
 * 这种"即时写入"的设计确保意外断电时不丢数据，代价是每次操作会消耗一次 Flash 写入寿命。
 * ESP32 NVS 的 Flash 页写入寿命约 10 万次，对普通使用强度足够。
 *
 * historyIndex 说明
 * -----------------
 * historyIndex（当前查看的历史条目编号）是 UI 导航状态，不属于配置数据，
 * 因此不在本类管理，而由 main.cpp 的全局变量 historyIndex 维护。
 *
 * 全局实例
 * --------
 * 本模块通过 `extern AppConfig appConfig` 暴露一个全局单例，
 * 各模块直接访问 appConfig.xxx，无需手动创建实例。
 */

#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <Arduino.h>
#include "CableTester.h"

/**
 * @brief 应用配置与历史记录管理类
 */
class AppConfig {
public:
    static const int MAX_HISTORY = 10;  ///< 历史记录最大条数（FIFO 满后覆盖最旧的）

    // =========================================================================
    // 用户偏好设置（持久化到 NVS "cable_test" 命名空间）
    // =========================================================================
    bool useFeet;       ///< 长度单位：true=英尺，false=米
    bool soundOn;       ///< 蜂鸣器开关（预留字段，当前硬件暂无蜂鸣器引脚）
    bool isCalibrated;  ///< 是否已完成过校准（校准数据写入后置 true）

    // =========================================================================
    // 历史记录（持久化数据，最近 MAX_HISTORY 条测试结果的 FIFO 队列）
    // =========================================================================
    CableStatus historyLogs[MAX_HISTORY];  ///< 历史记录数组（下标 0 最旧，historyCount-1 最新）
    int historyCount;                      ///< 当前有效历史记录条数（0 ~ MAX_HISTORY）

    // =========================================================================
    // 公共方法
    // =========================================================================

    /** @brief 构造函数，设置安全的默认值（未校准、英尺、声音开启、历史为空） */
    AppConfig();

    /**
     * @brief 从 NVS Flash 加载所有持久化数据（校准参数 + 偏好 + 历史记录）
     *
     * 必须在 setup() 中调用一次，在此之前所有字段均为构造函数默认值。
     * @param tester 校准参数加载完成后会通过此引用设置到 CableTester 内部
     */
    void loadAll(CableTester& tester);

    /**
     * @brief 保存校准参数到 NVS Flash，并同步到 CableTester
     *
     * 校准完成后调用。会将 isCalibrated 置为 true。
     * @param base   4 个线对的基准充电周期数（空载测量值）
     * @param perM   4 个线对每米对应的充电周期增量
     * @param tester 同步写入 CableTester 的内部校准数据
     */
    void saveCalibration(const uint32_t base[4], const uint32_t perM[4], CableTester& tester);

    /**
     * @brief 切换长度单位（英尺 ↔ 米），并立即保存到 NVS Flash
     */
    void toggleUnit();

    /**
     * @brief 切换蜂鸣器开关，并立即保存到 NVS Flash
     */
    void toggleSound();

    /**
     * @brief 添加一条历史记录，满 MAX_HISTORY 时淘汰最旧的，并立即写入 Flash
     *
     * @param status 要保存的测试结果
     */
    void addHistory(const CableStatus& status);

    /**
     * @brief 清空所有历史记录，并立即写入 Flash
     */
    void clearHistory();

    /**
     * @brief 将当前历史记录数据写入 NVS Flash（内部调用，通常不需要外部直接调用）
     */
    void saveHistoryToFlash();

private:
    /** @brief 从 NVS "cable_test" 加载校准参数和偏好设置 */
    void loadCalibration(CableTester& tester);

    /** @brief 从 NVS "cable_history" 加载历史记录 */
    void loadHistory();
};

/** @brief 全局 AppConfig 单例，在 AppConfig.cpp 中定义 */
extern AppConfig appConfig;

#endif // APP_CONFIG_H
