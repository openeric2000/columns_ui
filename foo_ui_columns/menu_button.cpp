#include "pch.h"

#include "fallback_menu.h"
#include "layout.h"
#include "ng_playlist/ng_playlist.h"

// 自編版新增：主選單按鈕（☰）
// - 按下時在按鈕下方彈出 File／Edit／View／Playback／Library／Help 六組子選單，
//   用的是主視窗沒有 Menu 工具列時的 FallbackMenu，內容跟 Menu 工具列一樣由 fb2k 產生
// - 同時宣告工具列與面板兩種類型，可以放在頂端工具列區，也可以放進版面（例如專輯列表上方）
// - Alt、Alt＋字母不用另外處理：拿掉 Menu 工具列後，主視窗本來就會改用 FallbackMenu
// - 底色與圖示顏色跟 Playlist view 的配色一致（專輯列表、播放清單、底線式分頁同一套）
// - 2026-10-28 同一列最右邊加文字按鈕「收合」：讓版面裡的 Album list 回到初始狀態（只展開最上層）
//   原本用兩個箭頭對指的圖示，使用者回報小尺寸看起來像 X、看不出用途，改成直接寫字

namespace cui::panels::menu_button {

namespace {

// foo_uie_albumlist 的面板 GUID
constexpr GUID album_list_panel_guid{0x606e9cdd, 0x45ee, 0x4c3b, {0x9f, 0xd5, 0x49, 0x38, 0x1c, 0xeb, 0xe8, 0xae}};

// 「收合」的字型跟播放清單分頁同一個（Colours and fonts 的 Playlist tabs，playlist_tabs.cpp 的
// g_guid_playlist_switcher_tabs_font；那邊是檔內常數，這裡照抄 GUID）
constexpr GUID playlist_tabs_font_guid{0x942c36a4, 0x4e28, 0x4cea, {0x96, 0x44, 0xf2, 0x23, 0xc9, 0xa8, 0x38, 0xec}};

// Segoe MDL2 Assets 的漢堡圖示（GlobalNavigationButton）
constexpr wchar_t hamburger_glyph[] = L"\xE700";
// 2026-10-28 使用者覺得只寫「收合」看不出要收合什麼，改「全部收合」（「收合清單」的「清單」容易被當成播放清單）
constexpr wchar_t collapse_text[] = L"全部收合";
// 以下尺寸單位是 DIP：☰ 的可點範圍是正方形、圖示置中；「收合」同高、寬度＝字寬＋左右留白；
// 面板高度固定；兩顆按鈕各離左右邊 button_margin
constexpr int panel_height = 48;
constexpr int button_size = 40;
constexpr int button_margin = 4;
constexpr int glyph_font_size = 20;
constexpr int text_button_padding = 12;

enum class Button {
    none,
    menu,
    collapse,
};

// 把 item 和它後面的兄弟節點裡，展開中的都收合（子節點先收，重新展開時底下也是收合的）
// Album list（reupen/album_list_panel）的節點展開狀態靠 TVN_ITEMEXPANDED 記錄、關閉時存起來，
// 但程式送的 TVM_EXPAND 在節點展開過一次後就不會再發這個通知，所以收合後照它自己的格式補送一個；
// 選取若在被收合的節點底下，會移到該節點，這種選取變更（TVC_UNKNOWN）Album list 不會送歌到播放清單
void collapse_tree_items(HWND tree_wnd, HTREEITEM item)
{
    for (; item; item = TreeView_GetNextSibling(tree_wnd, item)) {
        if (!(TreeView_GetItemState(tree_wnd, item, TVIS_EXPANDED) & TVIS_EXPANDED))
            continue;

        collapse_tree_items(tree_wnd, TreeView_GetChild(tree_wnd, item));
        TreeView_Expand(tree_wnd, item, TVE_COLLAPSE);

        NMTREEVIEW nmtv{};
        nmtv.hdr.hwndFrom = tree_wnd;
        nmtv.hdr.idFrom = gsl::narrow_cast<UINT_PTR>(GetDlgCtrlID(tree_wnd));
        nmtv.hdr.code = TVN_ITEMEXPANDED;
        nmtv.action = TVE_COLLAPSE;
        nmtv.itemNew.mask = TVIF_HANDLE | TVIF_PARAM | TVIF_STATE;
        nmtv.itemNew.hItem = item;
        nmtv.itemNew.stateMask = TVIS_EXPANDED;
        if (TreeView_GetItem(tree_wnd, &nmtv.itemNew))
            SendMessage(GetParent(tree_wnd), WM_NOTIFY, nmtv.hdr.idFrom, reinterpret_cast<LPARAM>(&nmtv));
    }
}

// 讓樹狀圖回到初始狀態：最上層以下全部收合、捲回最上面
// 最上層只有一個節點時，當成 Album list 的根節點（設定裡開了 show root node），保持展開、只收合它底下
void collapse_tree_view(HWND tree_wnd)
{
    const auto root = TreeView_GetRoot(tree_wnd);
    if (!root)
        return;

    SetWindowRedraw(tree_wnd, FALSE);

    if (TreeView_GetNextSibling(tree_wnd, root))
        collapse_tree_items(tree_wnd, root);
    else
        collapse_tree_items(tree_wnd, TreeView_GetChild(tree_wnd, root));

    SendMessage(tree_wnd, WM_VSCROLL, MAKEWPARAM(SB_TOP, 0), 0);
    SendMessage(tree_wnd, WM_HSCROLL, MAKEWPARAM(SB_LEFT, 0), 0);

    SetWindowRedraw(tree_wnd, TRUE);
    RedrawWindow(tree_wnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME);
}

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
    RECT get_button_rect(Button button) const;
    Button hit_test(POINT pt) const;
    void paint(HDC dc) const;
    void show_menu();
    void collapse_album_list() const;
    void create_tooltip();
    void update_tooltip_rects() const;
    void set_tooltip_theme() const;

    std::optional<uih::BufferedPaintInitialiser> m_buffered_paint_initialiser;
    std::unique_ptr<colours::dark_mode_notifier> m_dark_mode_notifier;
    std::shared_ptr<FallbackMenu> m_menu;
    wil::unique_hfont m_glyph_font;
    wil::unique_hfont m_text_font;
    int m_collapse_text_width{};
    HWND m_tooltip_wnd{};
    Button m_hot_button{Button::none};
    // 收合：按下就執行，按住期間顯示按下的底色
    Button m_pressed_button{Button::none};
    bool m_is_menu_open{};
    // 選單開著時點按鈕會關掉選單，這一下點擊之後還會送到按鈕上，要略過，不然選單又會馬上打開
    bool m_ignore_next_click{};
};

RECT MenuButton::get_button_rect(Button button) const
{
    RECT client_rect{};
    GetClientRect(get_wnd(), &client_rect);

    const auto size = uih::scale_dpi_value(button_size);
    const auto margin = uih::scale_dpi_value(button_margin);
    const auto top = std::max(0l, (wil::rect_height(client_rect) - size) / 2);

    if (button == Button::collapse) {
        const auto right = client_rect.right - margin;
        const auto width = m_collapse_text_width + uih::scale_dpi_value(text_button_padding) * 2;
        return {right - width, top, right, top + size};
    }

    return {margin, top, margin + size, top + size};
}

Button MenuButton::hit_test(POINT pt) const
{
    for (const auto button : {Button::menu, Button::collapse}) {
        const auto rect = get_button_rect(button);
        if (PtInRect(&rect, pt))
            return button;
    }

    return Button::none;
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

    SetBkMode(dc, TRANSPARENT);

    for (const auto button : {Button::menu, Button::collapse}) {
        auto button_rect = get_button_rect(button);
        const auto is_pressed = (button == Button::menu && m_is_menu_open) || m_pressed_button == button;
        const auto is_hot = m_hot_button == button;

        if (is_pressed || is_hot) {
            const wil::unique_hbrush hot_brush(CreateSolidBrush(blend(is_pressed ? 0.18 : 0.10)));
            FillRect(dc, &button_rect, hot_brush.get());
        }

        if (button == Button::menu) {
            SetTextColor(dc, is_pressed || is_hot ? text_colour : blend(0.86));
            const auto _select_font = wil::SelectObject(dc, m_glyph_font.get());
            DrawText(dc, hamburger_glyph, -1, &button_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            continue;
        }

        // 「收合」平常是灰字（跟未選的分頁同一個混色比例），滑過或按下變白
        SetTextColor(dc, is_pressed || is_hot ? text_colour : blend(0.62));
        const auto _select_font = wil::SelectObject(dc, m_text_font.get());
        DrawText(dc, collapse_text, -1, &button_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
}

void MenuButton::show_menu()
{
    // 選單裡的命令可能會改版面、把這個按鈕關掉，所以先留住自己
    ptr self{this};
    const auto wnd = get_wnd();

    if (!m_menu)
        m_menu = std::make_shared<FallbackMenu>();

    auto button_rect = get_button_rect(Button::menu);
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

void MenuButton::collapse_album_list() const
{
    const auto album_list = g_layout_window.find_window_by_guid(album_list_panel_guid);

    if (!album_list.is_valid())
        return;

    // 直接收合樹狀圖的節點（2026-10-28 實測 4～9ms）。原本「再選一次目前的檢視」會整棵樹重建，曲庫大時要約 670ms
    if (const auto tree_wnd = FindWindowEx(album_list->get_wnd(), nullptr, WC_TREEVIEW, nullptr)) {
        collapse_tree_view(tree_wnd);
        return;
    }

    // 找不到樹狀圖時的備案，跟手動收合的做法一樣：在 Album list 自己的右鍵選單裡，再選一次目前的檢視（View 裡打勾的那一項），
    // 整棵樹會重建成初始狀態。不比對選單文字（Album list 介面語言改了也能用）：找第一個「有打勾子項目」的子選單，執行打勾的那一項
    const pfc::refcounted_object_ptr_t menu_hook = new uie::menu_hook_impl;
    album_list->get_menu_items(*menu_hook.get_ptr());

    for (size_t index = 0; index < menu_hook->get_children_count(); ++index) {
        uie::menu_node_ptr node;
        menu_hook->get_child(index, node);

        if (!node.is_valid() || node->get_type() != uie::menu_node_t::type_popup)
            continue;

        for (size_t child_index = 0; child_index < node->get_children_count(); ++child_index) {
            uie::menu_node_ptr child;
            node->get_child(child_index, child);

            pfc::string8 text;
            unsigned state{};
            if (child.is_valid() && child->get_type() == uie::menu_node_t::type_command
                && child->get_display_data(text, state) && (state & uie::menu_node_t::state_checked)) {
                child->execute();
                return;
            }
        }
    }
}

void MenuButton::create_tooltip()
{
    m_tooltip_wnd = CreateWindowEx(WS_EX_TOPMOST | WS_EX_TRANSPARENT, TOOLTIPS_CLASS, nullptr,
        WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, get_wnd(),
        nullptr, core_api::get_my_instance(), nullptr);

    if (!m_tooltip_wnd)
        return;

    set_tooltip_theme();

    // 只有 ☰ 需要提示（「收合」本身就是字），範圍在 update_tooltip_rects 跟著按鈕位置更新
    TOOLINFO ti{};
    ti.cbSize = TTTOOLINFO_V1_SIZE;
    ti.uFlags = TTF_SUBCLASS;
    ti.hwnd = get_wnd();
    ti.uId = static_cast<UINT_PTR>(Button::menu);
    ti.rect = get_button_rect(Button::menu);
    ti.lpszText = const_cast<wchar_t*>(L"選單");
    SendMessage(m_tooltip_wnd, TTM_ADDTOOL, 0, reinterpret_cast<LPARAM>(&ti));
}

void MenuButton::update_tooltip_rects() const
{
    if (!m_tooltip_wnd)
        return;

    TOOLINFO ti{};
    ti.cbSize = TTTOOLINFO_V1_SIZE;
    ti.hwnd = get_wnd();
    ti.uId = static_cast<UINT_PTR>(Button::menu);
    ti.rect = get_button_rect(Button::menu);
    SendMessage(m_tooltip_wnd, TTM_NEWTOOLRECT, 0, reinterpret_cast<LPARAM>(&ti));
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
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(lf.lfFaceName, L"Segoe MDL2 Assets");
        lf.lfHeight = -uih::scale_dpi_value(glyph_font_size);
        m_glyph_font.reset(CreateFontIndirect(&lf));

        m_text_font.reset(fb2k::std_api_get<fonts::manager>()->get_font(playlist_tabs_font_guid));
        {
            const auto dc = wil::GetDC(wnd);
            const auto _select_font = wil::SelectObject(dc.get(), m_text_font.get());
            SIZE text_size{};
            GetTextExtentPoint32(dc.get(), collapse_text, gsl::narrow<int>(wcslen(collapse_text)), &text_size);
            m_collapse_text_width = text_size.cx;
        }

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
        m_text_font.reset();
        m_buffered_paint_initialiser.reset();
        break;
    case WM_GETMINMAXINFO: {
        const auto mmi = reinterpret_cast<LPMINMAXINFO>(lp);
        mmi->ptMinTrackSize.x = uih::scale_dpi_value(button_margin * 4 + button_size + text_button_padding * 2)
            + m_collapse_text_width;
        mmi->ptMinTrackSize.y = uih::scale_dpi_value(panel_height);
        mmi->ptMaxTrackSize.y = uih::scale_dpi_value(panel_height);
        return 0;
    }
    case WM_WINDOWPOSCHANGED: {
        const auto lpwp = reinterpret_cast<LPWINDOWPOS>(lp);
        if (!(lpwp->flags & SWP_NOSIZE)) {
            update_tooltip_rects();
            InvalidateRect(wnd, nullptr, FALSE);
        }
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
        const auto hot_button = hit_test({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});

        if (hot_button != m_hot_button) {
            m_hot_button = hot_button;
            InvalidateRect(wnd, nullptr, FALSE);
        }

        if (hot_button != Button::none) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, wnd, 0};
            TrackMouseEvent(&tme);
        }
        break;
    }
    case WM_MOUSELEAVE:
        if (m_hot_button != Button::none) {
            m_hot_button = Button::none;
            InvalidateRect(wnd, nullptr, FALSE);
        }
        break;
    case WM_LBUTTONDOWN: {
        if (std::exchange(m_ignore_next_click, false))
            return 0;

        const auto button = hit_test({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});

        if (button == Button::menu) {
            show_menu();
        } else if (button == Button::collapse) {
            // 按下就執行（原本放開才執行，使用者覺得反應慢）；先把按下的底色畫出來再執行，Album list 重建時也看得到有按到
            m_pressed_button = button;
            SetCapture(wnd);
            RedrawWindow(wnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
            collapse_album_list();
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        m_ignore_next_click = false;

        if (m_pressed_button == Button::none)
            break;

        m_pressed_button = Button::none;
        ReleaseCapture();
        InvalidateRect(wnd, nullptr, FALSE);
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (m_pressed_button != Button::none) {
            m_pressed_button = Button::none;
            InvalidateRect(wnd, nullptr, FALSE);
        }
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
