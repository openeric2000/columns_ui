#include "pch.h"

#include <charconv>
#include <unordered_map>

#include "now_playing_cover_zoom.h"
#include "menu_items.h"
#include "next_track_toast.h"
#include "nocturne_text.h"
#include "splitter.h"

// 展開頁大封面右下角的「看大圖」放大鈕，說明見 now_playing_cover_zoom.h

namespace cui::cover_zoom {

namespace {

constexpr wchar_t window_class_name[] = L"columns_ui_cover_zoom";

// 按鈕外觀（DIP）：照 2026-11-09 在 main.xml 試做、使用者看過位置的那顆
constexpr int button_size_dip = 44;
constexpr int button_inset_dip = 14;     // 離封面右緣、下緣
constexpr float icon_font_dip = 24;      // main.xml 的 font-size 18（pt）＝24 DIP
constexpr int stage_min_height_dip = 300; // main.xml 的 stage min-height：比這矮整個展開頁會藏起來

// 平常不搶封面（2026-11-09 使用者看了 .45 深色圓底、再淡的圓底都說還是一圈很明顯，最後定案「平常不要圓底、只留雙箭頭」）：
// 平常只畫箭頭（白 .24），底下墊一層往右下偏 1 DIP 的淡黑陰影，白色或很淺的封面上才認得出來；
// 滑過、按下才出現深色圓底（.59／.71）與全白箭頭
const Gdiplus::Color disc_hot_colour(150, 0, 0, 0);
const Gdiplus::Color disc_pressed_colour(180, 0, 0, 0);
const Gdiplus::Color icon_colour(61, 241, 241, 241);
const Gdiplus::Color icon_shadow_colour(46, 0, 0, 0);
const Gdiplus::Color icon_hot_colour(255, 255, 255, 255);

// 出現時淡入（2026-11-09 使用者回報：展開頁滑完停住的那一格按鈕才突然冒出來，像彈一下）。
// 展開動畫是一張往上滑的截圖，截圖時按鈕還藏著，所以按鈕沒辦法跟著滑上來；改成停住後用這段時間淡入
constexpr auto fade_in_duration = std::chrono::milliseconds(180);
constexpr UINT_PTR fade_timer_id = 1;
constexpr UINT fade_timer_interval_ms = 10;

int dip(int value)
{
    return uih::scale_dpi_value(value);
}

float dip_f(float value)
{
    return value * static_cast<float>(uih::scale_dpi_value(1000)) / 1000.0f;
}

// 跟 volume_chip.cpp 同一套：draw 畫好的內容用 UpdateLayeredWindow 貼到 wnd（32 位元預乘 alpha 的 DIB）；
// opacity 是整個視窗再乘上的不透明度（淡入用，0～255）
void present_layered(HWND wnd, int width, int height, BYTE opacity, const std::function<void(Gdiplus::Graphics&)>& draw)
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
    if (!bitmap || !bits)
        return;
    const auto _select_bitmap = wil::SelectObject(memory_dc.get(), bitmap.get());

    {
        Gdiplus::Bitmap canvas(width, height, width * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(bits));
        Gdiplus::Graphics g(&canvas);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));
        draw(g);
    }

    SIZE size{width, height};
    POINT source{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, opacity, AC_SRC_ALPHA};
    UpdateLayeredWindow(wnd, screen_dc.get(), nullptr, &size, memory_dc.get(), &source, 0, &blend, ULW_ALPHA);
}

// 圖示字型：Windows 10 是 Segoe MDL2 Assets，Windows 11 還有 Segoe Fluent Icons。建一次放著不釋放（GDI+ 關閉後才釋放會出錯）
const Gdiplus::FontFamily* icon_family()
{
    static const Gdiplus::FontFamily* family = []() -> const Gdiplus::FontFamily* {
        for (const auto name : {L"Segoe MDL2 Assets", L"Segoe Fluent Icons"}) {
            auto* candidate = new Gdiplus::FontFamily(name);
            if (candidate->GetLastStatus() == Gdiplus::Ok)
                return candidate;
            delete candidate;
        }
        return nullptr;
    }();
    return family;
}

// ---- main.xml 裡的封面位置標記 ----
// <!-- nocturne-cover-zoom: left=472 cover=600 gap=24 info=190 -->；數字是 DIP，加 % 是展開頁寬度的百分比
// 版面檔位置跟 volume_chip.cpp 相同；檔案最後修改時間變了才重讀，而且最多每秒檢查一次
struct Length {
    double value{};
    bool percent{};
};

struct CoverGeometry {
    bool valid{};
    Length left;
    Length cover;
    Length gap;
    Length info;
    // 歌詞欄（2026-11-11）：lyrics-left＝展開頁左緣到歌詞欄、lyrics-right＝歌詞欄右緣到展開頁右緣；
    // vu-width（可省略）＝VU 表最寬多少。給 VU 表模式的位置與歌詞防誤拖的蓋層用
    bool has_lyrics{};
    Length lyrics_left;
    Length lyrics_right;
    bool has_vu_width{};
    Length vu_width;
};

std::wstring layout_file_path()
{
    const pfc::string8 native = ::filesystem::g_get_native_path(core_api::get_profile_path());
    return std::wstring(pfc::stringcvt::string_wide_from_utf8(native.c_str()).get_ptr())
        + L"\\eslyric-data\\layout\\fb2k-modern\\main.xml";
}

std::optional<Length> parse_length(std::string_view text)
{
    Length length;
    if (!text.empty() && text.back() == '%') {
        length.percent = true;
        text.remove_suffix(1);
    }

    double value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value < 0)
        return {};

    length.value = value;
    return length;
}

CoverGeometry parse_geometry(const std::string& text)
{
    constexpr std::string_view marker = "nocturne-cover-zoom:";
    const auto start = text.find(marker);
    if (start == std::string::npos)
        return {};

    const auto end = text.find("-->", start);
    const std::string_view body(text.data() + start + marker.size(),
        (end == std::string::npos ? text.size() : end) - start - marker.size());

    CoverGeometry geometry;
    bool has_left{}, has_cover{}, has_gap{}, has_info{}, has_lyrics_left{}, has_lyrics_right{};
    size_t pos = 0;

    while (pos < body.size()) {
        while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos])))
            ++pos;
        const auto token_end = std::min(body.find_first_of(" \t\r\n", pos), body.size());
        const auto token = body.substr(pos, token_end - pos);
        pos = token_end;

        const auto equals = token.find('=');
        if (equals == std::string_view::npos)
            continue;

        const auto key = token.substr(0, equals);
        const auto value = parse_length(token.substr(equals + 1));
        if (!value)
            continue;

        if (key == "left") {
            geometry.left = *value;
            has_left = true;
        } else if (key == "cover") {
            geometry.cover = *value;
            has_cover = true;
        } else if (key == "gap") {
            geometry.gap = *value;
            has_gap = true;
        } else if (key == "info") {
            geometry.info = *value;
            has_info = true;
        } else if (key == "lyrics-left") {
            geometry.lyrics_left = *value;
            has_lyrics_left = true;
        } else if (key == "lyrics-right") {
            geometry.lyrics_right = *value;
            has_lyrics_right = true;
        } else if (key == "vu-width") {
            geometry.vu_width = *value;
            geometry.has_vu_width = geometry.vu_width.value > 0;
        }
    }

    geometry.valid = has_left && has_cover && has_gap && has_info && geometry.cover.value > 0;
    geometry.has_lyrics = has_lyrics_left && has_lyrics_right;
    return geometry;
}

struct GeometryProbe {
    CoverGeometry geometry;
    bool read{};
    FILETIME stamp{};
    ULONGLONG checked_at{};
};
GeometryProbe g_probe;

const CoverGeometry& layout_geometry()
{
    const auto now = GetTickCount64();
    if (g_probe.read && now - g_probe.checked_at < 1000)
        return g_probe.geometry;
    g_probe.checked_at = now;

    try {
        const auto path = layout_file_path();
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) {
            g_probe.read = true;
            g_probe.geometry = {};
            return g_probe.geometry;
        }
        if (g_probe.read && CompareFileTime(&info.ftLastWriteTime, &g_probe.stamp) == 0)
            return g_probe.geometry;

        std::string text;
        FILE* file{};
        if (_wfopen_s(&file, path.c_str(), L"rb") == 0 && file) {
            char buffer[8192];
            size_t read{};
            while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0 && text.size() < (size_t{4} << 20))
                text.append(buffer, read);
            fclose(file);
            g_probe.geometry = parse_geometry(text);
        } else {
            g_probe.geometry = {};
        }
        g_probe.stamp = info.ftLastWriteTime;
        g_probe.read = true;
    }
    catch (...) {
        g_probe.read = true;
        g_probe.geometry = {};
    }
    return g_probe.geometry;
}

void open_cover_viewer()
{
    metadb_handle_ptr track;
    if (!playback_control::get()->get_now_playing(track))
        return;

    if (fb2k::imageViewer::ptr api; fb2k::imageViewer::tryGet(api))
        api->load_and_show(core_api::get_main_window(), pfc::list_single_ref_t<metadb_handle_ptr>(track),
            album_art_ids::cover_front);
}

class Button;

std::vector<Button*>& live_buttons()
{
    static std::vector<Button*> buttons;
    return buttons;
}

class Button {
public:
    static LRESULT CALLBACK s_wndproc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp);
    static Button* from(HWND wnd) { return reinterpret_cast<Button*>(GetWindowLongPtr(wnd, GWLP_USERDATA)); }

    void set_bar_rect(const RECT& rect) { m_bar_rect = rect; }
    // 給排版批次（defer_layout）用：視窗該放的位置（分割面板座標），空矩形＝要藏起來
    RECT target_rect() const { return compute_layout(); }

    void apply_layout()
    {
        const auto target = compute_layout();

        if (IsRectEmpty(&target)) {
            stop_fade();
            if (IsWindowVisible(m_wnd))
                ShowWindow(m_wnd, SW_HIDE);
            reset_mouse_state();
            return;
        }

        // 從藏著變成顯示：先放好位置、貼上全透明的第一格再顯示，然後淡入。
        // 先貼再顯示也避免露出上次藏起來前留下的畫面（例如當時是滑過的深色圓底）
        if (!IsWindowVisible(m_wnd)) {
            m_opacity = 0;
            m_fade_start = std::chrono::steady_clock::now();
            SetWindowPos(m_wnd, HWND_TOP, target.left, target.top, target.right - target.left,
                target.bottom - target.top, SWP_NOACTIVATE);
            render(true);
            SetWindowPos(m_wnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            SetTimer(m_wnd, fade_timer_id, fade_timer_interval_ms, nullptr);
            return;
        }

        RECT current{};
        GetWindowRect(m_wnd, &current);
        MapWindowPoints(HWND_DESKTOP, GetParent(m_wnd), reinterpret_cast<POINT*>(&current), 2);

        if (!EqualRect(&current, &target)) {
            SetWindowPos(m_wnd, HWND_TOP, target.left, target.top, target.right - target.left,
                target.bottom - target.top, SWP_NOACTIVATE);
        }

        render();
    }

    // force：視窗還藏著也貼（顯示前先準備好第一格）
    void render(bool force = false)
    {
        if (!m_wnd || (!force && !IsWindowVisible(m_wnd)))
            return;

        RECT client{};
        GetClientRect(m_wnd, &client);
        if (client.right <= 0 || client.bottom <= 0)
            return;

        present_layered(m_wnd, client.right, client.bottom, m_opacity,
            [&](Gdiplus::Graphics& g) { draw(g, client.right, client.bottom); });
    }

private:
    // 淡入的每一格：ease-out（一開始變化快、最後慢慢到全不透明）
    void step_fade()
    {
        const auto elapsed = std::chrono::steady_clock::now() - m_fade_start;
        const auto progress = std::clamp(
            std::chrono::duration<double>(elapsed) / std::chrono::duration<double>(fade_in_duration), 0.0, 1.0);
        const auto eased = 1.0 - (1.0 - progress) * (1.0 - progress);
        m_opacity = static_cast<BYTE>(std::lround(255.0 * eased));
        render();
        if (progress >= 1.0)
            KillTimer(m_wnd, fade_timer_id);
    }

    void stop_fade()
    {
        KillTimer(m_wnd, fade_timer_id);
        m_opacity = 255;
    }

    RECT compute_layout() const
    {
        if (m_bar_rect.right <= m_bar_rect.left)
            return {};

        // 展開頁完全展開（動畫與截圖都結束）才顯示；收合、動畫中都藏起來
        RECT stage_screen{};
        HWND owner{};
        if (!panels::splitter::FlatSplitterPanel::s_get_now_playing_stage(stage_screen, owner))
            return {};

        // 資訊卡模式（樣式 3）沒有封面；純封面模式的大封面不在標記的位置（而且已經夠大，點封面就是看大圖）
        if (main_menu::get_now_playing_style() == 3 || main_menu::get_now_playing_mode() == main_menu::NowPlayingMode::cover)
            return {};

        // GDI+ 字型表還在背景暖機時不在主執行緒建字型（同音量圖示），暖機做完會重排
        if (!next_track_toast::gdiplus_fonts_ready())
            return {};

        // 展開頁＝分割面板扣掉底部播放列那一段（播放列視窗跟分割面板一樣大、在 (0, 0)）
        const auto stage_width = m_bar_rect.right - m_bar_rect.left;
        const auto stage_height = m_bar_rect.top;
        const auto cover_rect = cover_rect_in_stage(stage_width, stage_height);
        if (!cover_rect)
            return {};

        const auto cover = cover_rect->right - cover_rect->left;
        const auto size = dip(button_size_dip);
        const auto inset = dip(button_inset_dip);

        // 封面太小就不顯示按鈕
        if (cover < size + 2 * inset)
            return {};

        const auto right = m_bar_rect.left + cover_rect->right - inset;
        const auto bottom = cover_rect->bottom - inset;
        return {right - size, bottom - size, right, bottom};
    }

    void draw(Gdiplus::Graphics& g, int width, int height) const
    {
        // 幾乎看不見的底（透明度 1/255）：圓以外的四個角也收得到滑鼠，點了不會穿過去切到資訊卡
        {
            const Gdiplus::SolidBrush hit_brush(Gdiplus::Color(1, 0, 0, 0));
            g.FillRectangle(&hit_brush, 0, 0, width, height);
        }

        const auto lit = m_hot || m_pressed;

        // 圓底只在滑過、按下時畫
        if (lit) {
            const Gdiplus::SolidBrush brush(m_pressed ? disc_pressed_colour : disc_hot_colour);
            g.FillEllipse(&brush, 0.f, 0.f, static_cast<float>(width), static_cast<float>(height));
        }

        const auto family = icon_family();
        if (!family)
            return;

        const Gdiplus::Font font(family, dip_f(icon_font_dip), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
        format.SetAlignment(Gdiplus::StringAlignmentCenter);
        format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap | Gdiplus::StringFormatFlagsNoClip);
        const Gdiplus::RectF box(0.f, 0.f, static_cast<float>(width), static_cast<float>(height));

        // 平常（沒有圓底）先畫淡黑陰影，亮色封面上才看得出箭頭
        if (!lit) {
            const auto offset = dip_f(1.f);
            const Gdiplus::SolidBrush shadow(icon_shadow_colour);
            g.DrawString(L"\xE740", -1, &font, Gdiplus::RectF(box.X + offset, box.Y + offset, box.Width, box.Height),
                &format, &shadow);
        }

        const Gdiplus::SolidBrush brush(lit ? icon_hot_colour : icon_colour);
        // E740＝全螢幕（四角往外的箭頭）
        g.DrawString(L"\xE740", -1, &font, box, &format, &brush);
    }

    void reset_mouse_state()
    {
        m_hot = false;
        m_pressed = false;
        m_tracking = false;
    }

    void create_tooltip()
    {
        m_tooltip = CreateWindowEx(WS_EX_TOPMOST | WS_EX_TRANSPARENT, TOOLTIPS_CLASS, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, m_wnd, nullptr, core_api::get_my_instance(), nullptr);
        if (!m_tooltip)
            return;

        TOOLINFO tool{};
        tool.cbSize = sizeof(tool);
        tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        tool.hwnd = m_wnd;
        tool.uId = reinterpret_cast<UINT_PTR>(m_wnd);
        tool.lpszText = const_cast<wchar_t*>(nocturne_text::cover_zoom_tooltip);
        SendMessage(m_tooltip, TTM_ADDTOOL, 0, reinterpret_cast<LPARAM>(&tool));
    }

    LRESULT on_message(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg) {
        case WM_NCCREATE:
            m_wnd = wnd;
            SetWindowLongPtr(wnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
            live_buttons().push_back(this);
            break;
        case WM_CREATE:
            create_tooltip();
            break;
        case WM_NCDESTROY: {
            if (m_tooltip)
                DestroyWindow(std::exchange(m_tooltip, nullptr));
            auto& buttons = live_buttons();
            buttons.erase(std::remove(buttons.begin(), buttons.end(), this), buttons.end());
            SetWindowLongPtr(wnd, GWLP_USERDATA, 0);
            delete this;
            return DefWindowProc(wnd, msg, wp, lp);
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_TIMER:
            if (wp == fade_timer_id) {
                step_fade();
                return 0;
            }
            break;
        case WM_PAINT: {
            // 分層視窗的畫面由 render() 貼上去，這裡只要把更新區域標成已處理
            PAINTSTRUCT ps{};
            BeginPaint(wnd, &ps);
            EndPaint(wnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE:
            if (!m_tracking) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, wnd, 0};
                if (TrackMouseEvent(&tme))
                    m_tracking = true;
            }
            if (!m_hot) {
                m_hot = true;
                render();
            }
            return 0;
        case WM_MOUSELEAVE:
            m_tracking = false;
            // 按著滑鼠時（有捕捉）游標可能先跑到外面，放開時再依實際位置判斷
            if (!m_pressed) {
                m_hot = false;
                render();
            }
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            m_pressed = true;
            SetCapture(wnd);
            render();
            return 0;
        case WM_LBUTTONUP: {
            const auto was_pressed = std::exchange(m_pressed, false);
            if (GetCapture() == wnd)
                ReleaseCapture();

            RECT client{};
            GetClientRect(wnd, &client);
            const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};

            POINT cursor{};
            GetCursorPos(&cursor);
            m_hot = WindowFromPoint(cursor) == wnd;
            render();

            if (was_pressed && PtInRect(&client, pt))
                open_cover_viewer();
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (m_pressed) {
                m_pressed = false;
                render();
            }
            break;
        case WM_SETCURSOR:
            SetCursor(LoadCursor(nullptr, IDC_HAND));
            return TRUE;
        }

        return DefWindowProc(wnd, msg, wp, lp);
    }

    HWND m_wnd{};
    HWND m_tooltip{};
    RECT m_bar_rect{};
    BYTE m_opacity{255};
    std::chrono::steady_clock::time_point m_fade_start{};
    bool m_hot{};
    bool m_tracking{};
    bool m_pressed{};
};

LRESULT CALLBACK Button::s_wndproc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Button* button = from(wnd);

    if (msg == WM_NCCREATE) {
        button = new Button();
        return button->on_message(wnd, msg, wp, lp);
    }

    if (!button)
        return DefWindowProc(wnd, msg, wp, lp);

    return button->on_message(wnd, msg, wp, lp);
}

} // namespace

std::optional<RECT> cover_rect_in_stage(int stage_width, int stage_height)
{
    const auto& geometry = layout_geometry();
    if (!geometry.valid || stage_width <= 0 || stage_height < dip(stage_min_height_dip))
        return {};

    const auto resolve = [stage_width](const Length& length) {
        return length.percent ? static_cast<int>(std::lround(stage_width * length.value / 100.0))
                              : uih::scale_dpi_value(static_cast<int>(std::lround(length.value)));
    };

    // 封面＋gap＋info 這一整組在展開頁垂直置中（main.xml 的 disc 容器 justify=middle）
    const auto cover = resolve(geometry.cover);
    const auto cover_left = resolve(geometry.left);
    const auto group_height = cover + resolve(geometry.gap) + resolve(geometry.info);
    const auto cover_top = (stage_height - group_height) / 2;

    // 跑出展開頁（視窗太小）就當作沒有
    if (cover <= 0 || cover_left + cover > stage_width || cover_top < 0 || cover_top + cover > stage_height)
        return {};

    return RECT{cover_left, cover_top, cover_left + cover, cover_top + cover};
}

std::optional<RECT> lyrics_column_in_stage(int stage_width, int stage_height)
{
    const auto& geometry = layout_geometry();
    if (!geometry.has_lyrics || stage_width <= 0 || stage_height < dip(stage_min_height_dip))
        return {};

    const auto resolve = [stage_width](const Length& length) {
        return length.percent ? static_cast<int>(std::lround(stage_width * length.value / 100.0))
                              : uih::scale_dpi_value(static_cast<int>(std::lround(length.value)));
    };

    const auto left = resolve(geometry.lyrics_left);
    const auto right = stage_width - resolve(geometry.lyrics_right);
    if (left < 0 || right - left < dip(40))
        return {};

    return RECT{left, 0, right, stage_height};
}

std::optional<RECT> vu_rect_in_stage(int stage_width, int stage_height)
{
    const auto column = lyrics_column_in_stage(stage_width, stage_height);
    if (!column)
        return {};

    const auto& geometry = layout_geometry();
    auto width = static_cast<int>(column->right - column->left);
    if (geometry.has_vu_width) {
        const auto cap = geometry.vu_width.percent
            ? static_cast<int>(std::lround(stage_width * geometry.vu_width.value / 100.0))
            : uih::scale_dpi_value(static_cast<int>(std::lround(geometry.vu_width.value)));
        width = std::min(width, cap);
    }

    // 高度照 VU 表外觀 Modern antonyb 的比例 830×199（同 2026-11-07 拿掉的樣式 2）；在展開頁垂直置中、靠歌詞欄左緣（歌詞也是靠左）
    const auto height = MulDiv(width, 199, 830);
    if (width <= 0 || height <= 0 || height > stage_height)
        return {};

    const auto top = (stage_height - height) / 2;
    return RECT{column->left, top, column->left + width, top + height};
}

void refresh_layouts()
{
    for (const auto button : live_buttons())
        button->apply_layout();
}

HWND create(HWND parent)
{
    static const auto class_registered = [] {
        WNDCLASSEX window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        window_class.lpfnWndProc = Button::s_wndproc;
        window_class.hInstance = core_api::get_my_instance();
        window_class.hCursor = LoadCursor(nullptr, IDC_HAND);
        window_class.lpszClassName = window_class_name;
        return RegisterClassEx(&window_class) != 0;
    }();

    if (!class_registered)
        return nullptr;

    const auto wnd = CreateWindowEx(WS_EX_LAYERED, window_class_name, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, parent,
        nullptr, core_api::get_my_instance(), nullptr);

    // GDI+ 字型表暖機做完要重排：暖機沒做完時 compute_layout() 傳回空矩形。只登記一次
    if (wnd) {
        static const auto registered = [] {
            next_track_toast::when_gdiplus_fonts_ready([] { refresh_layouts(); });
            return true;
        }();
        (void)registered;
    }

    return wnd;
}

void layout(HWND wnd, const RECT& bar_rect)
{
    if (const auto button = Button::from(wnd)) {
        button->set_bar_rect(bar_rect);
        button->apply_layout();
    }
}

HDWP defer_layout(HDWP dwp, HWND wnd, const RECT& bar_rect)
{
    const auto button = Button::from(wnd);
    if (!button)
        return dwp;

    button->set_bar_rect(bar_rect);
    const auto target = button->target_rect();

    if (IsRectEmpty(&target)) {
        if (IsWindowVisible(wnd))
            return DeferWindowPos(dwp, wnd, nullptr, 0, 0, 0, 0,
                SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return dwp;
    }

    // 還藏著的不在批次裡顯示：排版批次之後一定會再呼叫 layout()，由那裡貼好第一格再淡入
    if (!IsWindowVisible(wnd))
        return dwp;

    return DeferWindowPos(dwp, wnd, HWND_TOP, target.left, target.top, target.right - target.left,
        target.bottom - target.top, SWP_NOACTIVATE);
}

} // namespace cui::cover_zoom

// ---- 歌詞防誤拖（2026-11-11）----
// 放在這個檔案是為了共用 main.xml 標記的讀取（歌詞欄位置）與分層視窗的貼圖，免得動到專案檔
namespace cui::lyrics_lock {

namespace {

constexpr wchar_t window_class_name[] = L"columns_ui_lyrics_lock";
constexpr char eslyric_window_class[] = "uie_eslyric_wnd_class";

std::vector<HWND>& live_windows()
{
    static std::vector<HWND> windows;
    return windows;
}

// 每個蓋層記著上一次的播放列位置（分割面板座標），設定或模式改了用它重排
std::unordered_map<HWND, RECT>& bar_rects()
{
    static std::unordered_map<HWND, RECT> rects;
    return rects;
}

RECT compute_layout(const RECT& bar_rect)
{
    if (bar_rect.right <= bar_rect.left || !is_enabled())
        return {};

    // 只在展開頁完全展開、而且是歌詞模式時蓋
    RECT stage_screen{};
    HWND owner{};
    if (!panels::splitter::FlatSplitterPanel::s_get_now_playing_stage(stage_screen, owner))
        return {};
    if (main_menu::get_now_playing_mode() != main_menu::NowPlayingMode::lyrics)
        return {};

    const auto column = cover_zoom::lyrics_column_in_stage(bar_rect.right - bar_rect.left, bar_rect.top);
    if (!column)
        return {};

    return RECT{bar_rect.left + column->left, column->top, bar_rect.left + column->right, column->bottom};
}

// 蓋層底下那個 ESLyric 視窗（滾輪、右鍵轉給它）
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

// 滑鼠按鍵訊息（座標是蓋層的用戶區座標）換算成 ESLyric 視窗的座標再送過去
void forward_client_message(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    POINT screen = pt;
    ClientToScreen(wnd, &screen);
    const auto target = find_eslyric_under(wnd, screen);
    if (!target)
        return;
    ScreenToClient(target, &screen);
    SendMessage(target, msg, wp, MAKELPARAM(screen.x, screen.y));
}

void render(HWND wnd)
{
    RECT client{};
    GetClientRect(wnd, &client);
    if (client.right <= 0 || client.bottom <= 0)
        return;

    // 幾乎看不見的底（透明度 1/255）：整塊收得到滑鼠，看起來跟沒蓋一樣
    cover_zoom::present_layered(wnd, client.right, client.bottom, 255, [&](Gdiplus::Graphics& g) {
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
    // 排版批次（defer_layout）可能已經改了大小，所以每次都重貼一次（只是一塊單色，很便宜）
    render(wnd);
    if (!visible)
        SetWindowPos(wnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

LRESULT CALLBACK window_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
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
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_SETCURSOR:
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        return TRUE;
    // 左鍵（拖曳、點擊、雙擊）全部吃掉：歌詞就拉不動、也不會被點到跳時間
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_MOUSEMOVE:
        return 0;
    // 滾輪、右鍵、中鍵照常交給底下的歌詞
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        const POINT screen{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (const auto target = find_eslyric_under(wnd, screen))
            return SendMessage(target, msg, wp, lp);
        return 0;
    }
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
        forward_client_message(wnd, msg, wp, lp);
        return 0;
    }
    return DefWindowProc(wnd, msg, wp, lp);
}

} // namespace

bool is_enabled()
{
    return fb2k::configStore::get()->getConfigBool(config_key, enabled_default);
}

void set_enabled(bool enabled)
{
    fb2k::configStore::get()->setConfigBool(config_key, enabled);
    refresh_layouts();
}

HWND create(HWND parent)
{
    static const auto class_registered = [] {
        WNDCLASSEX window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.style = CS_DBLCLKS;
        window_class.lpfnWndProc = window_proc;
        window_class.hInstance = core_api::get_my_instance();
        window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
        window_class.lpszClassName = window_class_name;
        return RegisterClassEx(&window_class) != 0;
    }();

    if (!class_registered)
        return nullptr;

    return CreateWindowEx(WS_EX_LAYERED, window_class_name, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, parent,
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

} // namespace cui::lyrics_lock
