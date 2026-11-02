#include "pch.h"

#include <timeapi.h>

#include "now_playing_progress.h"
#include "menu_items.h"

#pragma comment(lib, "winmm.lib")

namespace cui::now_playing_progress {

namespace {

using Clock = std::chrono::steady_clock;

constexpr wchar_t window_class_name[] = L"columns_ui_now_playing_progress";
constexpr UINT_PTR timer_id = 1;
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
// 跳轉之後 fb2k 回報的位置要過一下才會更新，這段時間先顯示跳轉的目標，免得紅線先彈回舊位置
constexpr auto seek_hold = std::chrono::milliseconds(400);

const Gdiplus::Color background_colour(255, 24, 24, 24);
const Gdiplus::Color track_colour(255, 61, 61, 61);
const Gdiplus::Color played_colour(255, 247, 11, 11);

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

    void tick();
    View sample_view();
    double smooth_position(double source, bool paused);
    void apply_view(const View& view);
    void set_interval(UINT ms);

    double ratio_from_x(int x) const;
    void begin_drag(int x);
    void finish_drag(bool apply);
    void seek_to(double ratio);
    void invalidate();

    HWND m_wnd{};
    UINT m_interval{};
    // 計時精度是否已調成 1 ms（要成對還原）：預設 15.6 ms 的精度下 16 ms 的計時器常常要等到 31 ms
    bool m_high_resolution_timer{};

    View m_view;
    bool m_hover{};
    bool m_tracking_mouse{};
    bool m_dragging{};
    double m_drag_ratio{};

    // 位置濾波：上一次的來源位置與時間、當時是否暫停，以及目前吸收掉的偏差（顯示位置＝來源位置＋偏差）
    bool m_position_valid{};
    bool m_position_paused{};
    double m_position_source{};
    double m_position_offset{};
    Clock::time_point m_position_time{};

    struct SeekHold {
        double seconds{};
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
        set_interval(interval_idle_ms);
        return 0;
    case WM_DESTROY:
        KillTimer(m_wnd, timer_id);
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
    case WM_TIMER:
        if (wp == timer_id) {
            try {
                tick();
            }
            CATCH_LOG()
            return 0;
        }
        break;
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

void ProgressBar::set_interval(UINT ms)
{
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
    SetTimer(m_wnd, timer_id, ms, nullptr);
}

double ProgressBar::smooth_position(double source, bool paused)
{
    const auto now = Clock::now();

    if (m_position_valid) {
        const auto dt = std::chrono::duration<double>(now - m_position_time).count();
        const auto expected = (paused && m_position_paused) ? 0.0 : dt;
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

    const auto now = Clock::now();
    double position{};

    if (m_seek_hold && now < m_seek_hold->until) {
        position = m_seek_hold->seconds;
    } else {
        if (m_seek_hold) {
            m_seek_hold.reset();
            m_position_valid = false;
        }

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

    playback->playback_seek(seconds);
    m_seek_hold = SeekHold{seconds, Clock::now() + seek_hold};
    m_position_valid = false;
}

} // namespace

HWND create(HWND parent)
{
    static const auto class_registered = [] {
        WNDCLASSEX window_class{};
        window_class.cbSize = sizeof(window_class);
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
    SetWindowPos(wnd, HWND_TOP, bar_rect.left, bar_rect.top, bar_rect.right - bar_rect.left,
        uih::scale_dpi_value(bar_height_dip), SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

} // namespace cui::now_playing_progress
