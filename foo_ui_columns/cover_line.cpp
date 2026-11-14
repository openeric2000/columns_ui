#include "pch.h"
#include "cover_line.h"

#include "menu_items.h"
#include "next_track_toast.h"
#include "nocturne_surface.h"
#include "nocturne_text.h"
#include "now_playing_cover_zoom.h"
#include "popup_dismiss.h"
#include "splitter.h"

namespace cui::cover_line {

namespace {

constexpr wchar_t hotspot_class_name[] = L"columns_ui_cover_line_hotspot";
constexpr wchar_t picker_class_name[] = L"columns_ui_cover_line_picker";
constexpr char eslyric_window_class[] = "uie_eslyric_wnd_class";
// 熱區比那一排的排版高度上下各多一點，比較好點到
constexpr int hotspot_pad_dip = 10;

uint32_t clamp_style(uint32_t value)
{
    return value >= 1 && value <= style_count ? value : style_default;
}

void on_style_changed();

} // namespace

fbh::ConfigUint32 style(GUID{0x4a520ac0, 0x3401, 0x4395, {0xb0, 0x48, 0xf9, 0x73, 0x1c, 0xf0, 0x27, 0x48}}, style_default,
    [](uint32_t, uint32_t, std::optional<uint32_t>) { fb2k::inMainThread([] { on_style_changed(); }); });

namespace {

// ---- 下一首：主執行緒算好存起來，欄位只讀存好的值（欄位可能在別的執行緒算）----
std::mutex next_mutex;
std::string next_title;
std::string next_artist;
bool next_valid{};
bool next_update_scheduled{};

std::string format_handle(const metadb_handle_ptr& handle, const char* script)
{
    titleformat_object::ptr object;
    titleformat_compiler::get()->compile_safe(object, script);
    pfc::string8 out;
    handle->format_title(nullptr, out, object, nullptr);
    return out.c_str();
}

// 下一首的規則跟右下角的下一首提示同一套（佇列第一首 → 播放跟隨游標 → 下一列；隨機、單曲循環、沒有下一列就沒有）
void update_next()
{
    std::string title;
    std::string artist;
    bool valid{};
    metadb_handle_ptr now_playing;
    if (play_control::get()->get_now_playing(now_playing)) {
        if (const auto next = next_track_toast::predict_next_track(now_playing); next.is_valid()) {
            title = format_handle(next, "%title%");
            artist = format_handle(next, "[%artist%]");
            valid = !title.empty();
        }
    }

    bool changed{};
    {
        std::scoped_lock lock(next_mutex);
        changed = valid != next_valid || title != next_title || artist != next_artist;
        next_valid = valid;
        next_title = std::move(title);
        next_artist = std::move(artist);
    }
    if (changed)
        main_menu::refresh_layout_fields();
}

// 事件常常一次來好幾個（框選時焦點一直變），併成一次
void schedule_next_update()
{
    if (next_update_scheduled)
        return;
    next_update_scheduled = true;
    fb2k::inMainThread([] {
        next_update_scheduled = false;
        update_next();
    });
}

class NextPlaylistCallback : public playlist_callback_impl_base {
public:
    NextPlaylistCallback()
        : playlist_callback_impl_base(flag_on_items_added | flag_on_items_reordered | flag_on_items_removed
              | flag_on_item_focus_change | flag_on_playback_order_changed | flag_on_playlist_activate)
    {
    }
    void on_items_added(size_t, size_t, metadb_handle_list_cref, const bit_array&) override { schedule_next_update(); }
    void on_items_reordered(size_t, const size_t*, size_t) override { schedule_next_update(); }
    void on_items_removed(size_t, const bit_array&, size_t, size_t) override { schedule_next_update(); }
    void on_item_focus_change(size_t, size_t, size_t) override { schedule_next_update(); }
    void on_playback_order_changed(size_t) override { schedule_next_update(); }
    void on_playlist_activate(size_t, size_t) override { schedule_next_update(); }
};

class NextPlayCallback : public play_callback_impl_base {
public:
    NextPlayCallback() : play_callback_impl_base(flag_on_playback_new_track | flag_on_playback_stop) {}
    void on_playback_new_track(metadb_handle_ptr) override { schedule_next_update(); }
    void on_playback_stop(play_control::t_stop_reason) override { schedule_next_update(); }
};

class NextQueueCallback : public playback_queue_callback {
public:
    void on_changed(t_change_origin) override { fb2k::inMainThread([] { schedule_next_update(); }); }
};

service_factory_single_t<NextQueueCallback> next_queue_callback;

std::unique_ptr<NextPlaylistCallback> g_playlist_callback;
std::unique_ptr<NextPlayCallback> g_play_callback;

class CoverLineInitQuit : public initquit {
public:
    void on_init() override
    {
        g_playlist_callback = std::make_unique<NextPlaylistCallback>();
        g_play_callback = std::make_unique<NextPlayCallback>();
        schedule_next_update();
    }
    void on_quit() override
    {
        g_playlist_callback.reset();
        g_play_callback.reset();
    }
};

initquit_factory_t<CoverLineInitQuit> cover_line_initquit;

// ---- 共用：分層視窗貼圖 ----
void present_layered(HWND wnd, const POINT* origin, int width, int height, const std::function<void(Gdiplus::Graphics&)>& draw)
{
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits{};
    const wil::unique_hdc_window screen_dc(GetDC(nullptr));
    const wil::unique_hdc memory_dc(CreateCompatibleDC(screen_dc.get()));
    const wil::unique_hbitmap bitmap(CreateDIBSection(screen_dc.get(), &info, DIB_RGB_COLORS, &bits, nullptr, 0));
    if (!memory_dc || !bitmap || !bits)
        return;
    memset(bits, 0, static_cast<size_t>(width) * height * 4);
    {
        Gdiplus::Bitmap canvas(width, height, width * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(bits));
        Gdiplus::Graphics g(&canvas);
        draw(g);
    }
    const auto old = SelectObject(memory_dc.get(), bitmap.get());
    SIZE size{width, height};
    POINT source{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(wnd, screen_dc.get(), const_cast<POINT*>(origin), &size, memory_dc.get(), &source, 0, &blend,
        ULW_ALPHA);
    SelectObject(memory_dc.get(), old);
}

// ---- 挑選卡（外觀跟 VU 表／頻譜的樣式卡一致：nocturne_surface 的實心卡片、暖色框標示選中的那一列）----
namespace picker {

constexpr float shadow_dip = 18.f;
constexpr float pad_dip = 16.f;
constexpr float title_h_dip = 36.f;
constexpr float row_h_dip = 60.f;
constexpr float row_gap_dip = 8.f;
constexpr float card_w_dip = 420.f;
constexpr int close_hit = 900;

struct Picker {
    HWND wnd{};
    HWND hotspot{};
    float k{1.f};
    int width{};
    int height{};
    int shadow{};
    Gdiplus::RectF card;
    Gdiplus::RectF rows[style_count];
    Gdiplus::RectF close_box;
    std::wstring previews[style_count];
    int hover{-1};
    bool tracking{};
};

std::unique_ptr<Picker>& current()
{
    static std::unique_ptr<Picker> picker;
    return picker;
}

Gdiplus::Color rgba(int r, int g, int b, double a)
{
    return Gdiplus::Color(static_cast<BYTE>(std::clamp(a, 0.0, 1.0) * 255.0 + 0.5), static_cast<BYTE>(r),
        static_cast<BYTE>(g), static_cast<BYTE>(b));
}

// 主題字型（同 VU 表的樣式卡）；GDI+ 字型表沒暖機完時用系統的無襯線字
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

Gdiplus::FontStyle style_of(const Gdiplus::FontFamily& family, bool bold)
{
    if (bold && family.IsStyleAvailable(Gdiplus::FontStyleBold))
        return Gdiplus::FontStyleBold;
    return family.IsStyleAvailable(Gdiplus::FontStyleRegular) ? Gdiplus::FontStyleRegular : Gdiplus::FontStyleBold;
}

void draw_text(Gdiplus::Graphics& g, const wchar_t* text, const Gdiplus::Font& font, const Gdiplus::Color& colour,
    const Gdiplus::RectF& box, Gdiplus::StringAlignment align = Gdiplus::StringAlignmentNear)
{
    Gdiplus::StringFormat format;
    format.SetAlignment(align);
    format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    const Gdiplus::SolidBrush brush(colour);
    g.DrawString(text, -1, &font, box, &format, &brush);
}

std::wstring wide(const std::string& text)
{
    return pfc::stringcvt::string_wide_from_utf8(text.c_str()).get_ptr();
}

std::wstring format_now_playing(const char* script)
{
    titleformat_object::ptr object;
    titleformat_compiler::get()->compile_safe(object, script);
    pfc::string8 out;
    if (!play_control::get()->playback_format_title(nullptr, out, object, nullptr, play_control::display_level_all))
        return {};
    return wide(out.c_str());
}

// 每一列的預覽：用目前這首實際會顯示的字
void build_previews(Picker& p)
{
    {
        std::scoped_lock lock(next_mutex);
        if (next_valid) {
            std::wstring text = nocturne_text::cover_line_style_names[0];
            text += L"\x3000" + wide(next_title);
            if (!next_artist.empty())
                text += L"\x3000" + wide(next_artist);
            p.previews[0] = text;
        } else {
            p.previews[0] = nocturne_text::cover_line_no_next;
        }
    }
    p.previews[1] = format_now_playing(nocturne_text::cover_line_format_script);
    p.previews[2] = format_now_playing(nocturne_text::cover_line_stats_script);
}

void build_layout(Picker& p)
{
    const auto k = p.k;
    const auto px = [k](float dip) { return std::round(dip * k); };
    p.shadow = static_cast<int>(px(shadow_dip));
    const auto pad = px(pad_dip);
    const auto card_w = px(card_w_dip);
    const auto card_h = px(title_h_dip) + style_count * px(row_h_dip) + (style_count - 1) * px(row_gap_dip) + pad;
    const auto sh = static_cast<float>(p.shadow);
    p.card = {sh, sh - px(4), card_w, card_h};
    p.width = static_cast<int>(card_w + 2 * sh);
    p.height = static_cast<int>(card_h + 2 * sh);
    p.close_box = {p.card.X + card_w - pad - px(26), p.card.Y + px(5), px(26), px(26)};
    for (uint32_t i = 0; i < style_count; ++i)
        p.rows[i] = {p.card.X + pad, p.card.Y + px(title_h_dip) + i * (px(row_h_dip) + px(row_gap_dip)), card_w - 2 * pad,
            px(row_h_dip)};
}

bool inside(const Gdiplus::RectF& r, float x, float y)
{
    return x >= r.X && x < r.X + r.Width && y >= r.Y && y < r.Y + r.Height;
}

int hit_test(const Picker& p, POINT pt)
{
    const auto fx = static_cast<float>(pt.x);
    const auto fy = static_cast<float>(pt.y);
    if (inside(p.close_box, fx, fy))
        return close_hit;
    for (uint32_t i = 0; i < style_count; ++i)
        if (inside(p.rows[i], fx, fy))
            return static_cast<int>(i);
    return -1;
}

void render(Picker& p)
{
    const auto k = p.k;
    const auto selected = static_cast<int>(clamp_style(style.get())) - 1;
    present_layered(p.wnd, nullptr, p.width, p.height, [&](Gdiplus::Graphics& g) {
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

        nocturne_surface::paint_panel(g, p.card, nocturne_surface::card_radius_dip * k, p.shadow);

        const auto family = theme_family();
        const Gdiplus::Font title_font(family.get(), 13.f * k, style_of(*family, false), Gdiplus::UnitPixel);
        const Gdiplus::Font name_font(family.get(), 15.f * k, style_of(*family, false), Gdiplus::UnitPixel);
        const Gdiplus::Font name_bold(family.get(), 15.f * k, style_of(*family, true), Gdiplus::UnitPixel);
        const Gdiplus::Font preview_font(family.get(), 13.f * k, style_of(*family, false), Gdiplus::UnitPixel);

        draw_text(g, nocturne_text::cover_line_picker_title, title_font, rgba(244, 240, 230, 0.55),
            {p.card.X + pad_dip * k, p.card.Y, p.card.Width - 2 * pad_dip * k, title_h_dip * k});

        // 右上角關閉鈕
        {
            const auto& c = p.close_box;
            if (p.hover == close_hit) {
                const Gdiplus::SolidBrush hover_fill(rgba(255, 255, 255, 0.10));
                g.FillEllipse(&hover_fill, c);
            }
            const Gdiplus::Pen cross(rgba(244, 240, 230, p.hover == close_hit ? 0.9 : 0.55), 1.6f * k);
            const auto m = 8.5f * k;
            g.DrawLine(&cross, c.X + m, c.Y + m, c.X + c.Width - m, c.Y + c.Height - m);
            g.DrawLine(&cross, c.X + c.Width - m, c.Y + m, c.X + m, c.Y + c.Height - m);
        }

        // 三列：名稱＋預覽（目前這首會顯示的字），選中的暖色框（同 VU 表樣式卡的指針按鈕）
        for (uint32_t i = 0; i < style_count; ++i) {
            const auto& r = p.rows[i];
            const auto chosen = static_cast<int>(i) == selected;
            const auto hovered = p.hover == static_cast<int>(i);
            Gdiplus::GraphicsPath path;
            nocturne_surface::add_rounded(path, r, 10.f * k);
            const Gdiplus::SolidBrush fill(chosen ? rgba(240, 190, 120, 0.14) : rgba(255, 255, 255, hovered ? 0.07 : 0.035));
            g.FillPath(&fill, &path);
            const Gdiplus::Pen pen(
                chosen ? rgba(240, 190, 120, 0.95) : rgba(255, 255, 255, hovered ? 0.30 : 0.10), (chosen ? 1.5f : 1.f) * k);
            g.DrawPath(&pen, &path);

            const auto inner_x = r.X + 14.f * k;
            const auto inner_w = r.Width - 28.f * k;
            draw_text(g, nocturne_text::cover_line_style_names[i], chosen ? name_bold : name_font,
                chosen ? rgba(244, 240, 230, 0.96) : rgba(244, 240, 230, hovered ? 0.85 : 0.65),
                {inner_x, r.Y + 6.f * k, inner_w, 24.f * k});
            draw_text(g, p.previews[i].c_str(), preview_font, rgba(244, 240, 230, chosen ? 0.70 : 0.45),
                {inner_x, r.Y + 30.f * k, inner_w, 22.f * k});
        }
    });
}

LRESULT CALLBACK window_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    // 點外面、切走程式、失去啟用、熱區藏起來（收合、換成純封面、改回詳細）時關（見 popup_dismiss.h）
    if (popup_dismiss::handle_message(wnd, msg, wp, lp))
        return 0;

    auto& picker = current();
    if (!picker || picker->wnd != wnd)
        return DefWindowProc(wnd, msg, wp, lp);
    auto& p = *picker;

    switch (msg) {
    case WM_MOUSEMOVE: {
        if (!p.tracking) {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, wnd, 0};
            p.tracking = TrackMouseEvent(&track) != FALSE;
        }
        const auto hit = hit_test(p, {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        if (hit != p.hover) {
            p.hover = hit;
            SetCursor(LoadCursor(nullptr, hit >= 0 ? IDC_HAND : IDC_ARROW));
            render(p);
        }
        return 0;
    }
    case WM_SETCURSOR:
        SetCursor(LoadCursor(nullptr, p.hover >= 0 ? IDC_HAND : IDC_ARROW));
        return TRUE;
    case WM_MOUSELEAVE:
        p.tracking = false;
        if (p.hover != -1) {
            p.hover = -1;
            render(p);
        }
        return 0;
    case WM_LBUTTONUP: {
        const auto hit = hit_test(p, {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        if (hit == close_hit) {
            PostMessage(wnd, WM_CLOSE, 0, 0);
            return 0;
        }
        // 點一下就換；卡片留著方便比較，點外面才關（同 VU 表樣式卡）
        if (hit >= 0 && hit < static_cast<int>(style_count)) {
            const auto value = static_cast<uint32_t>(hit) + 1;
            if (clamp_style(style.get()) != value)
                style = value;
            render(p);
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE)
            PostMessage(wnd, WM_CLOSE, 0, 0);
        return 0;
    case WM_CLOSE:
        DestroyWindow(wnd);
        return 0;
    case WM_NCDESTROY:
        picker.reset();
        return DefWindowProc(wnd, msg, wp, lp);
    }
    return DefWindowProc(wnd, msg, wp, lp);
}

void close()
{
    if (auto& picker = current(); picker && picker->wnd)
        DestroyWindow(picker->wnd);
}

// 在滑鼠位置開卡片（卡片左上角對齊滑鼠，超出螢幕工作區就往內挪）
void open(HWND hotspot, POINT at)
{
    close();
    static const auto class_registered = [] {
        WNDCLASSEX window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = window_proc;
        window_class.hInstance = core_api::get_my_instance();
        window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
        window_class.lpszClassName = picker_class_name;
        return RegisterClassEx(&window_class) != 0;
    }();
    if (!class_registered)
        return;

    auto p = std::make_unique<Picker>();
    p->hotspot = hotspot;
    p->k = static_cast<float>(uih::scale_dpi_value(1000)) / 1000.f;
    build_layout(*p);
    build_previews(*p);

    const auto sh = p->shadow;
    auto x = at.x - sh;
    auto y = at.y - sh;
    MONITORINFO monitor{sizeof(monitor)};
    if (GetMonitorInfo(MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST), &monitor)) {
        const auto& work = monitor.rcWork;
        x = std::clamp(x, work.left - sh, std::max(work.left - sh, work.right + sh - p->width));
        y = std::clamp(y, work.top - sh, std::max(work.top - sh, work.bottom + sh - p->height));
    }

    const auto owner = GetAncestor(hotspot, GA_ROOT);
    current() = std::move(p);
    const auto wnd = CreateWindowEx(WS_EX_LAYERED | WS_EX_TOOLWINDOW, picker_class_name, L"", WS_POPUP, x, y,
        current()->width, current()->height, owner, nullptr, core_api::get_my_instance(), nullptr);
    if (!wnd) {
        current().reset();
        return;
    }
    current()->wnd = wnd;
    popup_dismiss::attach(wnd, [hotspot] { return IsWindow(hotspot) && IsWindowVisible(hotspot); });
    render(*current());
    ShowWindow(wnd, SW_SHOW);
    SetForegroundWindow(wnd);
    SetFocus(wnd);
}

// 樣式換了：卡片開著就重畫（選中的那列）
void refresh()
{
    if (auto& picker = current(); picker && picker->wnd)
        render(*picker);
}

} // namespace picker

// ---- 右鍵熱區 ----
std::vector<HWND>& live_windows()
{
    static std::vector<HWND> windows;
    return windows;
}

// 每個熱區記著上一次的播放列位置（分割面板座標），設定或模式改了用它重排
std::unordered_map<HWND, RECT>& bar_rects()
{
    static std::unordered_map<HWND, RECT> rects;
    return rects;
}

RECT compute_layout(const RECT& bar_rect)
{
    if (bar_rect.right <= bar_rect.left || !cover_zoom::compact_info.get())
        return {};

    // 只在展開頁完全展開、而且左邊有封面（純封面模式沒有這一排）時蓋
    RECT stage_screen{};
    HWND owner{};
    if (!panels::splitter::FlatSplitterPanel::s_get_now_playing_stage(stage_screen, owner))
        return {};
    if (main_menu::get_now_playing_mode() == main_menu::NowPlayingMode::cover)
        return {};

    const auto stage_width = bar_rect.right - bar_rect.left;
    const auto stage_height = bar_rect.top;
    const auto cover = cover_zoom::cover_rect_in_stage(stage_width, stage_height);
    const auto top = cover_zoom::info_top_in_stage(stage_width, stage_height);
    const auto bottom = cover_zoom::info_bottom_in_stage(stage_width, stage_height);
    if (!cover || !top || !bottom)
        return {};

    const auto pad = uih::scale_dpi_value(hotspot_pad_dip);
    return RECT{bar_rect.left + cover->left, std::max(0, *top - pad), bar_rect.left + cover->right,
        std::min(static_cast<int>(stage_height), *bottom + pad)};
}

// 熱區底下那個 ESLyric 視窗（右鍵以外的滑鼠訊息轉給它）
HWND find_eslyric_under(HWND wnd, POINT screen_point)
{
    struct Search {
        HWND self;
        POINT point;
        HWND found;
    } search{wnd, screen_point, nullptr};

    EnumChildWindows(
        GetParent(wnd),
        [](HWND child, LPARAM lp) -> BOOL {
            auto& state = *reinterpret_cast<Search*>(lp);
            if (child == state.self || !IsWindowVisible(child))
                return TRUE;
            char name[64]{};
            GetClassNameA(child, name, static_cast<int>(std::size(name)));
            if (strcmp(name, eslyric_window_class) != 0)
                return TRUE;
            RECT rect{};
            GetWindowRect(child, &rect);
            if (!PtInRect(&rect, state.point))
                return TRUE;
            state.found = child;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&search));

    return search.found;
}

void forward_client_message(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    POINT screen{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    ClientToScreen(wnd, &screen);
    const auto target = find_eslyric_under(wnd, screen);
    if (!target)
        return;
    ScreenToClient(target, &screen);
    SendMessage(target, msg, wp, MAKELPARAM(screen.x, screen.y));
}

void render_hotspot(HWND wnd)
{
    RECT client{};
    GetClientRect(wnd, &client);
    if (client.right <= 0 || client.bottom <= 0)
        return;
    // 幾乎看不見的底（透明度 1/255）：整塊收得到滑鼠，看起來跟沒蓋一樣
    present_layered(wnd, nullptr, client.right, client.bottom, [&](Gdiplus::Graphics& g) {
        const Gdiplus::SolidBrush brush(Gdiplus::Color(1, 0, 0, 0));
        g.FillRectangle(&brush, 0, 0, client.right, client.bottom);
    });
}

void apply_layout(HWND wnd)
{
    const auto it = bar_rects().find(wnd);
    const auto target = it == bar_rects().end() ? RECT{} : compute_layout(it->second);

    if (IsRectEmpty(&target)) {
        if (IsWindowVisible(wnd))
            ShowWindow(wnd, SW_HIDE);
        return;
    }

    RECT current{};
    GetWindowRect(wnd, &current);
    MapWindowPoints(HWND_DESKTOP, GetParent(wnd), reinterpret_cast<POINT*>(&current), 2);
    const auto visible = IsWindowVisible(wnd) != FALSE;

    if (!visible || !EqualRect(&current, &target))
        SetWindowPos(wnd, HWND_TOP, target.left, target.top, target.right - target.left, target.bottom - target.top,
            SWP_NOACTIVATE);
    render_hotspot(wnd);
    if (!visible)
        SetWindowPos(wnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

LRESULT CALLBACK hotspot_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_NCCREATE:
        live_windows().push_back(wnd);
        break;
    case WM_NCDESTROY: {
        auto& windows = live_windows();
        windows.erase(std::remove(windows.begin(), windows.end(), wnd), windows.end());
        bar_rects().erase(wnd);
        break;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(wnd, &ps);
        EndPaint(wnd, &ps);
        return 0;
    }
    case WM_SETCURSOR:
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        return TRUE;
    // 右鍵：開挑選卡（放開時開，按下那一下吃掉，免得 ESLyric 也跳選單）
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
    case WM_CONTEXTMENU:
        return 0;
    case WM_RBUTTONUP: {
        POINT at{};
        GetCursorPos(&at);
        picker::open(wnd, at);
        return 0;
    }
    // 其他按鍵與滾輪照常交給底下的 ESLyric
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
        forward_client_message(wnd, msg, wp, lp);
        return 0;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        const POINT screen{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (const auto target = find_eslyric_under(wnd, screen))
            return SendMessage(target, msg, wp, lp);
        return 0;
    }
    }
    return DefWindowProc(wnd, msg, wp, lp);
}

void on_style_changed()
{
    main_menu::refresh_layout_fields();
    picker::refresh();
}

} // namespace

uint32_t style_for_fields()
{
    return clamp_style(style.get());
}

bool next_text_for_fields(bool artist, std::string& out)
{
    std::scoped_lock lock(next_mutex);
    if (!next_valid)
        return false;
    out = artist ? next_artist : next_title;
    return !out.empty();
}

HWND create(HWND parent)
{
    static const auto class_registered = [] {
        WNDCLASSEX window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.style = CS_DBLCLKS;
        window_class.lpfnWndProc = hotspot_proc;
        window_class.hInstance = core_api::get_my_instance();
        window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
        window_class.lpszClassName = hotspot_class_name;
        return RegisterClassEx(&window_class) != 0;
    }();

    if (!class_registered)
        return nullptr;

    return CreateWindowEx(WS_EX_LAYERED, hotspot_class_name, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, parent,
        nullptr, core_api::get_my_instance(), nullptr);
}

void layout(HWND wnd, const RECT& bar_rect)
{
    bar_rects()[wnd] = bar_rect;
    apply_layout(wnd);
}

HDWP defer_layout(HDWP dwp, HWND wnd, const RECT& bar_rect)
{
    bar_rects()[wnd] = bar_rect;
    const auto target = compute_layout(bar_rect);

    if (IsRectEmpty(&target)) {
        if (IsWindowVisible(wnd))
            return DeferWindowPos(dwp, wnd, nullptr, 0, 0, 0, 0,
                SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return dwp;
    }

    // 還藏著的、或大小要變的不在批次裡處理：排版批次之後一定會再呼叫 layout()，由那裡貼好畫面再顯示
    if (!IsWindowVisible(wnd))
        return dwp;

    return DeferWindowPos(dwp, wnd, HWND_TOP, target.left, target.top, target.right - target.left,
        target.bottom - target.top, SWP_NOACTIVATE);
}

void refresh_layouts()
{
    for (const auto wnd : live_windows())
        apply_layout(wnd);
}

} // namespace cui::cover_line
