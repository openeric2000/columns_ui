#pragma once

// 自編版新增：進度條、下一首提示、停止畫面、啟動淡入共用的小工具（2026-11-07 從各檔重複的寫法抽出來）
// - PositionSmoother：讓播放位置保持連續（進度條與下一首提示的倒數圈共用同一套）
// - TimerResolution：把系統計時精度調成 1 ms，成對還原
// - WindowTimer：掛在隱藏視窗上的雙頻 WM_TIMER（動畫時快、閒置時慢），動畫期間才調高計時精度

#include <timeapi.h>
#include <algorithm>
#include <chrono>
#include <cmath>

#pragma comment(lib, "winmm.lib")

namespace cui::nocturne_timing {

// 暫停→繼續瞬間 fb2k 回報的位置會往前跳約 0.2～0.3 秒（ASIO+DSD，錄影量到進度條紅線跳 2 px、倒數圈內數字早翻一格），
// 狂按暫停就一直小卡。單次不超過這個秒數的跳動當成回報誤差吸收掉，更大的（跳轉、換曲）照實顯示
inline constexpr double max_absorb_seconds = 0.6;
// 吸收掉的偏差用這個時間常數慢慢歸零，免得每次暫停繼續都累積落後
inline constexpr double offset_decay_seconds = 0.7;

// 讓播放位置保持連續。顯示位置＝來源位置＋偏差；每次比較「來源位置實際前進多少」與「這段時間理應前進多少
// （上一次取樣在播放＝經過的時間，上一次取樣在暫停＝0）」，差距在 max_absorb_seconds 以內就算回報誤差，記進偏差讓顯示位置不跳，
// 差距更大（跳轉）就重設偏差照實顯示；偏差再以 offset_decay_seconds 慢慢歸零
// 只能在同一個執行緒用（進度條在視窗執行緒、提示在主執行緒，各自一份）
class PositionSmoother {
public:
    using Clock = std::chrono::steady_clock;

    double filter(double source, bool paused)
    {
        const auto now = Clock::now();

        if (m_valid) {
            const auto dt = std::chrono::duration<double>(now - m_time).count();
            // 上一次取樣時是暫停，這段時間（暫停→繼續的那一次取樣，最多一個閒置間隔）位置沒有在走，應該前進 0。
            // 只在「前後都是暫停」時算 0 的話，繼續播放那一次取樣會把整段暫停時間當成播放中，位置一次往前補 dt
            // （2026-11-02 進度條自動化錄影：繼續第一步 0.25～1.0 px 不等，跟 dt 吻合）；下一首提示原本還是舊寫法，這次一起改成這樣
            const auto expected = m_paused ? 0.0 : dt;
            const auto residual = (source - m_source) - expected;

            if (std::abs(residual) <= max_absorb_seconds)
                m_offset -= residual;
            else
                m_offset = 0;

            // 暫停中偏差要原地不動，不然顯示位置會在暫停時慢慢往前爬
            if (!paused)
                m_offset *= std::exp(-dt / offset_decay_seconds);
        } else {
            m_offset = 0;
        }

        m_valid = true;
        m_paused = paused;
        m_source = source;
        m_time = now;
        return source + m_offset;
    }

    // 停止、換曲、沒有曲長等位置不連續的時候呼叫：下一次 filter 從頭開始
    void reset() { m_valid = false; }

private:
    bool m_valid{};
    bool m_paused{};
    double m_source{};
    double m_offset{};
    Clock::time_point m_time{};
};

// 把系統計時精度調成 1 ms（預設約 15.6 ms，16 ms 的計時器常常要等到 31 ms 才觸發，動畫只剩 33～41 Hz）。
// request(true) 和 request(false) 成對，重複呼叫同一個值不會重複計數；物件解構時自動還原
class TimerResolution {
public:
    TimerResolution() = default;
    TimerResolution(const TimerResolution&) = delete;
    TimerResolution& operator=(const TimerResolution&) = delete;
    ~TimerResolution() { request(false); }

    void request(bool want)
    {
        if (want == m_requested)
            return;

        if (want)
            timeBeginPeriod(1);
        else
            timeEndPeriod(1);
        m_requested = want;
    }

private:
    bool m_requested{};
};

// 掛在視窗上的 WM_TIMER，有兩種間隔：動畫中（快，要求 1 ms 計時精度）與閒置（慢，不影響耗電）。
// 視窗由呼叫端建立與銷毀；start 之前或視窗是 nullptr 時 set_interval 什麼都不做
class WindowTimer {
public:
    void start(HWND wnd, UINT_PTR id)
    {
        m_wnd = wnd;
        m_id = id;
        m_interval = 0;
    }

    // high_resolution 為真時（動畫間隔）把系統計時精度調成 1 ms，否則還原；間隔沒變就不重設計時器
    void set_interval(UINT ms, bool high_resolution)
    {
        if (!m_wnd)
            return;

        m_resolution.request(high_resolution);

        if (m_interval == ms)
            return;

        m_interval = ms;
        SetTimer(m_wnd, m_id, ms, nullptr);
    }

    // 停掉計時器並還原計時精度
    void stop()
    {
        if (m_wnd)
            KillTimer(m_wnd, m_id);

        m_resolution.request(false);
        m_wnd = nullptr;
        m_interval = 0;
    }

private:
    HWND m_wnd{};
    UINT_PTR m_id{};
    UINT m_interval{};
    TimerResolution m_resolution;
};

} // namespace cui::nocturne_timing
