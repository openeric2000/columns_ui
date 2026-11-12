#include "pch.h"

#include "nocturne_tooltip.h"

#include "font_utils.h"
#include "nocturne_surface.h"

// 自編版新增（2026-11-12）：ESLyric 播放列與展開頁上的滑鼠提示改成自己畫（跟其他浮動元件同一套外觀，nocturne_surface.h）
// - ESLyric 用 Windows 原生提示（tooltips_class32，白底方框），它是第三方元件改不到，所以在 fb2k 行程裡攔：
//   原生提示視窗一建立就掛子類別；它要顯示的那一刻問「目前這個提示是哪個視窗的」，是 ESLyric 的就把原生視窗
//   設成空區域（看不見、滑鼠也穿過去，但它自己的顯示／隱藏邏輯照走），在同一個位置畫我們自己的提示；原生提示隱藏時跟著收
// - 所有原生提示視窗的擁有者都是 fb2k 主視窗，分不出是誰的，只能在要顯示時看「目前的工具」屬於哪個視窗
// - 其他元件（Columns UI 自己的播放清單、工具列等）的提示不動
// - 文字用 Columns UI「Common (labels)」字型

namespace cui::nocturne_tooltip {

namespace {

constexpr wchar_t window_class_name[] = L"columns_ui_nocturne_tooltip";
constexpr wchar_t native_class_name[] = L"tooltips_class32";
constexpr wchar_t eslyric_class_name[] = L"uie_eslyric_wnd_class";
constexpr UINT_PTR subclass_id = 0x4e7c;
constexpr UINT_PTR fade_timer_id = 1;
// 網頁提示的保險：每 150 ms 看一次游標還在不在元件上，離開就收（網頁的「滑鼠離開」萬一沒送到也不會殘留）
constexpr UINT_PTR watch_timer_id = 2;
constexpr UINT watch_interval_ms = 150;
// 網頁提示跟元件下緣的距離（DIP）
constexpr float below_gap_dip = 6.f;
// 網頁提示用的「擁有者」記號（不是真的視窗）
const auto web_owner = reinterpret_cast<HWND>(static_cast<intptr_t>(-1));
constexpr UINT fade_frame_ms = 16;
constexpr double fade_in_ms = 120;
constexpr float pad_x_dip = 10.f;
constexpr float pad_y_dip = 4.f;
constexpr float font_dip = 13.f;
constexpr float max_width_dip = 420.f;
constexpr int shadow_dip = 6;

HWINEVENTHOOK g_hook{};
bool g_alive{};

bool has_class(HWND wnd, const wchar_t* name)
{
    wchar_t buffer[64]{};
    return wnd && GetClassName(wnd, buffer, static_cast<int>(std::size(buffer))) && wcscmp(buffer, name) == 0;
}

// 這個視窗（或它的上層）是不是 ESLyric 面板
bool belongs_to_eslyric(HWND wnd)
{
    for (auto current = wnd; current; current = GetParent(current)) {
        if (has_class(current, eslyric_class_name))
            return true;
    }
    return false;
}

// 原生提示「目前的工具」是 ESLyric 的就回傳它的文字
std::optional<std::wstring> eslyric_tip_text(HWND native)
{
    TTTOOLINFOW info{};
    info.cbSize = sizeof(info);
    if (!SendMessage(native, TTM_GETCURRENTTOOLW, 0, reinterpret_cast<LPARAM>(&info)))
        return {};
    if (!belongs_to_eslyric(info.hwnd))
        return {};

    std::array<wchar_t, 1024> text{};
    info.lpszText = text.data();
    SendMessage(native, TTM_GETTEXTW, text.size(), reinterpret_cast<LPARAM>(&info));
    std::wstring result(text.data());
    if (result.empty())
        return {};
    return result;
}

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

class TipWindow {
public:
    // owner：原生提示視窗（ESLyric），或 web_owner（網頁）。body_origin＝提示本體左上角（螢幕座標）
    void show(HWND owner, const std::wstring& text, POINT body_origin, UINT dpi);
    void show_below(const RECT& anchor, const std::wstring& text, UINT dpi);
    void move(HWND owner, POINT body_origin);
    void hide(HWND owner);
    void destroy();
    void on_timer(UINT_PTR id);

private:
    bool ensure_window();
    bool render(const std::wstring& text, UINT dpi);
    void appear(HWND owner, POINT body_origin);
    void present(BYTE alpha, bool with_content);

    HWND m_wnd{};
    HWND m_native{};
    wil::unique_hbitmap m_bitmap;
    wil::unique_hdc m_dc;
    HGDIOBJ m_old_bitmap{};
    SIZE m_size{};
    SIZE m_body{};
    int m_shadow{};
    POINT m_origin{};
    RECT m_anchor{};
    std::chrono::steady_clock::time_point m_shown_at;
};

TipWindow& tip()
{
    static TipWindow instance;
    return instance;
}

LRESULT CALLBACK tip_wndproc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_TIMER:
        if (g_alive)
            tip().on_timer(wp);
        return 0;
    }
    return DefWindowProc(wnd, msg, wp, lp);
}

bool TipWindow::ensure_window()
{
    if (m_wnd)
        return true;

    static const auto registered = [] {
        WNDCLASSEX wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = tip_wndproc;
        wc.hInstance = core_api::get_my_instance();
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = window_class_name;
        return RegisterClassEx(&wc) != 0;
    }();
    if (!registered)
        return false;

    m_wnd = CreateWindowEx(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_TOPMOST,
        window_class_name, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, core_api::get_my_instance(), nullptr);
    if (!m_wnd)
        return false;

    BOOL disable_transitions = TRUE;
    DwmSetWindowAttribute(m_wnd, DWMWA_TRANSITIONS_FORCEDISABLED, &disable_transitions, sizeof(disable_transitions));
    return true;
}

void TipWindow::show(HWND owner, const std::wstring& text, POINT body_origin, UINT dpi)
{
    if (!render(text, dpi))
        return;
    appear(owner, body_origin);
}

// 網頁元件的提示：放在元件正下方、水平置中
void TipWindow::show_below(const RECT& anchor, const std::wstring& text, UINT dpi)
{
    if (!render(text, dpi))
        return;
    const auto k = static_cast<float>(std::max<UINT>(dpi, USER_DEFAULT_SCREEN_DPI)) / USER_DEFAULT_SCREEN_DPI;
    m_anchor = anchor;
    const POINT origin{(anchor.left + anchor.right) / 2 - m_body.cx / 2,
        anchor.bottom + static_cast<int>(std::lround(below_gap_dip * k))};
    appear(web_owner, origin);
    SetTimer(m_wnd, watch_timer_id, watch_interval_ms, nullptr);
}

void TipWindow::appear(HWND owner, POINT body_origin)
{
    KillTimer(m_wnd, watch_timer_id);
    m_native = owner;
    move(owner, body_origin);
    m_shown_at = std::chrono::steady_clock::now();
    present(0, true);
    ShowWindow(m_wnd, SW_SHOWNOACTIVATE);
    SetWindowPos(m_wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetTimer(m_wnd, fade_timer_id, fade_frame_ms, nullptr);
}

// 量字、建好畫面（還沒顯示）
bool TipWindow::render(const std::wstring& text, UINT dpi)
{
    if (!ensure_window())
        return false;

    dpi = std::max<UINT>(dpi, USER_DEFAULT_SCREEN_DPI);
    const auto k = static_cast<float>(dpi) / USER_DEFAULT_SCREEN_DPI;

    const auto family = create_font_family();
    const Gdiplus::Font font(family.get(), font_dip * k, pick_font_style(*family), Gdiplus::UnitPixel);
    Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
    format.SetFormatFlags(Gdiplus::StringFormatFlagsMeasureTrailingSpaces);

    Gdiplus::RectF measured;
    {
        const wil::unique_hdc_window screen(GetDC(nullptr));
        Gdiplus::Graphics measure(screen.get());
        measure.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
        measure.MeasureString(text.c_str(), -1, &font, Gdiplus::RectF(0, 0, max_width_dip * k, 10000.f), &format, &measured);
    }

    const auto pad_x = pad_x_dip * k;
    const auto pad_y = pad_y_dip * k;
    m_shadow = static_cast<int>(std::lround(shadow_dip * k));
    const auto body_w = static_cast<int>(std::ceil(measured.Width + 2 * pad_x));
    const auto body_h = static_cast<int>(std::ceil(measured.Height + 2 * pad_y));
    m_body = {body_w, body_h};
    m_size = {body_w + 2 * m_shadow, body_h + 2 * m_shadow};

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = m_size.cx;
    info.bmiHeader.biHeight = -m_size.cy;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    if (m_dc && m_old_bitmap)
        SelectObject(m_dc.get(), m_old_bitmap);
    m_old_bitmap = nullptr;
    m_bitmap.reset();
    m_dc.reset();

    const wil::unique_hdc_window screen_dc(GetDC(nullptr));
    void* bits{};
    m_bitmap.reset(CreateDIBSection(screen_dc.get(), &info, DIB_RGB_COLORS, &bits, nullptr, 0));
    if (!m_bitmap || !bits)
        return false;
    m_dc.reset(CreateCompatibleDC(screen_dc.get()));
    m_old_bitmap = SelectObject(m_dc.get(), m_bitmap.get());

    {
        Gdiplus::Bitmap canvas(m_size.cx, m_size.cy, m_size.cx * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(bits));
        Gdiplus::Graphics g(&canvas);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));

        const Gdiplus::RectF body(static_cast<float>(m_shadow), static_cast<float>(m_shadow), static_cast<float>(body_w),
            static_cast<float>(body_h));
        nocturne_surface::paint_panel(g, body, nocturne_surface::tip_radius_dip * k, m_shadow);

        const Gdiplus::SolidBrush ink(Gdiplus::Color(255, nocturne_surface::text_bright, nocturne_surface::text_bright,
            nocturne_surface::text_bright));
        g.DrawString(text.c_str(), -1, &font,
            Gdiplus::RectF(body.X + pad_x, body.Y + pad_y, measured.Width + 1.f, measured.Height + 1.f), &format, &ink);
    }
    return true;
}

// 原生提示放在哪，我們的提示本體就放在哪（四周多出的是陰影）；超出螢幕工作區就往內推
void TipWindow::move(HWND owner, POINT body_origin)
{
    if (!m_wnd || owner != m_native)
        return;

    POINT origin{body_origin.x - m_shadow, body_origin.y - m_shadow};
    MONITORINFO monitor{sizeof(monitor)};
    if (GetMonitorInfo(MonitorFromPoint(body_origin, MONITOR_DEFAULTTONEAREST), &monitor)) {
        const auto& area = monitor.rcWork;
        origin.x = std::clamp(origin.x, area.left - m_shadow, std::max(area.left - m_shadow, area.right + m_shadow - m_size.cx));
        origin.y = std::clamp(origin.y, area.top - m_shadow, std::max(area.top - m_shadow, area.bottom + m_shadow - m_size.cy));
    }
    m_origin = origin;
    if (IsWindowVisible(m_wnd))
        SetWindowPos(m_wnd, nullptr, origin.x, origin.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void TipWindow::present(BYTE alpha, bool with_content)
{
    BLENDFUNCTION blend{AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
    if (with_content) {
        POINT source{0, 0};
        UpdateLayeredWindow(m_wnd, nullptr, &m_origin, &m_size, m_dc.get(), &source, 0, &blend, ULW_ALPHA);
    } else {
        UpdateLayeredWindow(m_wnd, nullptr, nullptr, nullptr, nullptr, nullptr, 0, &blend, ULW_ALPHA);
    }
}

void TipWindow::on_timer(UINT_PTR id)
{
    if (id == watch_timer_id) {
        // 網頁提示：游標離開元件（多留 2 像素）就收
        POINT cursor{};
        GetCursorPos(&cursor);
        RECT area = m_anchor;
        InflateRect(&area, 2, 2);
        if (m_native == web_owner && !PtInRect(&area, cursor))
            hide(web_owner);
        return;
    }
    if (id != fade_timer_id)
        return;

    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_shown_at).count();
    const auto progress = std::clamp(elapsed / fade_in_ms, 0.0, 1.0);
    present(static_cast<BYTE>(std::lround(progress * 255)), false);
    if (progress >= 1.0)
        KillTimer(m_wnd, fade_timer_id);
}

void TipWindow::hide(HWND owner)
{
    if (!m_wnd || (owner && owner != m_native))
        return;
    KillTimer(m_wnd, fade_timer_id);
    KillTimer(m_wnd, watch_timer_id);
    ShowWindow(m_wnd, SW_HIDE);
    m_native = nullptr;
}

void TipWindow::destroy()
{
    hide(nullptr);
    if (m_wnd) {
        DestroyWindow(m_wnd);
        m_wnd = nullptr;
    }
    if (m_dc && m_old_bitmap)
        SelectObject(m_dc.get(), m_old_bitmap);
    m_old_bitmap = nullptr;
    m_dc.reset();
    m_bitmap.reset();
}

// 原生提示的子類別：要顯示的那一刻決定是不是換成我們的
LRESULT CALLBACK native_subclass(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    switch (msg) {
    case WM_WINDOWPOSCHANGING: {
        if (!g_alive)
            break;
        const auto* pos = reinterpret_cast<const WINDOWPOS*>(lp);
        if (pos->flags & SWP_SHOWWINDOW) {
            if (const auto text = eslyric_tip_text(wnd)) {
                // 空區域：原生提示整個看不見、滑鼠穿過，它自己的計時、顯示、隱藏照常
                SetWindowRgn(wnd, CreateRectRgn(0, 0, 0, 0), FALSE);
                POINT origin{pos->x, pos->y};
                if (pos->flags & SWP_NOMOVE) {
                    RECT rect{};
                    GetWindowRect(wnd, &rect);
                    origin = {rect.left, rect.top};
                }
                tip().show(wnd, *text, origin, GetDpiForWindow(wnd));
            } else {
                // 同一個原生提示也可能給別的元件用：不是 ESLyric 的就還原成看得見
                SetWindowRgn(wnd, nullptr, FALSE);
                tip().hide(wnd);
            }
        } else if (pos->flags & SWP_HIDEWINDOW) {
            tip().hide(wnd);
        } else if (!(pos->flags & SWP_NOMOVE)) {
            tip().move(wnd, POINT{pos->x, pos->y});
        }
        break;
    }
    case WM_SHOWWINDOW:
        if (!wp)
            tip().hide(wnd);
        break;
    case WM_NCDESTROY:
        tip().hide(wnd);
        RemoveWindowSubclass(wnd, native_subclass, id);
        break;
    }
    return DefSubclassProc(wnd, msg, wp, lp);
}

// 掛子類別（只能在建立那個視窗的執行緒上掛；fb2k 的介面都在主執行緒）
void attach(HWND wnd)
{
    if (!has_class(wnd, native_class_name))
        return;
    if (GetWindowThreadProcessId(wnd, nullptr) != GetCurrentThreadId())
        return;
    DWORD_PTR existing{};
    if (GetWindowSubclass(wnd, native_subclass, subclass_id, &existing))
        return;
    SetWindowSubclass(wnd, native_subclass, subclass_id, 0);
}

void CALLBACK on_win_event(
    HWINEVENTHOOK, DWORD event, HWND wnd, LONG object_id, LONG child_id, DWORD, DWORD)
{
    if (!g_alive || !wnd || object_id != OBJID_WINDOW || child_id != CHILDID_SELF)
        return;
    if (event == EVENT_OBJECT_CREATE || event == EVENT_OBJECT_SHOW)
        attach(wnd);
}

class TooltipInitQuit : public initquit {
public:
    void on_init() override
    {
        g_alive = true;
        // 之後建立的原生提示：建立時就掛上（顯示事件是保險：萬一錯過建立）
        g_hook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_SHOW, nullptr, on_win_event, GetCurrentProcessId(), 0,
            WINEVENT_OUTOFCONTEXT);
        // 已經存在的
        EnumThreadWindows(
            GetCurrentThreadId(),
            [](HWND wnd, LPARAM) -> BOOL {
                attach(wnd);
                return TRUE;
            },
            0);
    }

    void on_quit() override
    {
        g_alive = false;
        if (g_hook) {
            UnhookWinEvent(g_hook);
            g_hook = nullptr;
        }
        tip().destroy();
    }
};

initquit_factory_t<TooltipInitQuit> tooltip_initquit;

} // namespace

void show_below(const RECT& anchor, const std::wstring& text, UINT dpi)
{
    if (g_alive && !text.empty())
        tip().show_below(anchor, text, dpi);
}

void hide_below()
{
    if (g_alive)
        tip().hide(web_owner);
}

} // namespace cui::nocturne_tooltip
