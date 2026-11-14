#include "pch.h"

#include "nocturne_segment.h"

#include "next_track_toast.h"

#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")

#if __has_include("nocturne_build_info.h")
#include "nocturne_build_info.h"
#endif

// 右鍵挑選卡共用的分段切換，說明見 nocturne_segment.h

namespace cui::nocturne_segment {

namespace {

bool g_animations = true;
float g_slow = 1.f;

float ease_out(float t)
{
    const auto u = 1.f - t;
    return 1.f - u * u * u;
}

float ms_since(Clock::time_point start, Clock::time_point now)
{
    return std::chrono::duration<float, std::milli>(now - start).count();
}

} // namespace

POINT place_card(HWND owner, POINT prefer, LONG flip_bottom, const Gdiplus::RectF& card)
{
    const auto card_w = static_cast<LONG>(std::lround(card.Width));
    const auto card_h = static_cast<LONG>(std::lround(card.Height));

    // 範圍：螢幕工作區，再跟 fb2k 主視窗的內容區取交集
    RECT bounds{};
    MONITORINFO monitor{sizeof(monitor)};
    const bool has_monitor = GetMonitorInfo(MonitorFromPoint(prefer, MONITOR_DEFAULTTONEAREST), &monitor) != 0;
    if (has_monitor)
        bounds = monitor.rcWork;
    const auto root = owner ? GetAncestor(owner, GA_ROOT) : nullptr;
    RECT client{};
    if (root && GetClientRect(root, &client)) {
        MapWindowPoints(root, HWND_DESKTOP, reinterpret_cast<LPPOINT>(&client), 2);
        RECT both{};
        if (!has_monitor)
            bounds = client;
        else if (IntersectRect(&both, &bounds, &client))
            bounds = both;
    }

    auto x = prefer.x;
    auto y = prefer.y;
    if (y + card_h > bounds.bottom)
        y = flip_bottom - card_h;
    x = std::clamp(x, bounds.left, std::max(bounds.left, bounds.right - card_w));
    y = std::clamp(y, bounds.top, std::max(bounds.top, bounds.bottom - card_h));
    return {x - static_cast<LONG>(std::lround(card.X)), y - static_cast<LONG>(std::lround(card.Y))};
}

void refresh_settings()
{
    BOOL enabled = TRUE;
    if (!SystemParametersInfo(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0))
        enabled = TRUE;
    g_animations = enabled != FALSE;

    g_slow = 1.f;
#if defined(NOCTURNE_BUILD_DEV)
    // 開發版：環境變數 NOCTURNE_ANIM_SLOW＝倍數（例如 5），測試時放慢好截中間格
    wchar_t value[16]{};
    if (GetEnvironmentVariable(L"NOCTURNE_ANIM_SLOW", value, static_cast<DWORD>(std::size(value))) > 0) {
        const auto factor = static_cast<float>(_wtof(value));
        if (factor >= 1.f && factor <= 50.f)
            g_slow = factor;
    }
#endif
}

bool animations_enabled()
{
    return g_animations;
}

float slow_factor()
{
    return g_slow;
}

// ---- Tween ----

void Tween::set(float value)
{
    m_from = m_to = m_value = value;
    m_running = false;
}

void Tween::go(float target, float duration_ms)
{
    if (!g_animations || duration_ms <= 0.f) {
        set(target);
        return;
    }
    if (m_running && target == m_to)
        return;
    if (!m_running && target == m_value)
        return;
    m_from = m_value;
    m_to = target;
    m_duration = duration_ms * g_slow;
    m_start = Clock::now();
    m_running = true;
}

bool Tween::update(Clock::time_point now)
{
    if (!m_running)
        return false;
    const auto t = std::clamp(ms_since(m_start, now) / m_duration, 0.f, 1.f);
    m_value = m_from + (m_to - m_from) * ease_out(t);
    if (t >= 1.f) {
        m_value = m_to;
        m_running = false;
    }
    return m_running;
}

// ---- 共用畫圖 ----

Gdiplus::Color ink(double alpha)
{
    return Gdiplus::Color(static_cast<BYTE>(std::clamp(alpha, 0.0, 1.0) * 255.0 + 0.5), 244, 240, 230);
}

std::unique_ptr<Gdiplus::FontFamily> theme_family()
{
    if (next_track_toast::gdiplus_fonts_ready()) {
        const auto log_font = fonts::get_log_font_with_fallback(fonts::labels_font_id);
        auto family = std::make_unique<Gdiplus::FontFamily>(log_font.lfFaceName);
        if (family->GetLastStatus() == Gdiplus::Ok)
            return family;
    }
    return std::unique_ptr<Gdiplus::FontFamily>(Gdiplus::FontFamily::GenericSansSerif()->Clone());
}

Gdiplus::FontStyle regular_style(const Gdiplus::FontFamily& family)
{
    if (family.IsStyleAvailable(Gdiplus::FontStyleRegular))
        return Gdiplus::FontStyleRegular;
    return family.IsStyleAvailable(Gdiplus::FontStyleBold) ? Gdiplus::FontStyleBold : Gdiplus::FontStyleItalic;
}

void add_rounded(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& rect, float radius)
{
    radius = std::min({radius, rect.Width / 2.f, rect.Height / 2.f});
    if (radius <= 0.f) {
        path.AddRectangle(rect);
        return;
    }
    const auto d = radius * 2.f;
    path.AddArc(rect.X, rect.Y, d, d, 180, 90);
    path.AddArc(rect.GetRight() - d, rect.Y, d, d, 270, 90);
    path.AddArc(rect.GetRight() - d, rect.GetBottom() - d, d, d, 0, 90);
    path.AddArc(rect.X, rect.GetBottom() - d, d, d, 90, 90);
    path.CloseFigure();
}

namespace {

void draw_aligned(Gdiplus::Graphics& g, const std::wstring& text, const Gdiplus::Font& font, const Gdiplus::Color& colour,
    const Gdiplus::RectF& box, Gdiplus::StringAlignment align)
{
    Gdiplus::StringFormat format;
    format.SetAlignment(align);
    format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    const Gdiplus::SolidBrush brush(colour);
    g.DrawString(text.c_str(), static_cast<INT>(text.size()), &font, box, &format, &brush);
}

} // namespace

void draw_centered(Gdiplus::Graphics& g, const std::wstring& text, const Gdiplus::Font& font, const Gdiplus::Color& colour,
    const Gdiplus::RectF& box)
{
    draw_aligned(g, text, font, colour, box, Gdiplus::StringAlignmentCenter);
}

void draw_left(Gdiplus::Graphics& g, const std::wstring& text, const Gdiplus::Font& font, const Gdiplus::Color& colour,
    const Gdiplus::RectF& box)
{
    draw_aligned(g, text, font, colour, box, Gdiplus::StringAlignmentNear);
}

// ---- 分段 ----

void layout(Row& row, const Gdiplus::RectF& track, float k)
{
    row.track = track;
    row.cells.clear();
    const auto count = static_cast<int>(row.labels.size());
    if (count <= 0)
        return;
    const auto pad = track_pad_dip * k;
    const auto gap = cell_gap_dip * k;
    const auto inner_w = track.Width - 2 * pad;
    const auto cell_w = (inner_w - (count - 1) * gap) / count;
    for (int i = 0; i < count; ++i)
        row.cells.push_back({track.X + pad + i * (cell_w + gap), track.Y + pad, cell_w, track.Height - 2 * pad});
    row.selected = std::clamp(row.selected, 0, count - 1);
    row.shown = row.selected;
    row.pill.set(row.cells[row.selected].X);
}

int cell_at(const Row& row, float x, float y)
{
    if (x < row.track.X || x >= row.track.GetRight() || y < row.track.Y || y >= row.track.GetBottom())
        return -1;
    // 段與段之間的空隙算給比較近的那段
    int best = -1;
    float best_distance = std::numeric_limits<float>::max();
    for (int i = 0; i < static_cast<int>(row.cells.size()); ++i) {
        const auto& c = row.cells[i];
        const auto distance = x < c.X ? c.X - x : (x >= c.GetRight() ? x - c.GetRight() : 0.f);
        if (distance < best_distance) {
            best_distance = distance;
            best = i;
        }
    }
    return best;
}

void draw(Gdiplus::Graphics& g, const Row& row, const Gdiplus::Font& font, float k)
{
    if (row.cells.empty())
        return;

    {
        Gdiplus::GraphicsPath path;
        add_rounded(path, row.track, track_radius_dip * k);
        const Gdiplus::SolidBrush brush(Gdiplus::Color(15, 255, 255, 255));
        g.FillPath(&brush, &path);
    }
    {
        const auto& first = row.cells.front();
        const Gdiplus::RectF pill{row.pill.value(), first.Y, first.Width, first.Height};
        Gdiplus::GraphicsPath path;
        add_rounded(path, pill, pill_radius_dip * k);
        const Gdiplus::SolidBrush brush(Gdiplus::Color(row.dragging ? 51 : 36, 255, 255, 255));
        g.FillPath(&brush, &path);
    }
    for (int i = 0; i < static_cast<int>(row.cells.size()); ++i) {
        const auto alpha = i == row.shown ? 0.97 : (i == row.hover && !row.dragging ? 0.9 : 0.58);
        draw_centered(g, row.labels[i], font, ink(alpha), row.cells[i]);
    }
}

void select(Row& row, int index, bool animate)
{
    if (row.cells.empty())
        return;
    index = std::clamp(index, 0, static_cast<int>(row.cells.size()) - 1);
    row.selected = index;
    row.shown = index;
    if (animate)
        row.pill.go(row.cells[index].X, pill_ms);
    else
        row.pill.set(row.cells[index].X);
}

bool update(Row& row, Clock::time_point now)
{
    return row.pill.update(now);
}

bool mouse_down(Row& row, float x, float y)
{
    if (cell_at(row, x, y) < 0)
        return false;
    row.pressed = true;
    row.dragging = false;
    row.press_x = x;
    row.press_y = y;
    return true;
}

void mouse_move(Row& row, float x, float k, const std::function<void(int)>& on_preview)
{
    if (!row.pressed || row.cells.empty())
        return;
    const auto& first = row.cells.front();
    if (!row.dragging) {
        if (std::abs(x - row.press_x) < drag_threshold_dip * k)
            return;
        row.dragging = true;
        // 從亮底上按下就保持抓住的位置；從別段按下就讓亮底中心對齊游標
        const auto left = row.pill.value();
        row.grab = row.press_x >= left && row.press_x < left + first.Width ? row.press_x - left : first.Width / 2.f;
    }
    // 亮底只在第一段到最後一段之間移動（2026-11-15 使用者：拖到邊界還會往外跑一截、超出軌道不好看；
    // 原本拉出兩端照四分之一的阻力繼續走、放開再彈回）
    const auto left = std::clamp(x - row.grab, first.X, row.cells.back().X);
    row.pill.set(left);

    const auto centre = left + first.Width / 2.f;
    int nearest = 0;
    float best = std::numeric_limits<float>::max();
    for (int i = 0; i < static_cast<int>(row.cells.size()); ++i) {
        const auto distance = std::abs(row.cells[i].X + row.cells[i].Width / 2.f - centre);
        if (distance < best) {
            best = distance;
            nearest = i;
        }
    }
    if (nearest != row.shown) {
        row.shown = nearest;
        if (on_preview)
            on_preview(nearest);
    }
}

void mouse_up(Row& row, const std::function<void(int)>& on_commit)
{
    if (!row.pressed)
        return;
    row.pressed = false;
    int target = -1;
    if (row.dragging) {
        row.dragging = false;
        target = row.shown;
    } else {
        target = cell_at(row, row.press_x, row.press_y);
    }
    if (target < 0)
        return;
    const auto changed = target != row.selected;
    select(row, target, true);
    if (changed && on_commit)
        on_commit(target);
}

void cancel_drag(Row& row)
{
    if (!row.pressed)
        return;
    row.pressed = false;
    row.dragging = false;
    select(row, row.selected, true);
}

// ---- 說明行 ----

void caption_set(Caption& caption, const std::wstring& text, int dir, bool animate)
{
    if (text == caption.new_text && (caption.t.running() || caption.t.value() >= 1.f))
        return;
    if (!animate || !g_animations || caption.new_text.empty()) {
        caption.old_text.clear();
        caption.new_text = text;
        caption.t.set(1.f);
        return;
    }
    // 舊字還沒淡完又換：目前看得比較清楚的那個當舊字
    caption.old_text = caption.t.value() >= 0.5f ? caption.new_text : caption.old_text;
    caption.new_text = text;
    caption.dir = dir >= 0 ? 1 : -1;
    caption.t.set(0.f);
    caption.t.go(1.f, caption_ms);
}

void draw_caption(Gdiplus::Graphics& g, const Caption& caption, const Gdiplus::RectF& box, const Gdiplus::Font& font,
    double alpha, float k)
{
    const auto t = caption.t.value();
    const auto shift = caption_shift_dip * k * caption.dir;
    const auto state = g.Save();
    g.SetClip(box);
    if (t < 1.f && !caption.old_text.empty()) {
        Gdiplus::RectF old_box = box;
        old_box.X -= shift * t;
        draw_centered(g, caption.old_text, font, ink(alpha * (1.f - t)), old_box);
    }
    Gdiplus::RectF new_box = box;
    new_box.X += shift * (1.f - t);
    draw_centered(g, caption.new_text, font, ink(alpha * t), new_box);
    g.Restore(state);
}

bool caption_update(Caption& caption, Clock::time_point now)
{
    return caption.t.update(now);
}

// ---- 縮圖底板 ----

void plate_place(Plate& plate, const Gdiplus::RectF& cell, bool animate)
{
    if (animate) {
        plate.x.go(cell.X, pill_ms);
        plate.y.go(cell.Y, pill_ms);
    } else {
        plate.x.set(cell.X);
        plate.y.set(cell.Y);
    }
}

bool plate_update(Plate& plate, Clock::time_point now)
{
    const auto x = plate.x.update(now);
    const auto y = plate.y.update(now);
    return x || y;
}

// ---- 卡片開關 ----

void Presenter::begin_open()
{
    m_closing = false;
    m_alpha.set(g_animations ? 0.f : 1.f);
    m_scale.set(g_animations ? open_scale_from : 1.f);
    m_alpha.go(1.f, open_ms);
    m_scale.go(1.f, open_ms);
}

void Presenter::begin_close()
{
    if (m_closing)
        return;
    m_closing = true;
    m_scale.set(1.f);
    m_alpha.go(0.f, close_ms);
}

bool Presenter::update(Clock::time_point now)
{
    const auto a = m_alpha.update(now);
    const auto s = m_scale.update(now);
    return a || s;
}

void Presenter::present(HWND wnd, const uint32_t* pixels, int width, int height, Gdiplus::PointF anchor)
{
    if (!wnd || width <= 0 || height <= 0)
        return;

    const wil::unique_hdc_window screen_dc(GetDC(nullptr));
    if (!m_dc || width != m_width || height != m_height) {
        if (m_dc && m_old_bitmap)
            SelectObject(m_dc.get(), m_old_bitmap);
        m_bitmap.reset();
        m_dc.reset(CreateCompatibleDC(screen_dc.get()));
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        m_bits = nullptr;
        m_bitmap.reset(CreateDIBSection(screen_dc.get(), &info, DIB_RGB_COLORS, &m_bits, nullptr, 0));
        if (!m_dc || !m_bitmap || !m_bits) {
            m_dc.reset();
            m_bitmap.reset();
            m_bits = nullptr;
            return;
        }
        m_old_bitmap = SelectObject(m_dc.get(), m_bitmap.get());
        m_width = width;
        m_height = height;
    }
    void* bits = m_bits;

    const auto bytes = static_cast<size_t>(width) * height * 4;
    const auto scale = m_scale.value();
    if (scale >= 0.9999f) {
        memcpy(bits, pixels, bytes);
    } else {
        // 開卡動畫：以卡片左上角為中心縮小貼上
        memset(bits, 0, bytes);
        Gdiplus::Bitmap target(width, height, width * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(bits));
        Gdiplus::Bitmap source(width, height, width * 4, PixelFormat32bppPARGB,
            reinterpret_cast<BYTE*>(const_cast<uint32_t*>(pixels)));
        Gdiplus::Graphics g(&target);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBilinear);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        g.TranslateTransform(anchor.X, anchor.Y);
        g.ScaleTransform(scale, scale);
        g.TranslateTransform(-anchor.X, -anchor.Y);
        g.DrawImage(&source, 0, 0, width, height);
    }

    GdiFlush();
    SIZE size{width, height};
    POINT source_point{0, 0};
    const auto alpha = static_cast<BYTE>(std::lround(std::clamp(m_alpha.value(), 0.f, 1.f) * 255.f));
    BLENDFUNCTION blend{AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
    UpdateLayeredWindow(wnd, screen_dc.get(), nullptr, &size, m_dc.get(), &source_point, 0, &blend, ULW_ALPHA);
}

// ---- 動畫時脈 ----

UINT FrameClock::message()
{
    static const UINT id = RegisterWindowMessage(L"Nocturne.SegmentFrame");
    return id;
}

void FrameClock::ensure(HWND wnd)
{
    if (m_running || !wnd)
        return;
    if (m_thread.joinable())
        m_thread.join();
    m_wnd = wnd;
    m_pending = false;
    m_running = true;
    m_thread = std::thread([this] {
        const auto msg = message();
        while (m_running) {
            // 等到下一次畫面合成；DWM 不能用時退回約 4 ms
            if (FAILED(DwmFlush()))
                Sleep(4);
            if (!m_running)
                break;
            if (!m_pending.exchange(true))
                PostMessage(m_wnd, msg, 0, 0);
        }
    });
}

void FrameClock::stop()
{
    m_running = false;
    if (m_thread.joinable())
        m_thread.join();
}

} // namespace cui::nocturne_segment
