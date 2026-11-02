#include "pch.h"

#include <timeapi.h>

#include "font_utils.h"
#include "next_track_toast.h"
#include "nocturne_text.h"
#include "splitter.h"

#pragma comment(lib, "winmm.lib")

// 自編版新增：停止（佇列還有歌）時的展開頁（2026-11-02）
// - 停止後 ESLyric 會把展開頁清成全黑，只剩角落一張「已停止 · 接下來」，看不出是什麼狀態
// - 做法：播放中把展開頁截下來備用（每首歌在 1.5 秒與 6 秒各一次，換視窗大小也補截），按停止後在展開頁上蓋一個
//   不擋滑鼠、不搶焦點的分層視窗，畫變暗的最後一頁，正中間浮一個「已停止」
// - 不在按下停止的瞬間才截：那時 ESLyric 可能已經把畫面清掉
// - 角落的「已停止 · 接下來」提示照舊（next_track_toast.cpp），這裡的視窗刻意放在它底下
// - 開關跟下一首提示同一個（next_track_toast::enabled），關掉就完全不出現

namespace cui::stopped_page {

namespace {

using panels::splitter::FlatSplitterPanel;
using Clock = std::chrono::steady_clock;

constexpr wchar_t page_class_name[] = L"columns_ui_stopped_page";
constexpr wchar_t timer_class_name[] = L"columns_ui_stopped_page_timer";
constexpr UINT_PTR timer_id = 1;
constexpr UINT interval_active_ms = 16;
constexpr UINT interval_idle_ms = 150;
constexpr double fade_in_seconds = 0.5;
constexpr double fade_out_seconds = 0.4;
// 單次動畫最多推進這麼多秒：主執行緒卡住時淡入淡出順著時間拖長，不會一次跳一大截
constexpr double max_step_seconds = 0.04;
// 這一首播到第幾秒時截圖：第一次等封面與標題畫好，第二次補上載入比較慢的封面
constexpr double first_capture_seconds = 1.5;
constexpr double second_capture_seconds = 6.0;
// 變暗：原畫面佔 (100 - dim_percent)％，其餘用底色蓋上去（預覽頁的 rgba(24,24,24,0.74) 再淡一點點）
constexpr int dim_percent = 72;
constexpr int dim_color = 24;
// 沒有截到圖（重開 fb2k 後直接停在停止、視窗大小變了）時用的底色，等於 #202020 變暗後的顏色
constexpr BYTE fallback_gray = 26;
// 字的視覺中心在基線上方多少（字級的倍數），跟下一首提示用同一個值
constexpr float ink_center_above_baseline = 0.38f;
// 「已停止」膠囊：字級 24 DIP
constexpr double label_font_dip = 24;

std::atomic<bool> g_alive{false};

Gdiplus::Color rgba(BYTE a, BYTE r, BYTE g, BYTE b)
{
    return Gdiplus::Color(a, r, g, b);
}

void add_round_rect(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& rect, float radius)
{
    const auto d = radius * 2;
    path.AddArc(rect.X, rect.Y, d, d, 180, 90);
    path.AddArc(rect.GetRight() - d, rect.Y, d, d, 270, 90);
    path.AddArc(rect.GetRight() - d, rect.GetBottom() - d, d, d, 0, 90);
    path.AddArc(rect.X, rect.GetBottom() - d, d, d, 90, 90);
    path.CloseFigure();
}

// 主題字型（Columns UI 的 Common (labels)）；找不到就用系統無襯線
std::unique_ptr<Gdiplus::FontFamily> create_font_family()
{
    const auto log_font = fonts::get_log_font_with_fallback(fonts::labels_font_id);
    auto family = std::make_unique<Gdiplus::FontFamily>(log_font.lfFaceName);

    if (family->GetLastStatus() != Gdiplus::Ok)
        family.reset(Gdiplus::FontFamily::GenericSansSerif()->Clone());

    return family;
}

Gdiplus::FontStyle pick_font_style(const Gdiplus::FontFamily& family)
{
    if (family.IsStyleAvailable(Gdiplus::FontStyleRegular))
        return Gdiplus::FontStyleRegular;
    if (family.IsStyleAvailable(Gdiplus::FontStyleBold))
        return Gdiplus::FontStyleBold;
    return Gdiplus::FontStyleItalic;
}

// 正中間的「■ 已停止」膠囊
void draw_label(Gdiplus::Bitmap& bitmap, int width, int height, double scale)
{
    Gdiplus::Graphics g(&bitmap);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

    const auto family = create_font_family();
    const auto style = pick_font_style(*family);
    const auto font_size = static_cast<float>(label_font_dip * scale);
    const Gdiplus::Font font(family.get(), font_size, style, Gdiplus::UnitPixel);

    Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
    format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap | Gdiplus::StringFormatFlagsMeasureTrailingSpaces);

    Gdiplus::RectF text_bounds;
    g.MeasureString(nocturne_text::stopped_label, -1, &font, Gdiplus::PointF(0, 0), &format, &text_bounds);

    const auto square = font_size * 0.62f;
    const auto gap = font_size * 0.5f;
    const auto pad_x = font_size * 1.2f;
    const auto pill_height = font_size * 1.25f + font_size * 0.94f;
    const auto pill_width = pad_x * 2 + square + gap + text_bounds.Width;
    const auto center_x = width / 2.f;
    const auto center_y = height / 2.f;
    const Gdiplus::RectF pill(center_x - pill_width / 2.f, center_y - pill_height / 2.f, pill_width, pill_height);

    Gdiplus::GraphicsPath path;
    add_round_rect(path, Gdiplus::RectF(pill.X + 0.5f, pill.Y + 0.5f, pill.Width - 1, pill.Height - 1), pill.Height / 2.f);
    const Gdiplus::SolidBrush background(rgba(224, 24, 25, 26));
    g.FillPath(&background, &path);
    const Gdiplus::Pen border(rgba(51, 255, 255, 255), 1.f);
    g.DrawPath(&border, &path);

    const Gdiplus::SolidBrush ink(rgba(255, 230, 230, 230));
    const auto square_x = pill.X + pad_x;
    g.FillRectangle(&ink, Gdiplus::RectF(square_x, center_y - square / 2.f, square, square));

    // GDI+ 畫字的 y 是字高頂端，基線在其下方 ascent 處；換算成讓字的視覺中心落在膠囊正中央
    const auto em_height = static_cast<float>(family->GetEmHeight(style));
    const auto ascent = font_size * family->GetCellAscent(style) / em_height;
    g.DrawString(nocturne_text::stopped_label, -1, &font,
        Gdiplus::PointF(square_x + square + gap, center_y + font_size * ink_center_above_baseline - ascent), &format,
        &ink);
}

// 32 位元 DIB，給 UpdateLayeredWindow 用（整張不透明，淡入淡出靠整體透明度）
class Surface {
public:
    bool ensure(int width, int height)
    {
        if (m_bitmap && width == m_width && height == m_height)
            return true;

        reset();

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        const auto screen_dc = wil::GetDC(nullptr);
        m_bitmap.reset(CreateDIBSection(screen_dc.get(), &info, DIB_RGB_COLORS, &m_bits, nullptr, 0));
        if (!m_bitmap)
            return false;

        m_dc.reset(CreateCompatibleDC(screen_dc.get()));
        m_old_bitmap = SelectObject(m_dc.get(), m_bitmap.get());
        m_width = width;
        m_height = height;
        return true;
    }

    void reset()
    {
        if (m_dc && m_old_bitmap)
            SelectObject(m_dc.get(), m_old_bitmap);
        m_old_bitmap = nullptr;
        m_dc.reset();
        m_bitmap.reset();
        m_bits = nullptr;
        m_width = m_height = 0;
    }

    // 把截下的展開頁（沒有就用底色）畫進來，整張變暗，再畫上膠囊
    void paint(HBITMAP page, double scale)
    {
        auto* pixels = static_cast<uint32_t*>(m_bits);
        const auto count = static_cast<size_t>(m_width) * m_height;

        if (page) {
            const wil::unique_hdc page_dc(CreateCompatibleDC(m_dc.get()));
            const auto _select_page = wil::SelectObject(page_dc.get(), page);
            BitBlt(m_dc.get(), 0, 0, m_width, m_height, page_dc.get(), 0, 0, SRCCOPY);
            // GDI 的繪圖可能還在排隊，要先寫進記憶體才能直接讀寫像素
            GdiFlush();

            constexpr uint32_t keep = 100 - dim_percent;
            constexpr uint32_t add = dim_color * dim_percent;
            for (size_t i = 0; i < count; i++) {
                const auto p = pixels[i];
                const uint32_t r = ((p >> 16 & 0xFF) * keep + add) / 100;
                const uint32_t g = ((p >> 8 & 0xFF) * keep + add) / 100;
                const uint32_t b = ((p & 0xFF) * keep + add) / 100;
                pixels[i] = 0xFF000000u | r << 16 | g << 8 | b;
            }
        } else {
            const auto gray = 0xFF000000u | uint32_t{fallback_gray} << 16 | uint32_t{fallback_gray} << 8 | fallback_gray;
            std::fill(pixels, pixels + count, gray);
        }

        Gdiplus::Bitmap canvas(m_width, m_height, m_width * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(m_bits));
        draw_label(canvas, m_width, m_height, scale);
    }

    HDC dc() const { return m_dc.get(); }
    int width() const { return m_width; }
    int height() const { return m_height; }

    ~Surface() { reset(); }

private:
    wil::unique_hbitmap m_bitmap;
    wil::unique_hdc m_dc;
    HGDIOBJ m_old_bitmap{};
    void* m_bits{};
    int m_width{};
    int m_height{};
};

LRESULT CALLBACK page_wndproc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_NCHITTEST:
        // 不擋滑鼠，點到的是底下的展開頁
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return TRUE;
    }
    return DefWindowProc(wnd, msg, wp, lp);
}

// 餘弦緩動：起步與收尾都很緩，跟下一首提示一樣
double ease(double progress)
{
    constexpr double pi = 3.14159265358979323846;
    progress = std::clamp(progress, 0.0, 1.0);
    return 0.5 - 0.5 * std::cos(progress * pi);
}

class Controller {
public:
    void start();
    void stop();
    void tick();
    void on_new_track();

private:
    void capture_if_needed(const RECT& stage);
    bool ensure_window(HWND owner, const RECT& stage, double scale);
    void destroy_window();
    void present(const RECT& stage);
    void set_interval(UINT ms);

    HWND m_timer_wnd{};
    HWND m_wnd{};
    UINT m_interval{};
    // 淡入淡出期間是否已把系統計時精度調成 1 ms（要成對還原）
    bool m_high_resolution_timer{};
    double m_alpha{};
    Clock::time_point m_last_tick{Clock::now()};
    Surface m_surface;

    // 最後一次播放中截下的展開頁（原圖，還沒變暗）、大小，以及這一首已經截了第幾次（0、1、2）
    wil::unique_hbitmap m_page;
    SIZE m_page_size{};
    int m_page_pass{};
};

Controller& controller()
{
    static Controller instance;
    return instance;
}

LRESULT CALLBACK timer_wndproc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_TIMER && wp == timer_id && g_alive) {
        try {
            controller().tick();
        }
        CATCH_LOG()
        return 0;
    }
    return DefWindowProc(wnd, msg, wp, lp);
}

void Controller::start()
{
    static const auto classes_registered = [] {
        WNDCLASSEX timer_class{};
        timer_class.cbSize = sizeof(timer_class);
        timer_class.lpfnWndProc = timer_wndproc;
        timer_class.hInstance = core_api::get_my_instance();
        timer_class.lpszClassName = timer_class_name;

        WNDCLASSEX page_class{};
        page_class.cbSize = sizeof(page_class);
        page_class.lpfnWndProc = page_wndproc;
        page_class.hInstance = core_api::get_my_instance();
        page_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
        page_class.lpszClassName = page_class_name;

        return RegisterClassEx(&timer_class) != 0 && RegisterClassEx(&page_class) != 0;
    }();

    if (!classes_registered)
        return;

    m_timer_wnd = CreateWindowEx(
        0, timer_class_name, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, core_api::get_my_instance(), nullptr);
    set_interval(interval_idle_ms);
}

void Controller::stop()
{
    if (m_timer_wnd) {
        KillTimer(m_timer_wnd, timer_id);
        DestroyWindow(m_timer_wnd);
        m_timer_wnd = nullptr;
    }

    destroy_window();
    m_page.reset();

    if (m_high_resolution_timer) {
        timeEndPeriod(1);
        m_high_resolution_timer = false;
    }
}

void Controller::set_interval(UINT ms)
{
    if (!m_timer_wnd)
        return;

    // 只有淡入淡出的期間才把系統計時精度調成 1 ms，閒置時還原（理由同下一首提示）
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
    SetTimer(m_timer_wnd, timer_id, ms, nullptr);
}

// 換曲了：手上的圖是上一首的展開頁，不能拿來當這一首的最後一頁
void Controller::on_new_track()
{
    m_page.reset();
    m_page_size = {};
    m_page_pass = 0;
}

void Controller::destroy_window()
{
    if (m_wnd) {
        DestroyWindow(m_wnd);
        m_wnd = nullptr;
    }
    m_surface.reset();
    m_alpha = 0;
}

// 播放中且展開頁已經定下來時，照時間點截圖；一首歌最多兩次，視窗大小變了再補一次
void Controller::capture_if_needed(const RECT& stage)
{
    const auto playback = playback_control::get();
    const auto position = playback->playback_get_position();
    const auto want_pass = position >= second_capture_seconds ? 2 : position >= first_capture_seconds ? 1 : 0;
    const SIZE size{wil::rect_width(stage), wil::rect_height(stage)};
    const auto size_changed = m_page_pass > 0 && (size.cx != m_page_size.cx || size.cy != m_page_size.cy);

    if (want_pass == 0 || (want_pass <= m_page_pass && !size_changed))
        return;

    RECT screen_rect{};
    HWND owner{};
    auto page = FlatSplitterPanel::s_capture_now_playing_stage(screen_rect, owner);
    if (!page)
        return;

    m_page = std::move(page);
    m_page_size = {wil::rect_width(screen_rect), wil::rect_height(screen_rect)};
    m_page_pass = want_pass;
}

bool Controller::ensure_window(HWND owner, const RECT& stage, double scale)
{
    const auto width = wil::rect_width(stage);
    const auto height = wil::rect_height(stage);

    // 展開頁大小變了（拖視窗邊緣）：舊的畫面對不上，收掉重建
    if (m_wnd && (m_surface.width() != width || m_surface.height() != height))
        destroy_window();

    if (m_wnd)
        return true;

    if (!m_surface.ensure(width, height))
        return false;

    const auto use_page = m_page && m_page_size.cx == width && m_page_size.cy == height;
    m_surface.paint(use_page ? m_page.get() : nullptr, scale);

    m_wnd = CreateWindowEx(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, page_class_name,
        L"", WS_POPUP, stage.left, stage.top, width, height, owner, nullptr, core_api::get_my_instance(), nullptr);

    if (!m_wnd) {
        m_surface.reset();
        return false;
    }

    BOOL disable_transitions = TRUE;
    DwmSetWindowAttribute(m_wnd, DWMWA_TRANSITIONS_FORCEDISABLED, &disable_transitions, sizeof(disable_transitions));
    m_alpha = 0;
    return true;
}

void Controller::present(const RECT& stage)
{
    POINT origin{stage.left, stage.top};
    SIZE size{m_surface.width(), m_surface.height()};
    POINT source{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, static_cast<BYTE>(std::lround(ease(m_alpha) * 255)), 0};

    UpdateLayeredWindow(m_wnd, nullptr, &origin, &size, m_surface.dc(), &source, 0, &blend, ULW_ALPHA);

    if (!IsWindowVisible(m_wnd)) {
        ShowWindow(m_wnd, SW_SHOWNOACTIVATE);
        // 放到同一個主視窗底下所有浮動視窗的最下面（仍在主視窗之上），角落的提示與動畫截圖才不會被它蓋住
        SetWindowPos(m_wnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void Controller::tick()
{
    const auto now = Clock::now();
    const auto elapsed = std::min(std::chrono::duration<double>(now - m_last_tick).count(), max_step_seconds);
    m_last_tick = now;

    RECT stage{};
    HWND owner{};

    // 設定關掉、展開頁收合（含動畫中）、主視窗縮小：不淡出，直接收掉
    if (!next_track_toast::enabled.get() || !FlatSplitterPanel::s_get_now_playing_stage(stage, owner)
        || IsIconic(owner) || !IsWindowVisible(owner)) {
        if (m_wnd)
            destroy_window();
        set_interval(interval_idle_ms);
        return;
    }

    const auto playback = playback_control::get();
    bool want_visible = false;

    if (playback->is_playing())
        capture_if_needed(stage);
    else
        want_visible = playlist_manager::get()->queue_get_count() > 0;

    if (!want_visible && !m_wnd) {
        set_interval(interval_idle_ms);
        return;
    }

    const auto dpi = std::max<UINT>(GetDpiForWindow(owner), USER_DEFAULT_SCREEN_DPI);
    const auto scale = static_cast<double>(dpi) / USER_DEFAULT_SCREEN_DPI;

    if (want_visible) {
        // 第一次出現的這一格要截圖、變暗、畫字，會花一段時間；這一格先停在全透明，時間從下一格才開始算
        const auto creating = !m_wnd;
        if (!ensure_window(owner, stage, scale))
            return;

        if (!creating)
            m_alpha = std::min(1.0, m_alpha + elapsed / fade_in_seconds);
        else
            m_last_tick = Clock::now();
    } else {
        m_alpha = std::max(0.0, m_alpha - elapsed / fade_out_seconds);

        if (m_alpha <= 0) {
            destroy_window();
            set_interval(interval_idle_ms);
            return;
        }
    }

    present(stage);
    set_interval(interval_active_ms);
}

class StoppedPageInitQuit : public initquit {
public:
    void on_init() override
    {
        g_alive = true;
        controller().start();
    }

    void on_quit() override
    {
        g_alive = false;
        controller().stop();
    }
};

initquit_factory_t<StoppedPageInitQuit> stopped_page_initquit;

class NewTrackCallback : public play_callback_static {
public:
    unsigned get_flags() override { return flag_on_playback_new_track; }

    void on_playback_new_track(metadb_handle_ptr track) override
    {
        if (g_alive)
            controller().on_new_track();
    }

    void on_playback_starting(play_control::t_track_command command, bool paused) override {}
    void on_playback_stop(play_control::t_stop_reason reason) override {}
    void on_playback_seek(double time) override {}
    void on_playback_pause(bool state) override {}
    void on_playback_edited(metadb_handle_ptr track) override {}
    void on_playback_dynamic_info(const file_info& info) override {}
    void on_playback_dynamic_info_track(const file_info& info) override {}
    void on_playback_time(double time) override {}
    void on_volume_change(float new_value) override {}
};

play_callback_static_factory_t<NewTrackCallback> new_track_callback;

} // namespace

} // namespace cui::stopped_page
