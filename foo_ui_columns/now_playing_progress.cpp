#include "pch.h"

#include <timeapi.h>
#include <thread>

#include "now_playing_progress.h"
#include "menu_items.h"

#pragma comment(lib, "winmm.lib")

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace cui::now_playing_progress {

namespace {

using Clock = std::chrono::steady_clock;

constexpr wchar_t window_class_name[] = L"columns_ui_now_playing_progress";
// 計時執行緒通知視窗「該更新了」的訊息
constexpr UINT msg_tick = WM_USER + 0x4e6;
// 播放狀態改變（暫停、繼續、開始、停止、跳轉）時叫視窗馬上量一次，不等計時執行緒下一次醒來
constexpr UINT msg_wake = WM_USER + 0x4e7;
// 播放中每格間隔；暫停、停止時沒有東西在動，慢慢檢查就好；看不到時更慢
constexpr UINT interval_active_ms = 16;
constexpr UINT interval_idle_ms = 100;
constexpr UINT interval_hidden_ms = 250;

// 尺寸（DIP，乘上系統縮放才是像素）：跟 main.xml 的 seekbar 一樣，量過 125% 的畫面
// （高 15 px、軌道厚約 3.75 px 置中、左右縮進約 7.5 px＝圓點半徑）
constexpr int bar_height_dip = 12;
constexpr float track_height_dip = 3;
constexpr float thumb_radius_dip = 6;

// 暫停→繼續瞬間 fb2k 回報的位置會往前跳約 0.2～0.3 秒（ASIO+DSD），單次不超過這個秒數的跳動當成回報誤差吸收掉，
// 吸收掉的偏差再用 offset_decay_seconds 慢慢歸零（跟 next_track_toast.cpp 的 smooth_position 同一套）
constexpr double max_absorb_seconds = 0.6;
constexpr double offset_decay_seconds = 0.7;
// 跳轉之後到 fb2k 說「跳完了」之前先顯示跳轉的目標，免得紅線先彈回舊位置；等太久（沒收到通知）就放棄
// 2026-11-02 錄影：原本固定壓住 400 ms，fb2k 其實已經開始播、位置一直在前進，期滿紅線才突然往前跳 0.4 秒，看起來像卡一下
constexpr auto seek_hold_timeout = std::chrono::milliseconds(1500);

// 滑鼠滾輪（2026-11-05 使用者決定）：滑鼠在進度條上滾一格（WHEEL_DELTA）跳 5 秒，按住 Shift 跳 30 秒，向上滾＝快進
// 連續滾時紅線與圓點先走到累積的目標位置，滾輪停下來 wheel_commit_delay_ms 之後才真的送一次跳轉
// （fb2k 每次跳轉都會重開解碼，每格都送，ASIO／DSD 可能爆音或卡頓）
constexpr double wheel_step_seconds = 5.0;
constexpr double wheel_shift_step_seconds = 30.0;
constexpr UINT wheel_commit_delay_ms = 250;
constexpr UINT_PTR timer_wheel_commit = 1;

const Gdiplus::Color background_colour(255, 24, 24, 24);
const Gdiplus::Color track_colour(255, 61, 61, 61);
const Gdiplus::Color played_colour(255, 247, 11, 11);

// 每跳轉完成一次（任何來源：這個進度條、別的進度條、快速鍵）加一，進度條用它判斷自己發的跳轉完成了沒
std::atomic<uint64_t> seek_generation{0};

// 現存的進度條視窗（只在主執行緒增減）
std::vector<HWND>& live_bars()
{
    static std::vector<HWND> bars;
    return bars;
}

// 2026-11-02 自動化錄影：暫停時計時器 100 ms 才看一次，繼續播放沒人叫醒它，最多等 100 ms 才動，
// 動的時候補一步，紅線在繼續的瞬間往前跳 1.0～1.5 px（約 0.1 秒）
void wake_all()
{
    for (const auto wnd : live_bars())
        PostMessage(wnd, msg_wake, 0, 0);
}

class SeekPlayCallback : public play_callback_static {
public:
    unsigned get_flags() override
    {
        return flag_on_playback_seek | flag_on_playback_pause | flag_on_playback_starting | flag_on_playback_new_track
            | flag_on_playback_stop;
    }

    void on_playback_seek(double time) override
    {
        ++seek_generation;
        wake_all();
    }

    void on_playback_starting(play_control::t_track_command command, bool paused) override { wake_all(); }
    void on_playback_new_track(metadb_handle_ptr track) override { wake_all(); }
    void on_playback_stop(play_control::t_stop_reason reason) override { wake_all(); }
    void on_playback_pause(bool state) override { wake_all(); }
    void on_playback_edited(metadb_handle_ptr track) override {}
    void on_playback_dynamic_info(const file_info& info) override {}
    void on_playback_dynamic_info_track(const file_info& info) override {}
    void on_playback_time(double time) override {}
    void on_volume_change(float new_value) override {}
};

play_callback_static_factory_t<SeekPlayCallback> seek_play_callback;

struct Geometry {
    float left{};
    float right{};
    float centre_y{};
    float thickness{};
    float radius{};
};

Geometry make_geometry(int width, int height)
{
    const auto scale = static_cast<float>(uih::scale_dpi_value(1000)) / 1000.0f;
    const auto radius = thumb_radius_dip * scale;
    return {radius, static_cast<float>(width) - radius, static_cast<float>(height) / 2.0f, track_height_dip * scale,
        radius};
}

// 畫面上要畫的東西
struct View {
    // 播放中（含暫停）而且曲長已知：才畫圓點、才能點
    bool seekable{};
    // 已播放比例 0～1；拖曳時是滑鼠的位置
    double ratio{};
    bool paused{};
    bool playing{};
};

class ProgressBar {
public:
    static LRESULT CALLBACK s_wndproc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp);

private:
    explicit ProgressBar(HWND wnd) : m_wnd(wnd) {}

    LRESULT on_message(UINT msg, WPARAM wp, LPARAM lp);
    void on_paint();
    void draw(HDC dc, int width, int height) const;
    void ensure_buffer(HDC reference, int width, int height);

    void start_ticker();
    void stop_ticker();
    void ticker_main();
    void tick();
    View sample_view();
    double smooth_position(double source, bool paused);
    void apply_view(const View& view);
    void set_interval(UINT ms);

    double ratio_from_x(int x) const;
    void begin_drag(int x);
    void finish_drag(bool apply);
    void seek_to(double ratio);
    void on_mouse_wheel(WPARAM wp);
    void commit_wheel_seek();
    void cancel_wheel_seek();
    void invalidate();

    HWND m_wnd{};

    // 計時執行緒：用高精度的等待計時器每 m_period_ms 毫秒通知一次視窗（PostMessage，上一個還沒處理就不再送）。
    // 不用 WM_TIMER：它的優先權最低、粒度粗（16 ms 的計時器常常要等到 31 ms），錄影量到紅線只有約 37 Hz 在動
    std::thread m_ticker;
    wil::unique_event m_ticker_stop;
    wil::unique_event m_ticker_wake;
    std::atomic<UINT> m_period_ms{interval_idle_ms};
    std::atomic<bool> m_tick_pending{false};
    UINT m_interval{};
    // 計時精度是否已調成 1 ms（要成對還原）；高精度等待計時器不可用時才靠它
    bool m_high_resolution_timer{};

    View m_view;
    bool m_hover{};
    bool m_tracking_mouse{};
    bool m_dragging{};
    double m_drag_ratio{};

    // 滑鼠滾輪：還沒滿一格的滾動量（觸控板一次只給幾十單位），以及累積中、還沒送出的跳轉目標（秒）
    int m_wheel_remainder{};
    std::optional<double> m_wheel_target;

    // 位置濾波：上一次的來源位置與時間、當時是否暫停，以及目前吸收掉的偏差（顯示位置＝來源位置＋偏差）
    bool m_position_valid{};
    bool m_position_paused{};
    double m_position_source{};
    double m_position_offset{};
    Clock::time_point m_position_time{};

    struct SeekHold {
        double seconds{};
        // 發出跳轉前的 seek_generation；變大了就表示 fb2k 跳完了
        uint64_t generation{};
        Clock::time_point until;
    };
    std::optional<SeekHold> m_seek_hold;

    wil::unique_hdc m_memory_dc;
    wil::unique_hbitmap m_buffer;
    SIZE m_buffer_size{};
};

LRESULT CALLBACK ProgressBar::s_wndproc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE) {
        SetWindowLongPtr(wnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(new ProgressBar(wnd)));
        return TRUE;
    }

    const auto bar = reinterpret_cast<ProgressBar*>(GetWindowLongPtr(wnd, GWLP_USERDATA));
    if (!bar)
        return DefWindowProc(wnd, msg, wp, lp);

    const auto result = bar->on_message(msg, wp, lp);

    if (msg == WM_NCDESTROY) {
        SetWindowLongPtr(wnd, GWLP_USERDATA, 0);
        delete bar;
    }

    return result;
}

LRESULT ProgressBar::on_message(UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        live_bars().push_back(m_wnd);
        start_ticker();
        return 0;
    case WM_DESTROY:
        KillTimer(m_wnd, timer_wheel_commit);
        std::erase(live_bars(), m_wnd);
        stop_ticker();
        if (m_high_resolution_timer) {
            timeEndPeriod(1);
            m_high_resolution_timer = false;
        }
        break;
    case WM_ERASEBKGND:
        return TRUE;
    case WM_PAINT:
        on_paint();
        return 0;
    case msg_tick:
        m_tick_pending = false;
        [[fallthrough]];
    case msg_wake:
        try {
            tick();
        }
        CATCH_LOG()
        return 0;
    case WM_MOUSEMOVE:
        if (!m_tracking_mouse) {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, m_wnd, 0};
            m_tracking_mouse = TrackMouseEvent(&track) != FALSE;
        }
        if (!m_hover) {
            m_hover = true;
            invalidate();
        }
        if (m_dragging) {
            m_drag_ratio = ratio_from_x(GET_X_LPARAM(lp));
            apply_view(sample_view());
        }
        return 0;
    case WM_MOUSELEAVE:
        m_tracking_mouse = false;
        if (m_hover) {
            m_hover = false;
            invalidate();
        }
        return 0;
    case WM_MOUSEWHEEL:
        // get_msg_hook.cpp 會把滾輪訊息轉給滑鼠底下的視窗，所以不用先點進度條取得焦點
        on_mouse_wheel(wp);
        return 0;
    case WM_TIMER:
        if (wp == timer_wheel_commit) {
            commit_wheel_seek();
            return 0;
        }
        break;
    case WM_LBUTTONDOWN:
        begin_drag(GET_X_LPARAM(lp));
        return 0;
    case WM_LBUTTONUP:
        finish_drag(true);
        return 0;
    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
        // 拖到一半被別的視窗搶走滑鼠（對話框、Alt+Tab）：取消，不跳轉
        finish_drag(false);
        return 0;
    }

    return DefWindowProc(m_wnd, msg, wp, lp);
}

void ProgressBar::invalidate()
{
    InvalidateRect(m_wnd, nullptr, FALSE);
    UpdateWindow(m_wnd);
}

void ProgressBar::ensure_buffer(HDC reference, int width, int height)
{
    if (m_buffer && m_buffer_size.cx == width && m_buffer_size.cy == height)
        return;

    wil::unique_hbitmap bitmap(CreateCompatibleBitmap(reference, width, height));
    if (!bitmap)
        return;

    if (!m_memory_dc)
        m_memory_dc.reset(CreateCompatibleDC(reference));

    // 先選進新的圖，舊的才能刪
    SelectObject(m_memory_dc.get(), bitmap.get());
    m_buffer = std::move(bitmap);
    m_buffer_size = {width, height};
}

void ProgressBar::on_paint()
{
    PAINTSTRUCT ps{};
    const auto paint_dc = wil::BeginPaint(m_wnd, &ps);

    RECT client{};
    GetClientRect(m_wnd, &client);
    if (client.right <= 0 || client.bottom <= 0)
        return;

    // 先畫在記憶體圖上再一次貼出去，紅線每格移動時不會閃
    ensure_buffer(paint_dc.get(), client.right, client.bottom);
    if (!m_buffer || !m_memory_dc)
        return;

    draw(m_memory_dc.get(), client.right, client.bottom);
    BitBlt(paint_dc.get(), ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
        ps.rcPaint.bottom - ps.rcPaint.top, m_memory_dc.get(), ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
}

void ProgressBar::draw(HDC dc, int width, int height) const
{
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    // 整數座標對齊像素邊界，小數座標才會得到跟 ESLyric 相同的邊緣淡出（紅線可以平順地以小於 1 px 的步距前進）
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    graphics.Clear(background_colour);

    const auto geometry = make_geometry(width, height);
    const auto track_top = geometry.centre_y - geometry.thickness / 2;
    const auto track_width = geometry.right - geometry.left;
    const Gdiplus::SolidBrush track_brush(track_colour);
    const Gdiplus::SolidBrush played_brush(played_colour);

    graphics.FillRectangle(&track_brush, geometry.left, track_top, track_width, geometry.thickness);

    const auto played_x = geometry.left + static_cast<float>(std::clamp(m_view.ratio, 0.0, 1.0)) * track_width;
    if (played_x > geometry.left)
        graphics.FillRectangle(&played_brush, geometry.left, track_top, played_x - geometry.left, geometry.thickness);

    if (m_view.seekable && (m_hover || m_dragging)) {
        graphics.FillEllipse(&played_brush, played_x - geometry.radius, geometry.centre_y - geometry.radius,
            geometry.radius * 2, geometry.radius * 2);
    }
}

void ProgressBar::start_ticker()
{
    m_ticker_stop.create(wil::EventOptions::ManualReset);
    m_ticker_wake.create(wil::EventOptions::None);
    m_ticker = std::thread([this] { ticker_main(); });
}

void ProgressBar::stop_ticker()
{
    if (!m_ticker.joinable())
        return;

    m_ticker_stop.SetEvent();
    m_ticker.join();
}

void ProgressBar::ticker_main()
{
    // 高精度等待計時器（Windows 10 1803 之後）可以準到 1 ms 以內；不支援就退回一般的等待計時器
    wil::unique_handle timer(
        CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS));
    if (!timer)
        timer.reset(CreateWaitableTimerW(nullptr, FALSE, nullptr));
    if (!timer)
        return;

    const HANDLE handles[]{m_ticker_stop.get(), m_ticker_wake.get(), timer.get()};
    UINT period = 0;

    for (;;) {
        const auto wanted = m_period_ms.load();
        if (wanted != period) {
            LARGE_INTEGER due{};
            due.QuadPart = -static_cast<LONGLONG>(wanted) * 10000;
            SetWaitableTimer(timer.get(), &due, static_cast<LONG>(wanted), nullptr, nullptr, FALSE);
            period = wanted;
        }

        const auto result = WaitForMultipleObjects(static_cast<DWORD>(std::size(handles)), handles, FALSE, INFINITE);

        if (result == WAIT_OBJECT_0)
            return;

        if (result == WAIT_OBJECT_0 + 2) {
            // 上一個通知還沒處理完就不再送，視窗忙的時候不會堆積
            if (!m_tick_pending.exchange(true))
                PostMessage(m_wnd, msg_tick, 0, 0);
        } else if (result != WAIT_OBJECT_0 + 1) {
            return;
        }
    }
}

void ProgressBar::set_interval(UINT ms)
{
    // 一般等待計時器（沒有高精度可用時）的精度要靠 timeBeginPeriod，有高精度時多開也無妨
    const auto want_high_resolution = ms == interval_active_ms;
    if (want_high_resolution != m_high_resolution_timer) {
        if (want_high_resolution)
            timeBeginPeriod(1);
        else
            timeEndPeriod(1);
        m_high_resolution_timer = want_high_resolution;
    }

    if (m_interval == ms)
        return;

    m_interval = ms;
    m_period_ms = ms;
    m_ticker_wake.SetEvent();
}

double ProgressBar::smooth_position(double source, bool paused)
{
    const auto now = Clock::now();

    if (m_position_valid) {
        const auto dt = std::chrono::duration<double>(now - m_position_time).count();
        // 上一次取樣時是暫停，這段時間（暫停→繼續的那一次取樣，最多一個閒置間隔 100 ms）紅線沒有在走，應該前進 0。
        // 原本只在「前後都是暫停」時算 0，繼續播放那一次取樣把整段暫停時間當成播放中，紅線一次往前補 dt，
        // 2026-11-02 自動化錄影：繼續第一步 0.25～1.0 px 不等（取決於繼續時閒置計時器走到哪），跟 dt 吻合
        const auto expected = m_position_paused ? 0.0 : dt;
        const auto residual = (source - m_position_source) - expected;

        if (std::abs(residual) <= max_absorb_seconds)
            m_position_offset -= residual;
        else
            m_position_offset = 0;

        // 暫停中偏差要原地不動，不然顯示位置會在暫停時慢慢往前爬
        if (!paused)
            m_position_offset *= std::exp(-dt / offset_decay_seconds);
    } else {
        m_position_offset = 0;
    }

    m_position_valid = true;
    m_position_paused = paused;
    m_position_source = source;
    m_position_time = now;
    return source + m_position_offset;
}

View ProgressBar::sample_view()
{
    View view;
    const auto playback = playback_control::get();

    if (!playback->is_playing()) {
        m_position_valid = false;
        m_seek_hold.reset();
        cancel_wheel_seek();
        return view;
    }

    view.playing = true;
    view.paused = playback->is_paused();

    const auto length = playback->playback_get_length();
    if (length <= 0) {
        m_position_valid = false;
        return view;
    }

    view.seekable = true;

    if (m_dragging) {
        view.ratio = m_drag_ratio;
        return view;
    }

    // 滾輪累積中：顯示目標位置，等滾輪停下來才真的跳轉
    if (m_wheel_target) {
        view.ratio = std::clamp(*m_wheel_target / length, 0.0, 1.0);
        return view;
    }

    const auto now = Clock::now();
    double position{};

    // 跳轉還沒完成（fb2k 還沒通知）而且沒等太久：顯示目標位置；完成了就改跟 fb2k 回報的位置，不再多壓
    if (m_seek_hold && now >= m_seek_hold->until)
        m_seek_hold.reset();
    if (m_seek_hold && seek_generation.load() != m_seek_hold->generation) {
        m_seek_hold.reset();
        m_position_valid = false;
    }

    if (m_seek_hold) {
        position = m_seek_hold->seconds;
    } else {
        auto source = playback->playback_get_position();
        // 暫停中 ASIO+DSD 回報的位置會前後跳（見 menu_items.cpp），改用按下暫停時記下的位置
        if (view.paused) {
            if (const auto paused_position = main_menu::get_paused_position())
                source = *paused_position;
        }
        position = smooth_position(source, view.paused);
    }

    view.ratio = std::clamp(position / length, 0.0, 1.0);
    return view;
}

void ProgressBar::apply_view(const View& view)
{
    RECT client{};
    GetClientRect(m_wnd, &client);
    const auto geometry = make_geometry(client.right, client.bottom);
    const auto track_width = std::max(1.0f, geometry.right - geometry.left);

    // 紅線前進不到 0.01 px 就不重畫
    const auto changed = view.seekable != m_view.seekable
        || std::abs(view.ratio - m_view.ratio) * track_width >= 0.01;
    m_view = view;

    if (changed)
        invalidate();
}

void ProgressBar::tick()
{
    if (!IsWindowVisible(m_wnd)) {
        set_interval(interval_hidden_ms);
        return;
    }

    const auto view = sample_view();
    apply_view(view);
    set_interval(view.playing && !view.paused ? interval_active_ms : interval_idle_ms);
}

double ProgressBar::ratio_from_x(int x) const
{
    RECT client{};
    GetClientRect(m_wnd, &client);
    const auto geometry = make_geometry(client.right, client.bottom);
    const auto track_width = geometry.right - geometry.left;
    if (track_width <= 0)
        return 0;

    return std::clamp(static_cast<double>((static_cast<float>(x) - geometry.left) / track_width), 0.0, 1.0);
}

void ProgressBar::begin_drag(int x)
{
    // 滾輪還在累積、沒送出的跳轉作廢，以點下去的位置為準（不然拖完放開後，計時一到又跳回滾輪的目標）
    cancel_wheel_seek();

    // 這一格的畫面狀態可能還沒更新，點下去前先量一次
    apply_view(sample_view());
    if (!m_view.seekable)
        return;

    m_dragging = true;
    m_drag_ratio = ratio_from_x(x);
    SetCapture(m_wnd);
    apply_view(sample_view());
}

void ProgressBar::finish_drag(bool apply)
{
    if (!m_dragging)
        return;

    m_dragging = false;
    if (GetCapture() == m_wnd)
        ReleaseCapture();

    if (apply)
        seek_to(m_drag_ratio);

    apply_view(sample_view());
}

void ProgressBar::seek_to(double ratio)
{
    const auto playback = playback_control::get();
    const auto length = playback->playback_get_length();
    if (!playback->is_playing() || length <= 0)
        return;

    // 跳到最後一刻會直接結束這首，留一點
    const auto target = std::clamp(ratio, 0.0, 1.0) * length;
    const auto seconds = std::clamp(target, 0.0, std::max(0.0, length - 0.1));

    // 先記下目前的跳轉次數再發出跳轉：fb2k 的通知可能在 playback_seek 裡就同步送出
    m_seek_hold = SeekHold{seconds, seek_generation.load(), Clock::now() + seek_hold_timeout};
    m_position_valid = false;
    playback->playback_seek(seconds);
}

void ProgressBar::on_mouse_wheel(WPARAM wp)
{
    // 拖曳中、沒在播放、曲長不明：跟點擊一樣不處理
    if (m_dragging)
        return;

    const auto playback = playback_control::get();
    const auto length = playback->playback_get_length();
    if (!playback->is_playing() || length <= 0)
        return;

    // 觸控板一次只給幾十單位，累積滿一格（WHEEL_DELTA）才算；換方向時丟掉沒滿一格的零頭
    const int delta = GET_WHEEL_DELTA_WPARAM(wp);
    if ((m_wheel_remainder > 0) != (delta > 0))
        m_wheel_remainder = 0;
    m_wheel_remainder += delta;

    const auto notches = m_wheel_remainder / WHEEL_DELTA;
    if (notches == 0)
        return;
    m_wheel_remainder -= notches * WHEEL_DELTA;

    const auto step = (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT) ? wheel_shift_step_seconds : wheel_step_seconds;

    // 起點：已經在累積就從累積的目標接著加；否則從目前顯示的位置（先量一次，免得拿到上一格的）
    double base{};
    if (m_wheel_target) {
        base = *m_wheel_target;
    } else {
        apply_view(sample_view());
        if (!m_view.seekable)
            return;
        base = m_view.ratio * length;
    }

    // 跳到最後一刻會直接結束這首，留一點（跟 seek_to 同一個規則）
    m_wheel_target = std::clamp(base + notches * step, 0.0, std::max(0.0, length - 0.1));

    // 每滾一格就重新計時，滾輪停下來才送出跳轉
    SetTimer(m_wnd, timer_wheel_commit, wheel_commit_delay_ms, nullptr);
    apply_view(sample_view());
}

void ProgressBar::commit_wheel_seek()
{
    KillTimer(m_wnd, timer_wheel_commit);
    if (!m_wheel_target)
        return;

    const auto seconds = *m_wheel_target;
    m_wheel_target.reset();
    m_wheel_remainder = 0;

    // seek_to 會記下跳轉目標，fb2k 說跳完之前紅線停在目標，不會彈回舊位置
    const auto length = playback_control::get()->playback_get_length();
    if (length > 0)
        seek_to(seconds / length);

    apply_view(sample_view());
}

void ProgressBar::cancel_wheel_seek()
{
    if (m_wheel_target)
        KillTimer(m_wnd, timer_wheel_commit);

    m_wheel_target.reset();
    m_wheel_remainder = 0;
}

} // namespace

HWND create(HWND parent)
{
    static const auto class_registered = [] {
        WNDCLASSEX window_class{};
        window_class.cbSize = sizeof(window_class);
        // 大小一變就整個視窗重畫。預設（沒有這兩個樣式）放大時只重畫新增的那一段、舊畫面保留，舊線右端 6 px 的縮進
        // 就留成缺口；停止／暫停時沒有東西會觸發重畫，要等滑鼠移上去才接回來（2026-11-03 使用者回報：視窗先縮小再放大到全螢幕）
        window_class.style = CS_HREDRAW | CS_VREDRAW;
        window_class.lpfnWndProc = ProgressBar::s_wndproc;
        window_class.hInstance = core_api::get_my_instance();
        window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
        window_class.lpszClassName = window_class_name;
        return RegisterClassEx(&window_class) != 0;
    }();

    if (!class_registered)
        return nullptr;

    return CreateWindowEx(0, window_class_name, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, parent, nullptr,
        core_api::get_my_instance(), nullptr);
}

void layout(HWND wnd, const RECT& bar_rect)
{
    const auto width = bar_rect.right - bar_rect.left;
    const auto height = uih::scale_dpi_value(bar_height_dip);

    // 位置、大小都沒變、已經顯示而且在最上層就什麼都不做（排版時已經用 defer_layout 放好的話就是這種情況）
    RECT current{};
    GetWindowRect(wnd, &current);
    MapWindowPoints(HWND_DESKTOP, GetParent(wnd), reinterpret_cast<POINT*>(&current), 2);
    const auto on_top = GetWindow(GetParent(wnd), GW_CHILD) == wnd;

    if (IsWindowVisible(wnd) && on_top && current.left == bar_rect.left && current.top == bar_rect.top
        && current.right - current.left == width && current.bottom - current.top == height)
        return;

    SetWindowPos(wnd, HWND_TOP, bar_rect.left, bar_rect.top, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

HDWP defer_layout(HDWP dwp, HWND wnd, const RECT& bar_rect)
{
    return DeferWindowPos(dwp, wnd, HWND_TOP, bar_rect.left, bar_rect.top, bar_rect.right - bar_rect.left,
        uih::scale_dpi_value(bar_height_dip), SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

} // namespace cui::now_playing_progress
