#include "pch.h"

#include "volume_chip.h"
#include "font_utils.h"
#include "next_track_toast.h"
#include "nocturne_text.h"
#include "output_chip.h"

namespace cui::volume_chip {

namespace {

constexpr wchar_t zone_class_name[] = L"columns_ui_volume_zone";
constexpr wchar_t popup_class_name[] = L"columns_ui_volume_popup";

constexpr UINT_PTR show_timer_id = 1;
constexpr UINT_PTR hide_timer_id = 2;
constexpr UINT_PTR bubble_timer_id = 3;
// 滑過圖示多久彈出、離開圖示與滑桿多久收起、調整後 dB 提示泡留多久（預覽頁的暫定值，使用者看過沒意見）
constexpr UINT show_delay_ms = 150;
constexpr UINT hide_delay_ms = 300;
constexpr UINT bubble_ms = 1200;

// 播放列右邊那串的寬度（DIP，時間欄右緣離播放列右緣），見 volume_chip.h
constexpr int cluster_with_volume_bar = 433;
constexpr int cluster_icon_only = 144;

// 圖示視窗（DIP，乘上系統縮放才是像素）：滑過與點擊的範圍 48×48，反白框只有 36×36（跟滑桿同寬）。
// 中心位置照 main.xml：右留白 12＋靜音鈕格寬 64 的一半＝離播放列右緣 44；垂直跟輸出裝置顯示同一條中線（比播放列正中央低 4）
constexpr int zone_size_dip = 48;
constexpr int zone_center_from_bar_right_dip = 44;
constexpr int vertical_offset_dip = 4;
constexpr int highlight_size_dip = 36;
constexpr int highlight_radius_dip = 10;
constexpr float icon_font_dip = 24;

// 彈出的滑桿（DIP）：小膠囊 36 寬，軌道長 140，上下各留 12；底邊離圖示範圍上緣 4。左邊留一塊空位給 dB 提示泡
constexpr int pill_width_dip = 36;
constexpr int pill_padding_dip = 12;
constexpr int track_length_dip = 140;
constexpr float track_width_dip = 4;
constexpr float thumb_diameter_dip = 12;
constexpr int gap_above_zone_dip = 4;
constexpr int shadow_dip = 10;
constexpr int bubble_room_dip = 90;
constexpr int bubble_gap_dip = 8;
constexpr int bubble_height_dip = 20;
constexpr float bubble_font_dip = 12;

// 滾輪每格調幾 dB
constexpr float wheel_step_db = 2.0f;

const Gdiplus::Color icon_colour(255, 216, 216, 216);
// 靜音：柔和的玫瑰紅（跟輸出裝置顯示的靜音同色）
const Gdiplus::Color mute_colour(255, 214, 134, 134);
const Gdiplus::Color pill_fill_colour(230, 24, 24, 24);
const Gdiplus::Color pill_border_colour(26, 255, 255, 255);
const Gdiplus::Color track_colour(255, 74, 74, 74);
const Gdiplus::Color bright_colour(255, 241, 241, 241);
const Gdiplus::Color muted_grey_colour(255, 138, 138, 138);
const Gdiplus::Color bubble_text_colour(255, 224, 224, 224);

int dip(int value)
{
    return uih::scale_dpi_value(value);
}

float dip_f(float value)
{
    return value * static_cast<float>(uih::scale_dpi_value(1000)) / 1000.0f;
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

// 把 draw 畫好的內容用 UpdateLayeredWindow 貼到 wnd（32 位元預乘 alpha 的 DIB）；跟 output_chip.cpp 同一套
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
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(wnd, screen_dc.get(), const_cast<POINT*>(origin), &size, memory_dc.get(), &source, 0, &blend, ULW_ALPHA);
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

// ---- 音量：刻度與 Columns UI／ESLyric 內建的音量條相同，位置＝2^(dB/10)（0 dB 在最上、−100 dB 在最下）----
double db_to_position(float db)
{
    if (db <= -100.0f)
        return 0.0;
    return std::clamp(std::pow(2.0, db / 10.0), 0.0, 1.0);
}

float position_to_db(double position)
{
    if (position <= 0.001)
        return -100.0f;
    return std::clamp(static_cast<float>(10.0 * std::log2(position)), -100.0f, 0.0f);
}

bool is_muted(float db)
{
    return db <= -99.9f;
}

// ---- 自動判斷要不要進圖示模式（2026-11-06）----
// 設定頁的勾選項只能「強制開」；預設看版面檔 main.xml：還有 ESLyric 的音量條（<volume>）就維持原樣，沒有就自動進入圖示模式。
// 這樣公開版的更新版只要換 DLL 與 main.xml，使用者不用去勾選項，也不會出現「換了沒音量條的 main.xml、設定卻沒開」而整個沒有音量控制。
// 版面檔位置固定在 profile\eslyric-data\layout\fb2k-modern\main.xml（所有環境都是這個名稱）；找不到或讀不到就當作有音量條（維持原樣）。
// 註解（<!-- -->）裡提到 <volume> 不算。檔案最後修改時間變了才重讀，而且最多每秒檢查一次
std::wstring layout_file_path()
{
    const pfc::string8 native = ::filesystem::g_get_native_path(core_api::get_profile_path());
    return std::wstring(pfc::stringcvt::string_wide_from_utf8(native.c_str()).get_ptr())
        + L"\\eslyric-data\\layout\\fb2k-modern\\main.xml";
}

bool xml_has_volume_element(std::string text)
{
    for (auto open = text.find("<!--"); open != std::string::npos; open = text.find("<!--")) {
        const auto close = text.find("-->", open + 4);
        text.erase(open, close == std::string::npos ? std::string::npos : close + 3 - open);
    }

    for (auto pos = text.find("<volume"); pos != std::string::npos; pos = text.find("<volume", pos + 1)) {
        const auto next = pos + 7;
        if (next >= text.size())
            return true;
        const auto c = text[next];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '/' || c == '>')
            return true;
    }
    return false;
}

struct LayoutProbe {
    bool valid{};
    bool no_volume_bar{};
    FILETIME stamp{};
    ULONGLONG checked_at{};
};
LayoutProbe g_layout_probe;

bool layout_has_no_volume_bar()
{
    const auto now = GetTickCount64();
    if (g_layout_probe.valid && now - g_layout_probe.checked_at < 1000)
        return g_layout_probe.no_volume_bar;
    g_layout_probe.checked_at = now;

    try {
        const auto path = layout_file_path();
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) {
            g_layout_probe.valid = true;
            g_layout_probe.no_volume_bar = false;
            return false;
        }
        if (g_layout_probe.valid && CompareFileTime(&info.ftLastWriteTime, &g_layout_probe.stamp) == 0)
            return g_layout_probe.no_volume_bar;

        std::string text;
        FILE* file{};
        if (_wfopen_s(&file, path.c_str(), L"rb") == 0 && file) {
            char buffer[8192];
            size_t read{};
            while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0 && text.size() < (size_t{4} << 20))
                text.append(buffer, read);
            fclose(file);
            g_layout_probe.no_volume_bar = !xml_has_volume_element(std::move(text));
        } else {
            g_layout_probe.no_volume_bar = false;
        }
        g_layout_probe.stamp = info.ftLastWriteTime;
        g_layout_probe.valid = true;
    }
    catch (...) {
        g_layout_probe.valid = true;
        g_layout_probe.no_volume_bar = false;
    }
    return g_layout_probe.no_volume_bar;
}

// 圖示模式：設定頁強制開，或版面檔已經沒有音量條
bool icon_mode()
{
    return enabled.get() || layout_has_no_volume_bar();
}

// 滾輪調音量：高解析滾輪一格不到 120，累積到一格才動
void nudge_volume(int wheel_delta)
{
    static int remainder = 0;
    remainder += wheel_delta;
    const auto notches = remainder / WHEEL_DELTA;
    if (notches == 0)
        return;
    remainder -= notches * WHEEL_DELTA;

    const auto control = playback_control::get();
    control->set_volume(std::clamp(control->get_volume() + wheel_step_db * static_cast<float>(notches), -100.0f, 0.0f));
}

class Zone;
// 目前彈出滑桿所屬的圖示視窗
Zone* g_active_zone{};

// 游標在哪：圖示、滑桿，或正在拖曳（拖曳時游標可以跑到滑桿外面，滑桿不收）。決定要不要彈出、要不要收起
struct HoverState {
    bool zone{};
    bool popup{};
    bool dragging{};
};
HoverState g_hover;

void schedule_hover(HWND zone_wnd);

// ---------------------------------------------------------------------------------------------------------------
// 彈出滑桿：圖示正上方的小膠囊（分層頂層視窗，不搶焦點）
// - 視窗比膠囊大：左邊留一塊給 dB 提示泡（那一塊完全透明，滑鼠會穿過去），膠囊四周的陰影範圍鋪一層幾乎看不見的底，
//   滑鼠才碰得到；底邊緊貼圖示範圍上緣，從圖示移上去的路上不會經過沒人管的空隙
// ---------------------------------------------------------------------------------------------------------------
class Popup {
public:
    static Popup& instance()
    {
        static Popup popup;
        return popup;
    }

    static LRESULT CALLBACK s_wndproc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        return instance().on_message(wnd, msg, wp, lp);
    }

    bool visible() const { return m_wnd && IsWindowVisible(m_wnd); }

    // zone_wnd：圖示視窗；zone_rect：它在螢幕上的位置
    void show(HWND zone_wnd, const RECT& zone_rect)
    {
        if (!ensure_window(zone_wnd))
            return;

        m_zone_wnd = zone_wnd;
        compute_metrics();

        const auto centre_x = (zone_rect.left + zone_rect.right) / 2;
        auto x = static_cast<LONG>(centre_x - (m_pill_x + m_pill_w / 2));
        auto y = static_cast<LONG>(zone_rect.top - m_height);

        // 不超出該螢幕的工作區（提示泡的空位可以伸出螢幕左邊，那一塊是空的）
        const auto monitor = MonitorFromRect(&zone_rect, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitor_info{sizeof(monitor_info)};
        GetMonitorInfo(monitor, &monitor_info);
        x = std::clamp<LONG>(x, monitor_info.rcWork.left - m_room, monitor_info.rcWork.right - m_width + m_shadow);
        y = std::max<LONG>(y, monitor_info.rcWork.top - m_shadow);
        m_origin = {x, y};

        render();
        ShowWindow(m_wnd, SW_SHOWNOACTIVATE);
    }

    void hide()
    {
        if (m_dragging) {
            m_dragging = false;
            if (GetCapture() == m_wnd)
                ReleaseCapture();
        }
        g_hover.popup = false;
        g_hover.dragging = false;
        m_bubble = false;
        if (m_zone_wnd)
            KillTimer(m_zone_wnd, bubble_timer_id);
        if (m_wnd && IsWindowVisible(m_wnd))
            ShowWindow(m_wnd, SW_HIDE);
    }

    void destroy()
    {
        hide();
        if (m_wnd)
            DestroyWindow(std::exchange(m_wnd, nullptr));
        m_zone_wnd = nullptr;
    }

    void refresh()
    {
        if (visible())
            render();
    }

    // 調整音量時讓 dB 提示泡出現，停手 bubble_ms 之後由圖示視窗的計時器收掉
    void bump_bubble()
    {
        m_bubble = true;
        if (m_zone_wnd)
            SetTimer(m_zone_wnd, bubble_timer_id, bubble_ms, nullptr);
        refresh();
    }

    void clear_bubble()
    {
        if (!m_bubble)
            return;
        m_bubble = false;
        refresh();
    }

private:
    bool ensure_window(HWND zone_wnd)
    {
        const auto owner = GetAncestor(zone_wnd, GA_ROOT);

        if (m_wnd && m_owner != owner)
            destroy();

        if (m_wnd)
            return true;

        static const auto class_registered = [] {
            WNDCLASSEX window_class{};
            window_class.cbSize = sizeof(window_class);
            window_class.lpfnWndProc = &Popup::s_wndproc;
            window_class.hInstance = core_api::get_my_instance();
            window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
            window_class.lpszClassName = popup_class_name;
            return RegisterClassEx(&window_class) != 0;
        }();

        if (!class_registered)
            return false;

        // 有擁有者的彈出視窗（永遠在主視窗上面）；不搶焦點，拖曳滑桿時主視窗的標題列照樣顯示為使用中
        m_wnd = CreateWindowEx(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, popup_class_name, L"", WS_POPUP, 0, 0, 1,
            1, owner, nullptr, core_api::get_my_instance(), nullptr);
        m_owner = owner;
        return m_wnd != nullptr;
    }

    // 每次彈出時重算（縮放可能變了）
    void compute_metrics()
    {
        m_shadow = dip(shadow_dip);
        m_room = dip(bubble_room_dip);
        m_pill_w = dip(pill_width_dip);
        m_pad = dip(pill_padding_dip);
        m_track_len = dip(track_length_dip);
        m_pill_h = 2 * m_pad + m_track_len;
        m_pill_x = m_room + m_shadow;
        m_pill_y = m_shadow;
        m_width = m_room + 2 * m_shadow + m_pill_w;
        m_height = m_shadow + m_pill_h + dip(gap_above_zone_dip);
        m_track_top = m_pill_y + m_pad;
        m_track_bottom = m_track_top + m_track_len;
    }

    void render()
    {
        if (!m_wnd)
            return;
        present_layered(m_wnd, &m_origin, m_width, m_height, [this](Gdiplus::Graphics& g) { draw(g); });
    }

    void draw(Gdiplus::Graphics& g) const
    {
        const auto volume = playback_control::get()->get_volume();
        const auto muted = is_muted(volume);
        const auto position = db_to_position(volume);

        // 幾乎看不見的底（透明度 1/255）：分層視窗透明度是 0 的像素滑鼠會穿過去。左邊提示泡的空位不鋪
        {
            const Gdiplus::SolidBrush hit_brush(Gdiplus::Color(1, 24, 24, 24));
            g.FillRectangle(&hit_brush, m_room, 0, m_pill_w + 2 * m_shadow, m_height);
        }

        const Gdiplus::RectF body(static_cast<float>(m_pill_x), static_cast<float>(m_pill_y), static_cast<float>(m_pill_w),
            static_cast<float>(m_pill_h));
        const auto radius = static_cast<float>(m_pill_w) / 2.0f;

        // 柔和陰影：往外擴的幾層很淡的圓角框
        for (int i = m_shadow; i >= 1; i -= std::max(1, m_shadow / 5)) {
            Gdiplus::GraphicsPath path;
            add_round_rect(path,
                Gdiplus::RectF(body.X - i, body.Y - i + m_shadow / 3.f, body.Width + 2.f * i, body.Height + 2.f * i),
                radius + i);
            const Gdiplus::SolidBrush brush(Gdiplus::Color(static_cast<BYTE>(10 + (m_shadow - i) * 2), 0, 0, 0));
            g.FillPath(&brush, &path);
        }

        Gdiplus::GraphicsPath body_path;
        add_round_rect(body_path, body, radius);
        const Gdiplus::SolidBrush fill(pill_fill_colour);
        g.FillPath(&fill, &body_path);
        const Gdiplus::Pen border(pill_border_colour, 1.f);
        g.DrawPath(&border, &body_path);

        // 軌道、已填的部分、圓點
        const auto track_w = std::max(2.0f, dip_f(track_width_dip));
        const auto centre_x = static_cast<float>(m_pill_x) + static_cast<float>(m_pill_w) / 2.0f;
        const auto thumb_y = static_cast<float>(m_track_bottom) - static_cast<float>(position) * static_cast<float>(m_track_len);
        const auto level_colour = muted ? muted_grey_colour : bright_colour;
        {
            Gdiplus::GraphicsPath track;
            add_round_rect(track,
                Gdiplus::RectF(centre_x - track_w / 2, static_cast<float>(m_track_top), track_w, static_cast<float>(m_track_len)),
                track_w / 2);
            const Gdiplus::SolidBrush brush(track_colour);
            g.FillPath(&brush, &track);
        }
        if (static_cast<float>(m_track_bottom) - thumb_y > track_w) {
            Gdiplus::GraphicsPath level;
            add_round_rect(level,
                Gdiplus::RectF(centre_x - track_w / 2, thumb_y, track_w, static_cast<float>(m_track_bottom) - thumb_y),
                track_w / 2);
            const Gdiplus::SolidBrush brush(level_colour);
            g.FillPath(&brush, &level);
        }
        {
            const auto d = dip_f(thumb_diameter_dip);
            const Gdiplus::SolidBrush brush(level_colour);
            g.FillEllipse(&brush, centre_x - d / 2, thumb_y - d / 2, d, d);
        }

        if (m_bubble)
            draw_bubble(g, volume, muted, thumb_y);
    }

    // dB 提示泡：貼在圓點左邊、高度跟著圓點
    void draw_bubble(Gdiplus::Graphics& g, float volume, bool muted, float thumb_y) const
    {
        std::wstring text;
        if (muted) {
            text = nocturne_text::volume_muted;
        } else {
            wchar_t buffer[32]{};
            swprintf_s(buffer, L"%.2f dB", volume);
            text = buffer;
        }

        const auto family = create_font_family();
        const Gdiplus::Font font(family.get(), dip_f(bubble_font_dip), pick_font_style(*family), Gdiplus::UnitPixel);
        Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
        format.SetAlignment(Gdiplus::StringAlignmentCenter);
        format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap | Gdiplus::StringFormatFlagsNoClip);

        Gdiplus::RectF measured;
        g.MeasureString(text.c_str(), -1, &font, Gdiplus::PointF(0, 0), &format, &measured);

        const auto padding = static_cast<float>(dip(8));
        const auto width = std::min(measured.Width + 2 * padding, static_cast<float>(m_room - dip(bubble_gap_dip)));
        const auto height = static_cast<float>(dip(bubble_height_dip));
        const auto right = static_cast<float>(m_pill_x - dip(bubble_gap_dip));
        const Gdiplus::RectF rect(right - width, thumb_y - height / 2, width, height);

        Gdiplus::GraphicsPath path;
        add_round_rect(path, rect, static_cast<float>(dip(6)));
        const Gdiplus::SolidBrush fill(Gdiplus::Color(230, 24, 24, 24));
        g.FillPath(&fill, &path);
        const Gdiplus::Pen border(Gdiplus::Color(31, 255, 255, 255), 1.f);
        g.DrawPath(&border, &path);

        const Gdiplus::SolidBrush brush(muted ? mute_colour : bubble_text_colour);
        g.DrawString(text.c_str(), -1, &font, rect, &format, &brush);
    }

    // 軌道（含圓點半徑的上下延伸）的點擊範圍：膠囊整個寬度都算
    bool in_track(int x, int y) const
    {
        const auto extra = static_cast<int>(std::ceil(dip_f(thumb_diameter_dip) / 2));
        return x >= m_pill_x && x < m_pill_x + m_pill_w && y >= m_track_top - extra && y <= m_track_bottom + extra;
    }

    void set_from_y(int y)
    {
        const auto position = std::clamp(
            static_cast<double>(m_track_bottom - y) / static_cast<double>(std::max(1, m_track_len)), 0.0, 1.0);
        playback_control::get()->set_volume(position_to_db(position));
        m_bubble = true;
        if (m_zone_wnd)
            SetTimer(m_zone_wnd, bubble_timer_id, bubble_ms, nullptr);
        render();
    }

    void end_drag()
    {
        if (!m_dragging)
            return;

        m_dragging = false;
        g_hover.dragging = false;
        if (GetCapture() == m_wnd)
            ReleaseCapture();

        // 拖曳中游標可能已經在滑桿外面：以游標實際位置為準，決定要不要收起
        POINT cursor{};
        GetCursorPos(&cursor);
        g_hover.popup = WindowFromPoint(cursor) == m_wnd;
        if (m_zone_wnd)
            schedule_hover(m_zone_wnd);
    }

    LRESULT on_message(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_ERASEBKGND:
            return 1;
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
            if (!g_hover.popup) {
                g_hover.popup = true;
                if (m_zone_wnd)
                    schedule_hover(m_zone_wnd);
            }
            if (m_dragging)
                set_from_y(GET_Y_LPARAM(lp));
            return 0;
        case WM_MOUSELEAVE:
            m_tracking = false;
            g_hover.popup = false;
            if (m_zone_wnd)
                schedule_hover(m_zone_wnd);
            return 0;
        case WM_LBUTTONDOWN: {
            const auto x = GET_X_LPARAM(lp);
            const auto y = GET_Y_LPARAM(lp);
            if (in_track(x, y)) {
                m_dragging = true;
                g_hover.dragging = true;
                SetCapture(wnd);
                set_from_y(y);
            }
            return 0;
        }
        case WM_LBUTTONUP:
            end_drag();
            return 0;
        case WM_CAPTURECHANGED:
            if (m_dragging)
                end_drag();
            break;
        case WM_MOUSEWHEEL:
            nudge_volume(GET_WHEEL_DELTA_WPARAM(wp));
            bump_bubble();
            return 0;
        case WM_SETCURSOR:
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
            return TRUE;
        }

        return DefWindowProc(wnd, msg, wp, lp);
    }

    HWND m_wnd{};
    HWND m_owner{};
    HWND m_zone_wnd{};
    POINT m_origin{};
    bool m_tracking{};
    bool m_dragging{};
    bool m_bubble{};
    // 這一次彈出時算好的尺寸（像素）：視窗、膠囊、軌道
    int m_shadow{};
    int m_room{};
    int m_pill_w{};
    int m_pill_h{};
    int m_pill_x{};
    int m_pill_y{};
    int m_pad{};
    int m_track_len{};
    int m_track_top{};
    int m_track_bottom{};
    int m_width{};
    int m_height{};
};

// 依游標在哪排程：圖示或滑桿上有游標（或正在拖曳）就取消收起、必要時排彈出；都沒有就排收起。計時器掛在圖示視窗上
void schedule_hover(HWND zone_wnd)
{
    const auto over = g_hover.zone || g_hover.popup || g_hover.dragging;

    if (over) {
        KillTimer(zone_wnd, hide_timer_id);
        if (!Popup::instance().visible() && g_hover.zone)
            SetTimer(zone_wnd, show_timer_id, show_delay_ms, nullptr);
    } else {
        KillTimer(zone_wnd, show_timer_id);
        if (Popup::instance().visible())
            SetTimer(zone_wnd, hide_timer_id, hide_delay_ms, nullptr);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// 圖示視窗：分割面板的分層子視窗，蓋在 main.xml 留給靜音鈕的位置上
// ---------------------------------------------------------------------------------------------------------------
std::vector<Zone*>& live_zones()
{
    static std::vector<Zone*> zones;
    return zones;
}

class Zone {
public:
    static LRESULT CALLBACK s_wndproc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp);
    static Zone* from(HWND wnd) { return reinterpret_cast<Zone*>(GetWindowLongPtr(wnd, GWLP_USERDATA)); }

    void set_bar_rect(const RECT& rect) { m_bar_rect = rect; }
    // 給排版批次（defer_layout）用：視窗該放的位置（分割面板座標），空矩形＝要藏起來
    RECT target_rect() { return compute_layout(); }

    void apply_layout()
    {
        const auto target = compute_layout();

        if (IsRectEmpty(&target)) {
            if (IsWindowVisible(m_wnd)) {
                hide_popup();
                ShowWindow(m_wnd, SW_HIDE);
            }
            return;
        }

        RECT current{};
        GetWindowRect(m_wnd, &current);
        MapWindowPoints(HWND_DESKTOP, GetParent(m_wnd), reinterpret_cast<POINT*>(&current), 2);

        if (!(IsWindowVisible(m_wnd) && EqualRect(&current, &target))) {
            SetWindowPos(m_wnd, HWND_TOP, target.left, target.top, target.right - target.left,
                target.bottom - target.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }

        render();
    }

    void render()
    {
        if (!m_wnd || !IsWindowVisible(m_wnd))
            return;

        RECT client{};
        GetClientRect(m_wnd, &client);
        if (client.right <= 0 || client.bottom <= 0)
            return;

        present_layered(m_wnd, nullptr, client.right, client.bottom,
            [&](Gdiplus::Graphics& g) { draw(g, client.right, client.bottom); });
    }

    void show_popup()
    {
        if (!icon_mode() || !m_wnd || !IsWindowVisible(m_wnd))
            return;

        RECT rect{};
        GetWindowRect(m_wnd, &rect);
        g_active_zone = this;
        Popup::instance().show(m_wnd, rect);
        render();
    }

    void hide_popup()
    {
        KillTimer(m_wnd, show_timer_id);
        KillTimer(m_wnd, hide_timer_id);
        Popup::instance().hide();
        if (g_active_zone == this)
            g_active_zone = nullptr;
        render();
    }

private:
    RECT compute_layout()
    {
        // 不是圖示模式（設定頁沒強制開、而且 main.xml 還有音量條）就整個藏起來，各環境照舊用 main.xml 的音量條
        if (!icon_mode() || m_bar_rect.right <= m_bar_rect.left)
            return {};

        // GDI+ 的字型表還在背景暖機：這時在主執行緒建字型，會讓整個介面晚出現（跟輸出裝置顯示同一個問題），先藏起來，
        // 暖機做完會重排（見 create()）
        if (!next_track_toast::gdiplus_fonts_ready())
            return {};

        const auto size = dip(zone_size_dip);
        const auto bar_height = m_bar_rect.bottom - m_bar_rect.top;
        const auto left = m_bar_rect.right - dip(zone_center_from_bar_right_dip) - size / 2;
        const auto top = m_bar_rect.top + (bar_height - size) / 2 + dip(vertical_offset_dip);
        return {left, top, left + size, top + size};
    }

    void draw(Gdiplus::Graphics& g, int width, int height) const
    {
        // 幾乎看不見的底（透明度 1/255），滑鼠才碰得到
        {
            const Gdiplus::SolidBrush hit_brush(Gdiplus::Color(1, 24, 24, 24));
            g.FillRectangle(&hit_brush, 0, 0, width, height);
        }

        const auto lit = m_hot || (g_active_zone == this && Popup::instance().visible());
        if (lit) {
            const auto size = static_cast<float>(dip(highlight_size_dip));
            Gdiplus::GraphicsPath path;
            add_round_rect(path,
                Gdiplus::RectF((static_cast<float>(width) - size) / 2, (static_cast<float>(height) - size) / 2, size, size),
                static_cast<float>(dip(highlight_radius_dip)));
            const Gdiplus::SolidBrush brush(Gdiplus::Color(m_pressed ? 40 : 23, 255, 255, 255));
            g.FillPath(&brush, &path);
        }

        const auto family = icon_family();
        if (!family)
            return;

        const auto muted = is_muted(playback_control::get()->get_volume());
        const Gdiplus::Font font(family, dip_f(icon_font_dip), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
        format.SetAlignment(Gdiplus::StringAlignmentCenter);
        format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap | Gdiplus::StringFormatFlagsNoClip);
        const Gdiplus::SolidBrush brush(muted ? mute_colour : icon_colour);
        // E767＝音量、E74F＝靜音
        g.DrawString(muted ? L"" : L"", -1, &font,
            Gdiplus::RectF(0.f, 0.f, static_cast<float>(width), static_cast<float>(height)), &format, &brush);
    }

    LRESULT on_message(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg) {
        case WM_NCCREATE:
            m_wnd = wnd;
            SetWindowLongPtr(wnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
            live_zones().push_back(this);
            break;
        case WM_NCDESTROY: {
            if (g_active_zone == this) {
                Popup::instance().hide();
                g_active_zone = nullptr;
            }
            auto& zones = live_zones();
            zones.erase(std::remove(zones.begin(), zones.end(), this), zones.end());
            if (zones.empty())
                Popup::instance().destroy();
            SetWindowLongPtr(wnd, GWLP_USERDATA, 0);
            delete this;
            return DefWindowProc(wnd, msg, wp, lp);
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            // 分層視窗的畫面由 render() 貼上去，這裡只要把更新區域標成已處理
            PAINTSTRUCT ps{};
            BeginPaint(wnd, &ps);
            EndPaint(wnd, &ps);
            return 0;
        }
        case WM_SHOWWINDOW:
            if (!wp)
                hide_popup();
            break;
        case WM_MOUSEMOVE:
            if (!m_tracking) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, wnd, 0};
                if (TrackMouseEvent(&tme))
                    m_tracking = true;
            }
            if (!m_hot) {
                m_hot = true;
                g_hover.zone = true;
                schedule_hover(wnd);
                render();
            }
            return 0;
        case WM_MOUSELEAVE:
            m_tracking = false;
            // 按著滑鼠時（有捕捉）游標可能先跑到外面，放開時再依實際位置判斷
            if (!m_pressed) {
                m_hot = false;
                g_hover.zone = false;
                schedule_hover(wnd);
                render();
            }
            return 0;
        case WM_LBUTTONDOWN:
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
            // 點一下＝靜音切換（原本 ESLyric 靜音鈕的功能）
            if (was_pressed && PtInRect(&client, pt))
                playback_control::get()->volume_mute_toggle();

            POINT cursor{};
            GetCursorPos(&cursor);
            m_hot = WindowFromPoint(cursor) == wnd;
            g_hover.zone = m_hot;
            schedule_hover(wnd);
            render();
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (m_pressed) {
                m_pressed = false;
                render();
            }
            break;
        case WM_MOUSEWHEEL:
            nudge_volume(GET_WHEEL_DELTA_WPARAM(wp));
            show_popup();
            Popup::instance().bump_bubble();
            return 0;
        case WM_TIMER:
            if (wp == show_timer_id) {
                KillTimer(wnd, show_timer_id);
                if (g_hover.zone || g_hover.popup)
                    show_popup();
                return 0;
            }
            if (wp == hide_timer_id) {
                KillTimer(wnd, hide_timer_id);
                if (!(g_hover.zone || g_hover.popup || g_hover.dragging))
                    hide_popup();
                return 0;
            }
            if (wp == bubble_timer_id) {
                KillTimer(wnd, bubble_timer_id);
                Popup::instance().clear_bubble();
                return 0;
            }
            break;
        case WM_SETCURSOR:
            SetCursor(LoadCursor(nullptr, IDC_HAND));
            return TRUE;
        }

        return DefWindowProc(wnd, msg, wp, lp);
    }

    HWND m_wnd{};
    RECT m_bar_rect{};
    bool m_hot{};
    bool m_tracking{};
    bool m_pressed{};
};

LRESULT CALLBACK Zone::s_wndproc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Zone* zone = from(wnd);

    if (msg == WM_NCCREATE) {
        zone = new Zone();
        return zone->on_message(wnd, msg, wp, lp);
    }

    if (!zone)
        return DefWindowProc(wnd, msg, wp, lp);

    return zone->on_message(wnd, msg, wp, lp);
}

// 別的地方改了音量（鍵盤、媒體鍵、別的音量控制項）也要跟著更新圖示與滑桿
class VolumePlayCallback : public play_callback_static {
public:
    unsigned get_flags() override { return flag_on_volume_change; }

    void on_playback_starting(play_control::t_track_command command, bool paused) override {}
    void on_playback_new_track(metadb_handle_ptr track) override {}
    void on_playback_stop(play_control::t_stop_reason reason) override {}
    void on_playback_seek(double time) override {}
    void on_playback_pause(bool state) override {}
    void on_playback_edited(metadb_handle_ptr track) override {}
    void on_playback_dynamic_info(const file_info& info) override {}
    void on_playback_dynamic_info_track(const file_info& info) override {}
    void on_playback_time(double time) override {}
    void on_volume_change(float new_value) override
    {
        for (const auto zone : live_zones())
            zone->render();
        Popup::instance().refresh();
    }
};

play_callback_static_factory_t<VolumePlayCallback> volume_play_callback;

} // namespace

int time_col_right_dip()
{
    return icon_mode() ? cluster_icon_only : cluster_with_volume_bar;
}

void refresh_layouts()
{
    for (const auto zone : live_zones())
        zone->apply_layout();
}

HWND create(HWND parent)
{
    static const auto class_registered = [] {
        WNDCLASSEX window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.style = CS_HREDRAW | CS_VREDRAW;
        window_class.lpfnWndProc = Zone::s_wndproc;
        window_class.hInstance = core_api::get_my_instance();
        window_class.hCursor = LoadCursor(nullptr, IDC_HAND);
        window_class.lpszClassName = zone_class_name;
        return RegisterClassEx(&window_class) != 0;
    }();

    if (!class_registered)
        return nullptr;

    const auto wnd = CreateWindowEx(WS_EX_LAYERED, zone_class_name, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, parent,
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
    if (const auto zone = Zone::from(wnd)) {
        zone->set_bar_rect(bar_rect);
        zone->apply_layout();
    }
}

HDWP defer_layout(HDWP dwp, HWND wnd, const RECT& bar_rect)
{
    const auto zone = Zone::from(wnd);
    if (!zone)
        return dwp;

    zone->set_bar_rect(bar_rect);
    const auto target = zone->target_rect();

    if (IsRectEmpty(&target)) {
        if (IsWindowVisible(wnd))
            return DeferWindowPos(dwp, wnd, nullptr, 0, 0, 0, 0,
                SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return dwp;
    }

    return DeferWindowPos(dwp, wnd, HWND_TOP, target.left, target.top, target.right - target.left,
        target.bottom - target.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

// 設定頁的開關：寫入（勾選、Reset page 都算）時重排圖示，輸出裝置顯示的位置也跟著變（右邊那串的寬度不同）
fbh::ConfigBool enabled(GUID{0x7c3d9e12, 0x5b84, 0x4f6a, {0x91, 0x2e, 0xa4, 0x08, 0x6d, 0xf3, 0x5c, 0x7b}}, false,
    [](bool, bool, std::optional<uint32_t>) {
        refresh_layouts();
        output_chip::refresh_layouts();
    });

} // namespace cui::volume_chip
