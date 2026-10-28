#include "pch.h"

#include "fallback_menu.h"
#include "ng_playlist/ng_playlist.h"

// 自編版新增：主選單按鈕（☰）
// - 按下時在按鈕下方彈出 File／Edit／View／Playback／Library／Help 六組子選單，
//   用的是主視窗沒有 Menu 工具列時的 FallbackMenu，內容跟 Menu 工具列一樣由 fb2k 產生
// - 同時宣告工具列與面板兩種類型，可以放在頂端工具列區，也可以放進版面（例如專輯列表上方）
// - Alt、Alt＋字母不用另外處理：拿掉 Menu 工具列後，主視窗本來就會改用 FallbackMenu
// - 底色與圖示顏色跟 Playlist view 的配色一致（專輯列表、播放清單、底線式分頁同一套）

namespace cui::panels::menu_button {

namespace {

// Segoe MDL2 Assets 的漢堡圖示（GlobalNavigationButton）
constexpr wchar_t hamburger_glyph[] = L"\xE700";
// 以下尺寸單位是 DIP：可點範圍是正方形，圖示置中；面板高度固定
constexpr int panel_height = 48;
constexpr int button_size = 40;
constexpr int button_margin_left = 4;
constexpr int glyph_font_size = 20;

class MenuButton : public uie::container_uie_window_v3 {
public:
    static constexpr GUID extension_guid{0xf12d4a25, 0x36c8, 0x47b3, {0x86, 0x11, 0x5c, 0x70, 0xc2, 0xab, 0xa1, 0xc8}};

    const GUID& get_extension_guid() const override { return extension_guid; }
    void get_name(pfc::string_base& out) const override { out = "主選單按鈕"; }
    void get_category(pfc::string_base& out) const override { out = "Toolbars"; }
    unsigned get_type() const override { return uie::type_toolbar | uie::type_panel; }

    uie::container_window_v3_config get_window_config() override
    {
        return {L"columns_ui_main_menu_button_f12d4a25", false};
    }

    LRESULT on_message(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) override;

private:
    RECT get_button_rect() const;
    void paint(HDC dc) const;
    void show_menu();
    void create_tooltip();
    void set_tooltip_theme() const;

    std::optional<uih::BufferedPaintInitialiser> m_buffered_paint_initialiser;
    std::unique_ptr<colours::dark_mode_notifier> m_dark_mode_notifier;
    std::shared_ptr<FallbackMenu> m_menu;
    wil::unique_hfont m_glyph_font;
    HWND m_tooltip_wnd{};
    bool m_is_hot{};
    bool m_is_menu_open{};
    // 選單開著時點按鈕會關掉選單，這一下點擊之後還會送到按鈕上，要略過，不然選單又會馬上打開
    bool m_ignore_next_click{};
};

RECT MenuButton::get_button_rect() const
{
    RECT client_rect{};
    GetClientRect(get_wnd(), &client_rect);

    const auto size = uih::scale_dpi_value(button_size);
    const auto left = uih::scale_dpi_value(button_margin_left);
    const auto top = std::max(0l, (wil::rect_height(client_rect) - size) / 2);

    return {left, top, left + size, top + size};
}

void MenuButton::paint(HDC dc) const
{
    colours::helper colour_helper(playlist_view::ColoursClient::id);
    const auto background_colour = colour_helper.get_colour(colours::colour_background);
    const auto text_colour = colour_helper.get_colour(colours::colour_text);

    // 跟分頁、播放列圖示同樣的作法：文字色往背景色混出淡底與略暗的圖示
    const auto blend = [&](double amount) {
        const auto mix = [amount](int from_value, int to_value) {
            return gsl::narrow_cast<BYTE>(std::lround(from_value + (to_value - from_value) * amount));
        };
        return RGB(mix(GetRValue(background_colour), GetRValue(text_colour)),
            mix(GetGValue(background_colour), GetGValue(text_colour)),
            mix(GetBValue(background_colour), GetBValue(text_colour)));
    };

    RECT client_rect{};
    GetClientRect(get_wnd(), &client_rect);

    const wil::unique_hbrush background_brush(CreateSolidBrush(background_colour));
    FillRect(dc, &client_rect, background_brush.get());

    auto button_rect = get_button_rect();

    if (m_is_menu_open || m_is_hot) {
        const wil::unique_hbrush hot_brush(CreateSolidBrush(blend(m_is_menu_open ? 0.18 : 0.10)));
        FillRect(dc, &button_rect, hot_brush.get());
    }

    const auto _select_font = wil::SelectObject(dc, m_glyph_font.get());
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, m_is_menu_open || m_is_hot ? text_colour : blend(0.86));
    DrawText(dc, hamburger_glyph, -1, &button_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

void MenuButton::show_menu()
{
    // 選單裡的命令可能會改版面、把這個按鈕關掉，所以先留住自己
    ptr self{this};
    const auto wnd = get_wnd();

    if (!m_menu)
        m_menu = std::make_shared<FallbackMenu>();

    auto button_rect = get_button_rect();
    MapWindowPoints(wnd, HWND_DESKTOP, reinterpret_cast<LPPOINT>(&button_rect), 2);

    m_is_menu_open = true;
    RedrawWindow(wnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);

    m_menu->show_at(wnd, {button_rect.left, button_rect.bottom});

    if (!IsWindow(wnd))
        return;

    m_is_menu_open = false;

    // 選單是被「點這顆按鈕」關掉的：滑鼠左鍵還按著、而且在按鈕上
    POINT cursor_pos{};
    GetCursorPos(&cursor_pos);
    const auto primary_button = GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON;
    m_ignore_next_click = (GetAsyncKeyState(primary_button) & 0x8000) && PtInRect(&button_rect, cursor_pos);

    InvalidateRect(wnd, nullptr, FALSE);
}

void MenuButton::create_tooltip()
{
    m_tooltip_wnd = CreateWindowEx(WS_EX_TOPMOST | WS_EX_TRANSPARENT, TOOLTIPS_CLASS, nullptr,
        WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, get_wnd(),
        nullptr, core_api::get_my_instance(), nullptr);

    if (!m_tooltip_wnd)
        return;

    set_tooltip_theme();

    TOOLINFO ti{};
    ti.cbSize = TTTOOLINFO_V1_SIZE;
    ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
    ti.hwnd = get_wnd();
    ti.uId = reinterpret_cast<UINT_PTR>(get_wnd());
    ti.lpszText = const_cast<wchar_t*>(L"選單");
    SendMessage(m_tooltip_wnd, TTM_ADDTOOL, 0, reinterpret_cast<LPARAM>(&ti));
}

void MenuButton::set_tooltip_theme() const
{
    if (m_tooltip_wnd)
        SetWindowTheme(m_tooltip_wnd, colours::is_dark_mode_active() ? L"DarkMode_Explorer" : nullptr, nullptr);
}

LRESULT MenuButton::on_message(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        m_buffered_paint_initialiser.emplace();

        LOGFONT lf{};
        lf.lfHeight = -uih::scale_dpi_value(glyph_font_size);
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(lf.lfFaceName, L"Segoe MDL2 Assets");
        m_glyph_font.reset(CreateFontIndirect(&lf));

        create_tooltip();

        m_dark_mode_notifier = std::make_unique<colours::dark_mode_notifier>([this, self = ptr{this}] {
            set_tooltip_theme();
            RedrawWindow(get_wnd(), nullptr, nullptr, RDW_INVALIDATE);
        });
        break;
    }
    case WM_DESTROY:
        m_dark_mode_notifier.reset();
        if (m_tooltip_wnd) {
            DestroyWindow(m_tooltip_wnd);
            m_tooltip_wnd = nullptr;
        }
        m_glyph_font.reset();
        m_buffered_paint_initialiser.reset();
        break;
    case WM_GETMINMAXINFO: {
        const auto mmi = reinterpret_cast<LPMINMAXINFO>(lp);
        mmi->ptMinTrackSize.x = uih::scale_dpi_value(button_margin_left * 2 + button_size);
        mmi->ptMinTrackSize.y = uih::scale_dpi_value(panel_height);
        mmi->ptMaxTrackSize.y = uih::scale_dpi_value(panel_height);
        return 0;
    }
    case WM_WINDOWPOSCHANGED: {
        const auto lpwp = reinterpret_cast<LPWINDOWPOS>(lp);
        if (!(lpwp->flags & SWP_NOSIZE))
            InvalidateRect(wnd, nullptr, FALSE);
        break;
    }
    case WM_ERASEBKGND:
        return TRUE;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        const auto paint_dc = wil::BeginPaint(wnd, &ps);
        const auto buffered_dc = uih::BufferedPaint(paint_dc.get(), ps.rcPaint);
        paint(buffered_dc.get());
        return 0;
    }
    case WM_MOUSEMOVE: {
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const auto button_rect = get_button_rect();
        const auto is_hot = PtInRect(&button_rect, pt) != FALSE;

        if (is_hot != m_is_hot) {
            m_is_hot = is_hot;
            InvalidateRect(wnd, &button_rect, FALSE);
        }

        if (is_hot) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, wnd, 0};
            TrackMouseEvent(&tme);
        }
        break;
    }
    case WM_MOUSELEAVE:
        if (m_is_hot) {
            m_is_hot = false;
            InvalidateRect(wnd, nullptr, FALSE);
        }
        break;
    case WM_LBUTTONDOWN: {
        if (std::exchange(m_ignore_next_click, false))
            return 0;

        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const auto button_rect = get_button_rect();

        if (PtInRect(&button_rect, pt))
            show_menu();
        return 0;
    }
    case WM_LBUTTONUP:
        m_ignore_next_click = false;
        break;
    case WM_MENUSELECT:
        if (m_menu)
            m_menu->on_menu_select(LOWORD(wp), HIWORD(wp));
        return 0;
    }
    return DefWindowProc(wnd, msg, wp, lp);
}

uie::window_factory<MenuButton> _menu_button_factory;

} // namespace

} // namespace cui::panels::menu_button
