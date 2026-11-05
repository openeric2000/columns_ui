#include "pch.h"

#include <timeapi.h>

#include "output_chip.h"
#include "fb2k_misc.h"
#include "font_utils.h"
#include "nocturne_text.h"

#pragma comment(lib, "winmm.lib")

namespace cui::output_chip {

namespace {

constexpr wchar_t chip_class_name[] = L"columns_ui_output_chip";
constexpr wchar_t panel_class_name[] = L"columns_ui_output_chip_panel";
constexpr wchar_t menu_class_name[] = L"columns_ui_output_chip_menu";
constexpr UINT_PTR hover_timer_id = 1;
constexpr UINT hover_delay_ms = 350;
constexpr UINT_PTR slide_timer_id = 2;
constexpr UINT slide_interval_ms = 10;
// 開始播放時裝置顯示往左推出來的時間，以及停止後往右縮回停止鈕旁的時間（回去慢一點、柔和一點：先慢後快再慢，2026-11-05 使用者指示）
constexpr double slide_out_seconds = 0.22;
constexpr double slide_back_seconds = 0.6;

// 尺寸（DIP，乘上系統縮放才是像素），照預覽 mockups\fb2k-output-device-bottombar-mockup.html
constexpr int chip_height = 36;
constexpr int chip_padding = 8;
constexpr int tag_height = 20;
// 選單最上面的標題列高度（標題字級用 tag_font_dip）；標題底下有一條跟群組分隔線同款的線（2026-11-05 預覽選的樣式 B）
constexpr int menu_title_height = 28;
constexpr int tag_padding_x = 7;
// 類型標記與名字之間的空隙：原本 8，使用者覺得太靠近、要多 1～2 px（2026-11-05），125% 下 8→11.75 DIP 約多 2 px
constexpr float tag_name_gap_dip = 9.4f;
constexpr float tag_font_dip = 12;
constexpr float name_font_dip = 14;
constexpr int min_name_width = 40;
// 位置照 main.xml（219）：時間欄（及其右邊的停止、靜音、音量、dB）的右緣離播放列右緣 433 DIP——2026-11-05 在 219 量畫面（125%）得到：
// 停止鈕圖示中心離播放列右緣 401 DIP、鈕格 64 寬，所以時間欄的右緣在 433；時間文字靠右、再離右邊 16，字級 16
// （「0:02 / 2:27」約 87 DIP 寬，跟 GDI+ 量 16 DIP 的結果一致；播放中時間文字右緣離播放列右緣 449 DIP，也對得上）。
// 裝置顯示的右緣貼在「時間文字」的左邊（空 time_gap＋自己的內距）；文字寬度用目前曲長的格式估：已播放時間的格式不會比曲長的更長，
// 所以最長的檔案（11:52:32，約 152 DIP）也不會碰到（分「不到一小時」「一小時以上」兩種寬度）。沒在播放時時間欄是空的，寬度當 0，裝置顯示就縮到停止鈕旁邊。換歌、開始、停止時重排。
// main.xml 時間欄右邊那一串的寬度、時間字級改了，這裡要跟著改（169、公開版的 main.xml 還沒量，用前要先量）
constexpr int time_col_right_from_bar_right = 433;
constexpr int time_right_margin = 16;
constexpr float time_font_dip = 16;
constexpr int time_gap = 6;
// 播放列裡的控制項是垂直置中在（播放列高度 − 上面 8 的留白）裡，比播放列正中央低約 4 DIP（量時間文字與中間按鈕得到）
constexpr int vertical_offset = 4;
// 左邊界：播放列正中間再往右這麼多（中間那組按鈕到循環鍵右緣約 134，再留空隙；169 的 1920 寬螢幕右邊界在 1260，留越多名字越容易被擠掉）
constexpr int center_clear = 150;

// 滑過面板
constexpr int panel_padding_x = 14;
constexpr int panel_padding_top = 10;
constexpr int panel_padding_bottom = 11;
constexpr int panel_min_width = 250;
constexpr int panel_radius = 10;
constexpr int panel_shadow = 10;
constexpr int panel_gap_above_chip = 8;
constexpr float panel_small_font_dip = 12;
constexpr float panel_name_font_dip = 14;

const Gdiplus::Color background_colour(255, 24, 24, 24);
const Gdiplus::Color amber_colour(255, 232, 184, 90);
const Gdiplus::Color amber_border_colour(255, 217, 164, 65);
// 靜音：柔和的玫瑰紅（2026-11-05 使用者要求靜音也換顏色，一樣要柔和，跟琥珀色的亮度相近）
const Gdiplus::Color mute_colour(255, 214, 134, 134);
const Gdiplus::Color mute_border_colour(255, 196, 112, 112);

// 類型標記的色調：一般（灰白）、琥珀（ASIO、獨占：獨佔裝置）、靜音（玫瑰紅）
enum class Tone { normal, amber, mute };

Gdiplus::Color tone_text_colour(Tone tone, const Gdiplus::Color& normal)
{
    return tone == Tone::amber ? amber_colour : tone == Tone::mute ? mute_colour : normal;
}

Gdiplus::Color tone_border_colour(Tone tone)
{
    return tone == Tone::amber ? amber_border_colour : tone == Tone::mute ? mute_border_colour
                                                                         : Gdiplus::Color(102, 241, 241, 241);
}

int dip(int value)
{
    return uih::scale_dpi_value(value);
}

float dip_f(float value)
{
    return value * static_cast<float>(uih::scale_dpi_value(1000)) / 1000.0f;
}

int tag_gap_px()
{
    return static_cast<int>(std::lround(dip_f(tag_name_gap_dip)));
}

std::wstring widen(const char* text)
{
    return pfc::stringcvt::string_wide_from_utf8(text).get_ptr();
}

bool ends_with(const std::wstring& text, std::wstring_view suffix)
{
    return text.size() >= suffix.size() && std::wstring_view(text).substr(text.size() - suffix.size()) == suffix;
}

void trim(std::wstring& text)
{
    while (!text.empty() && (text.back() == L' ' || text.back() == L'\t'))
        text.pop_back();
    size_t start = 0;
    while (start < text.size() && (text[start] == L' ' || text[start] == L'\t'))
        ++start;
    text.erase(0, start);
}

bool contains_no_case(const std::wstring& text, std::wstring_view needle)
{
    const auto it = std::search(text.begin(), text.end(), needle.begin(), needle.end(),
        [](wchar_t a, wchar_t b) { return std::towupper(a) == std::towupper(b); });
    return it != text.end();
}

// 一個輸出裝置在畫面上的樣子
struct DeviceView {
    // 外面的類型標記（ASIO、預設、獨占、靜音，其他輸出就用輸出的名字）；tone：ASIO 與獨占是琥珀色（獨佔裝置）、靜音是玫瑰紅
    std::wstring kind;
    Tone tone{Tone::normal};
    // 面板第一行後半段的模式說明
    std::wstring mode;
    // 裝置名：拿掉「輸出名 : 」前綴、[exclusive]、括號裡的廠牌
    std::wstring name;
    // 清單裡的排序（小的在上面）：預設 0、獨占 1、ASIO 2、其他輸出 3、靜音 4（使用者 2026-11-05 指定預設、獨占、ASIO 的順序）
    int rank{3};
};

// 拿掉第一個括號（半形或全形）起的部分；拿完是空的就保留原樣
std::wstring strip_parenthetical(std::wstring text)
{
    const auto position = text.find_first_of(L"(（");
    if (position != std::wstring::npos && position > 0) {
        auto cut = text.substr(0, position);
        trim(cut);
        if (!cut.empty())
            return cut;
    }
    return text;
}

// 括號裡 Windows 加的編號前綴「2- 」「5- 」拿掉：「DAC (2- Holo Audio USB Digital Interface Gen2)」→「DAC (Holo Audio USB Digital Interface Gen2)」
// （清單裡兩個裝置外面看起來一樣、要保留括號分辨時用）
std::wstring strip_index_prefix(std::wstring text)
{
    size_t position = 0;
    while ((position = text.find_first_of(L"(（", position)) != std::wstring::npos) {
        size_t end = position + 1;
        while (end < text.size() && text[end] >= L'0' && text[end] <= L'9')
            ++end;

        if (end > position + 1 && end + 1 < text.size() && text[end] == L'-' && text[end + 1] == L' ')
            text.erase(position + 1, end + 2 - (position + 1));

        ++position;
    }
    return text;
}

// 個人專屬的裝置顯示名稱（2026-11-05）：profile 資料夾裡的 output-device-names.txt，沒有這個檔就完全不生效
// （朋友版、公開版不放這個檔）。每行「裝置名稱裡要有的字=要顯示的名稱」，# 開頭是註解，UTF-8。
// 例：Holo Audio=DAC (Holo Audio) ——名稱（拿掉輸出名前綴與 [exclusive] 之後，括號也算）含「Holo Audio」的裝置，
// 外面、滑過面板、清單都顯示「DAC (Holo Audio)」，預設與獨占版一樣。比對不分大小寫。
std::vector<std::pair<std::wstring, std::wstring>> g_aliases;

void load_aliases()
{
    g_aliases.clear();

    try {
        const pfc::string8 native = ::filesystem::g_get_native_path(core_api::get_profile_path());
        std::wstring path = pfc::stringcvt::string_wide_from_utf8(native.c_str()).get_ptr();
        path += L"\\output-device-names.txt";

        FILE* file{};
        if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file)
            return;

        std::string content;
        char buffer[4096];
        size_t read_bytes{};
        while ((read_bytes = fread(buffer, 1, sizeof(buffer), file)) > 0 && content.size() < 65536)
            content.append(buffer, read_bytes);
        fclose(file);

        if (content.starts_with("\xEF\xBB\xBF"))
            content.erase(0, 3);

        size_t start = 0;
        while (start <= content.size()) {
            auto end = content.find('\n', start);
            if (end == std::string::npos)
                end = content.size();
            std::string line = content.substr(start, end - start);
            start = end + 1;

            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            const auto equals = line.find('=');
            if (line.empty() || line[0] == '#' || equals == std::string::npos)
                continue;

            auto key = widen(line.substr(0, equals).c_str());
            auto value = widen(line.substr(equals + 1).c_str());
            trim(key);
            trim(value);
            if (!key.empty() && !value.empty())
                g_aliases.emplace_back(std::move(key), std::move(value));
        }
    }
    catch (...) {
        g_aliases.clear();
    }
}

std::optional<std::wstring> find_alias(const std::wstring& device_name)
{
    for (const auto& [key, value] : g_aliases) {
        if (contains_no_case(device_name, key))
            return value;
    }
    return std::nullopt;
}

// full_name 是 fb2k 給的完整名稱，例如「ASIO+DSD : HoloAudio ASIO Driver」「Default : 螢幕喇叭 (…) [exclusive]」「Null Output」
// 預設、靜音用輸出 GUID 判斷；ASIO 看輸出名稱有沒有 ASIO；獨占看裝置名結尾有沒有 [exclusive]（字串比對）
DeviceView describe(const GUID& output, const std::wstring& full_name)
{
    DeviceView view;
    std::wstring output_name;
    std::wstring device_name = full_name;

    if (const auto separator = full_name.find(L" : "); separator != std::wstring::npos) {
        output_name = full_name.substr(0, separator);
        device_name = full_name.substr(separator + 3);
    }

    bool exclusive = false;
    if (ends_with(device_name, L"[exclusive]")) {
        exclusive = true;
        device_name.erase(device_name.size() - 11);
        trim(device_name);
    }

    if (output == output_id_null) {
        view.kind = nocturne_text::chip_kind_mute;
        view.tone = Tone::mute;
        view.mode = nocturne_text::chip_mode_mute;
        view.name = full_name;
        view.rank = 4;
        return view;
    }

    if (const auto alias = find_alias(device_name))
        view.name = *alias;
    else
        view.name = strip_parenthetical(device_name);

    if (contains_no_case(output_name, L"ASIO")) {
        view.kind = L"ASIO";
        view.tone = Tone::amber;
        view.mode = nocturne_text::chip_mode_asio; // 滑過面板第一行：跟「獨占模式」「靜音輸出」一樣寫成「ASIO 模式」
        view.rank = 2;
    } else if (exclusive) {
        view.kind = nocturne_text::chip_kind_exclusive;
        view.tone = Tone::amber;
        view.mode = nocturne_text::chip_mode_exclusive;
        view.rank = 1;
    } else if (output == output_id_default) {
        view.kind = nocturne_text::chip_kind_default;
        view.mode = nocturne_text::chip_mode_default;
        view.rank = 0;
    } else {
        view.kind = output_name.empty() ? std::wstring(nocturne_text::chip_unknown) : output_name;
        view.mode = view.kind;
    }

    return view;
}

// 目前輸出裝置（只在主執行緒讀寫）
struct State {
    bool valid{};
    DeviceView view;
};
State g_state;

bool api_available()
{
    return static_api_test_t<output_manager_v2>();
}

void refresh_state()
{
    load_aliases();

    State state;

    if (api_available()) {
        const auto api = output_manager_v2::get();
        outputCoreConfig_t config{};
        api->getCoreConfig(config);

        std::wstring found;
        bool matched = false;
        api->listDevices([&](const char* name, const GUID& output, const GUID& device) {
            if (!matched && output == config.m_output && device == config.m_device) {
                found = widen(name);
                matched = true;
            }
        });

        state.valid = true;
        state.view = describe(config.m_output, matched ? found : std::wstring());
        if (!matched) {
            state.view.name = nocturne_text::chip_unknown;
            state.view.mode = state.view.kind;
        }
    }

    g_state = std::move(state);
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

// 量文字寬度（像素）
float measure_text(const std::wstring& text, float px_size)
{
    if (text.empty())
        return 0;

    const auto family = create_font_family();
    const Gdiplus::Font font(family.get(), px_size, pick_font_style(*family), Gdiplus::UnitPixel);
    const wil::unique_hdc_window dc(GetDC(nullptr));
    Gdiplus::Graphics graphics(dc.get());
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    Gdiplus::RectF bounds;
    graphics.MeasureString(text.c_str(), -1, &font, Gdiplus::PointF(0, 0), Gdiplus::StringFormat::GenericTypographic(),
        &bounds);
    return bounds.Width;
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

// ---------------------------------------------------------------------------------------------------------------
// 滑過面板：浮在裝置顯示正上方的圓角面板（分層視窗，不吃滑鼠）
// ---------------------------------------------------------------------------------------------------------------
class Panel {
public:
    static void show(HWND chip, const RECT& chip_screen_rect, const DeviceView& view)
    {
        auto& self = instance();
        if (!self.ensure_window(chip))
            return;
        self.render_and_present(chip_screen_rect, view);
    }

    static void hide()
    {
        auto& self = instance();
        if (self.m_wnd && IsWindowVisible(self.m_wnd))
            ShowWindow(self.m_wnd, SW_HIDE);
    }

    static void destroy()
    {
        auto& self = instance();
        if (self.m_wnd)
            DestroyWindow(std::exchange(self.m_wnd, nullptr));
    }

private:
    static Panel& instance()
    {
        static Panel panel;
        return panel;
    }

    bool ensure_window(HWND chip)
    {
        const auto owner = GetAncestor(chip, GA_ROOT);

        if (m_wnd && m_owner != owner)
            destroy();

        if (m_wnd)
            return true;

        static const auto class_registered = [] {
            WNDCLASSEX window_class{};
            window_class.cbSize = sizeof(window_class);
            window_class.lpfnWndProc = DefWindowProc;
            window_class.hInstance = core_api::get_my_instance();
            window_class.lpszClassName = panel_class_name;
            return RegisterClassEx(&window_class) != 0;
        }();

        if (!class_registered)
            return false;

        m_wnd = CreateWindowEx(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
            panel_class_name, L"", WS_POPUP, 0, 0, 1, 1, owner, nullptr, core_api::get_my_instance(), nullptr);
        m_owner = owner;
        return m_wnd != nullptr;
    }

    void render_and_present(const RECT& chip_rect, const DeviceView& view)
    {
        const auto family = create_font_family();
        const auto style = pick_font_style(*family);
        const auto small_px = dip_f(panel_small_font_dip);
        const auto name_px = dip_f(panel_name_font_dip);
        const Gdiplus::Font small_font(family.get(), small_px, style, Gdiplus::UnitPixel);
        const Gdiplus::Font name_font(family.get(), name_px, style, Gdiplus::UnitPixel);

        // 第一行只寫模式（預設〔共用〕、獨占模式、ASIO、靜音輸出）：整個面板講的都是輸出裝置，不用再寫「輸出裝置」（使用者 2026-11-05）
        const std::wstring title = view.mode;
        const std::wstring hint = nocturne_text::chip_hint;

        const auto text_width = std::max({measure_text(title, small_px), measure_text(view.name, name_px),
            measure_text(hint, small_px)});
        const auto body_width = std::max(dip(panel_min_width), static_cast<int>(std::ceil(text_width)) + 2 * dip(panel_padding_x) + 2);
        const auto line_small = static_cast<int>(std::ceil(small_px * 1.25f));
        const auto line_name = static_cast<int>(std::ceil(name_px * 1.25f));
        const auto body_height = dip(panel_padding_top) + line_small + dip(3) + line_name + dip(4) + line_small + dip(panel_padding_bottom);

        const auto shadow = dip(panel_shadow);
        const auto width = body_width + 2 * shadow;
        const auto height = body_height + 2 * shadow;

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

            const Gdiplus::RectF body(static_cast<float>(shadow), static_cast<float>(shadow),
                static_cast<float>(body_width), static_cast<float>(body_height));
            const auto radius = static_cast<float>(dip(panel_radius));

            // 柔和陰影：往外擴的幾層很淡的圓角框
            for (int i = shadow; i >= 1; i -= std::max(1, shadow / 5)) {
                Gdiplus::GraphicsPath path;
                add_round_rect(path,
                    Gdiplus::RectF(body.X - i, body.Y - i + shadow / 3.f, body.Width + 2.f * i, body.Height + 2.f * i),
                    radius + i);
                const Gdiplus::SolidBrush brush(Gdiplus::Color(static_cast<BYTE>(10 + (shadow - i) * 2), 0, 0, 0));
                g.FillPath(&brush, &path);
            }

            Gdiplus::GraphicsPath body_path;
            add_round_rect(body_path, body, radius);
            const Gdiplus::SolidBrush fill(Gdiplus::Color(255, 38, 38, 38));
            g.FillPath(&fill, &body_path);
            const Gdiplus::Pen border(Gdiplus::Color(255, 72, 72, 72), 1.f);
            g.DrawPath(&border, &body_path);

            Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
            format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap | Gdiplus::StringFormatFlagsNoClip);
            const Gdiplus::SolidBrush bright(Gdiplus::Color(255, 241, 241, 241));
            const Gdiplus::SolidBrush hint_brush(Gdiplus::Color(255, 154, 154, 154));

            auto x = body.X + dip(panel_padding_x);
            auto y = body.Y + dip(panel_padding_top);
            // 第一行：模式（預設〔共用〕、獨占模式、ASIO、靜音輸出），依裝置類型上色（跟外面的標記一致：
            // ASIO／獨占琥珀色、靜音玫瑰紅、其他灰色）
            const Gdiplus::SolidBrush mode_brush(tone_text_colour(view.tone, Gdiplus::Color(255, 138, 138, 138)));
            g.DrawString(view.mode.c_str(), -1, &small_font, Gdiplus::PointF(x, y), &format, &mode_brush);
            y += line_small + dip(3);
            g.DrawString(view.name.c_str(), -1, &name_font, Gdiplus::PointF(x, y), &format, &bright);
            y += line_name + dip(4);
            g.DrawString(hint.c_str(), -1, &small_font, Gdiplus::PointF(x, y), &format, &hint_brush);
        }

        // 位置：面板的中線對齊裝置顯示的中線，面板底邊在裝置顯示上緣再往上 panel_gap_above_chip；不超出該螢幕的工作區
        const auto monitor = MonitorFromRect(&chip_rect, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitor_info{sizeof(monitor_info)};
        GetMonitorInfo(monitor, &monitor_info);
        // 面板的中線對齊裝置顯示
        auto x = (chip_rect.left + chip_rect.right) / 2 - body_width / 2 - shadow;
        auto y = chip_rect.top - dip(panel_gap_above_chip) - body_height - shadow;
        x = std::clamp<LONG>(x, monitor_info.rcWork.left - shadow, monitor_info.rcWork.right - width + shadow);
        y = std::max<LONG>(y, monitor_info.rcWork.top - shadow);

        POINT origin{x, y};
        SIZE size{width, height};
        POINT source{0, 0};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(m_wnd, screen_dc.get(), &origin, &size, memory_dc.get(), &source, 0, &blend, ULW_ALPHA);
        ShowWindow(m_wnd, SW_SHOWNOACTIVATE);
    }

    HWND m_wnd{};
    HWND m_owner{};
};

// ---------------------------------------------------------------------------------------------------------------
// 裝置清單：點裝置顯示後往上彈出的自畫選單（取代原生選單；樣式跟滑過面板同一套）
// - 面板中線對齊下方的裝置顯示，底邊在裝置顯示上緣再往上一小段；不超出該螢幕的工作區
// - 每列：打勾（目前裝置）、類型標記（ASIO／獨占琥珀色）、裝置名；類型換組時畫分隔線
// - 滑鼠：滑過反白、點一下選；點到清單外面或按 Esc、切到別的視窗就關掉。鍵盤：上下、Home／End、Enter
// ---------------------------------------------------------------------------------------------------------------
struct MenuItem {
    GUID output{};
    GUID device{};
    std::wstring kind;
    Tone tone{Tone::normal};
    std::wstring name;
    bool current{};
    int rank{};
};

// 把 draw 畫好的內容用 UpdateLayeredWindow 貼到 wnd（32 位元預乘 alpha 的 DIB）
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

// 面板底：柔和陰影＋圓角底＋細框（滑過面板與清單共用的樣子）
void draw_panel_body(Gdiplus::Graphics& g, const Gdiplus::RectF& body, int shadow)
{
    const auto radius = static_cast<float>(dip(panel_radius));

    for (int i = shadow; i >= 1; i -= std::max(1, shadow / 5)) {
        Gdiplus::GraphicsPath path;
        add_round_rect(path,
            Gdiplus::RectF(body.X - i, body.Y - i + shadow / 3.f, body.Width + 2.f * i, body.Height + 2.f * i),
            radius + i);
        const Gdiplus::SolidBrush brush(Gdiplus::Color(static_cast<BYTE>(10 + (shadow - i) * 2), 0, 0, 0));
        g.FillPath(&brush, &path);
    }

    Gdiplus::GraphicsPath body_path;
    add_round_rect(body_path, body, radius);
    const Gdiplus::SolidBrush fill(Gdiplus::Color(255, 38, 38, 38));
    g.FillPath(&fill, &body_path);
    const Gdiplus::Pen border(Gdiplus::Color(255, 72, 72, 72), 1.f);
    g.DrawPath(&border, &body_path);
}

class DeviceMenu {
public:
    static bool is_open()
    {
        auto& self = instance();
        return self.m_wnd && IsWindowVisible(self.m_wnd);
    }

    // chip_rect：裝置顯示的螢幕座標。on_choose(第幾項)、on_closed() 都在主執行緒呼叫
    static void show(HWND chip, const RECT& chip_rect, std::vector<MenuItem> items,
        std::function<void(size_t)> on_choose, std::function<void()> on_closed)
    {
        auto& self = instance();
        self.hide(false);
        if (items.empty() || !self.ensure_window(chip))
            return;

        self.m_items = std::move(items);
        self.m_on_choose = std::move(on_choose);
        self.m_on_closed = std::move(on_closed);
        self.m_hover = npos;
        self.m_pressed = npos;
        self.m_dismiss_pending = false;
        self.layout(chip_rect);
        self.render();

        ShowWindow(self.m_wnd, SW_SHOW);
        SetForegroundWindow(self.m_wnd);
        SetFocus(self.m_wnd);
        SetCapture(self.m_wnd);
    }

    static void close(bool notify) { instance().hide(notify); }

    static void destroy()
    {
        auto& self = instance();
        self.hide(false);
        if (self.m_wnd)
            DestroyWindow(std::exchange(self.m_wnd, nullptr));
    }

private:
    static constexpr size_t npos = static_cast<size_t>(-1);

    static DeviceMenu& instance()
    {
        static DeviceMenu menu;
        return menu;
    }

    static LRESULT CALLBACK s_wndproc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        return instance().on_message(wnd, msg, wp, lp);
    }

    bool ensure_window(HWND chip)
    {
        const auto owner = GetAncestor(chip, GA_ROOT);

        if (m_wnd && m_owner != owner)
            destroy();

        if (m_wnd)
            return true;

        static const auto class_registered = [] {
            WNDCLASSEX window_class{};
            window_class.cbSize = sizeof(window_class);
            window_class.lpfnWndProc = &DeviceMenu::s_wndproc;
            window_class.hInstance = core_api::get_my_instance();
            window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
            window_class.lpszClassName = menu_class_name;
            return RegisterClassEx(&window_class) != 0;
        }();

        if (!class_registered)
            return false;

        // 有擁有者的彈出視窗：用它的時候擁有者（主視窗）的標題列照樣顯示為使用中
        m_wnd = CreateWindowEx(WS_EX_LAYERED | WS_EX_TOOLWINDOW, menu_class_name, L"", WS_POPUP, 0, 0, 1, 1, owner,
            nullptr, core_api::get_my_instance(), nullptr);
        m_owner = owner;
        return m_wnd != nullptr;
    }

    // 排版：算每一列的位置與整個視窗的位置、大小
    void layout(const RECT& chip_rect)
    {
        const auto family = create_font_family();
        const auto style = pick_font_style(*family);
        const auto tag_px = dip_f(tag_font_dip);
        const auto name_px = dip_f(name_font_dip);

        float max_tag = 0;
        float max_name = 0;
        for (const auto& item : m_items) {
            max_tag = std::max(max_tag, measure_text(item.kind, tag_px));
            max_name = std::max(max_name, measure_text(item.name, name_px));
        }

        m_pad_x = dip(14);
        m_check_w = dip(22);
        m_tag_w = static_cast<int>(std::ceil(max_tag)) + 2 * dip(tag_padding_x) + 2;
        const auto gap_after_check = dip(4);
        const auto gap_after_tag = dip(12);
        const auto row_h = dip(32);
        const auto sep_h = dip(10);
        const auto pad_v = dip(6);

        m_body_w = std::max(dip(240), m_pad_x + m_check_w + gap_after_check + m_tag_w + gap_after_tag
                + static_cast<int>(std::ceil(max_name)) + 2 + dip(18));
        m_tag_x = m_pad_x + m_check_w + gap_after_check;
        m_name_x = m_tag_x + m_tag_w + gap_after_tag;

        m_rows.clear();
        m_separators.clear();
        int y = pad_v;

        // 最上面：標題「輸出模式」＋底下一條分隔線，再接各列
        m_title_top = y;
        m_title_h = dip(menu_title_height);
        y += m_title_h;
        m_separators.push_back(y + sep_h / 2);
        y += sep_h;

        for (size_t i = 0; i < m_items.size(); ++i) {
            if (i > 0 && m_items[i].kind != m_items[i - 1].kind) {
                m_separators.push_back(y + sep_h / 2);
                y += sep_h;
            }
            m_rows.push_back({dip(6), y, m_body_w - dip(6), y + row_h});
            y += row_h;
        }
        m_body_h = y + pad_v;

        m_shadow = dip(panel_shadow);
        m_width = m_body_w + 2 * m_shadow;
        m_height = m_body_h + 2 * m_shadow;

        // 中線對齊裝置顯示，底邊在裝置顯示上緣往上 panel_gap_above_chip
        const auto monitor = MonitorFromRect(&chip_rect, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitor_info{sizeof(monitor_info)};
        GetMonitorInfo(monitor, &monitor_info);
        const auto center_x = (chip_rect.left + chip_rect.right) / 2;
        auto x = center_x - m_body_w / 2 - m_shadow;
        auto y_pos = chip_rect.top - dip(panel_gap_above_chip) - m_body_h - m_shadow;
        x = std::clamp<LONG>(x, monitor_info.rcWork.left - m_shadow, monitor_info.rcWork.right - m_width + m_shadow);
        y_pos = std::max<LONG>(y_pos, monitor_info.rcWork.top - m_shadow);
        m_origin = {x, y_pos};
    }

    void render()
    {
        const auto family = create_font_family();
        const auto style = pick_font_style(*family);
        const Gdiplus::Font tag_font(family.get(), dip_f(tag_font_dip), style, Gdiplus::UnitPixel);
        const Gdiplus::Font name_font(family.get(), dip_f(name_font_dip), style, Gdiplus::UnitPixel);
        const Gdiplus::FontFamily icon_family(L"Segoe MDL2 Assets");
        const Gdiplus::Font icon_font(&icon_family, dip_f(12), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);

        present_layered(m_wnd, &m_origin, m_width, m_height, [&](Gdiplus::Graphics& g) {
            const auto shadow = static_cast<float>(m_shadow);
            draw_panel_body(g,
                Gdiplus::RectF(shadow, shadow, static_cast<float>(m_body_w), static_cast<float>(m_body_h)), m_shadow);

            const Gdiplus::Pen separator_pen(Gdiplus::Color(255, 64, 64, 64), 1.f);
            for (const auto y : m_separators)
                g.DrawLine(&separator_pen, shadow + m_pad_x, shadow + y, shadow + m_body_w - m_pad_x, shadow + y);

            Gdiplus::StringFormat center(Gdiplus::StringFormat::GenericTypographic());
            center.SetAlignment(Gdiplus::StringAlignmentCenter);
            center.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            center.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
            Gdiplus::StringFormat left(Gdiplus::StringFormat::GenericTypographic());
            left.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            left.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
            left.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

            // 標題：淡灰小字（跟滑過面板的提示字同色），靠左邊緣
            {
                const Gdiplus::SolidBrush title_brush(Gdiplus::Color(255, 154, 154, 154));
                const Gdiplus::RectF title_rect(shadow + m_pad_x, shadow + m_title_top,
                    static_cast<float>(m_body_w - 2 * m_pad_x), static_cast<float>(m_title_h));
                g.DrawString(nocturne_text::chip_menu_title, -1, &tag_font, title_rect, &left, &title_brush);
            }

            for (size_t i = 0; i < m_items.size(); ++i) {
                const auto& item = m_items[i];
                const auto& row = m_rows[i];
                const Gdiplus::RectF row_rect(shadow + row.left, shadow + row.top, static_cast<float>(row.right - row.left),
                    static_cast<float>(row.bottom - row.top));
                const auto hover = i == m_hover;

                if (hover) {
                    Gdiplus::GraphicsPath path;
                    add_round_rect(path, row_rect, static_cast<float>(dip(6)));
                    const Gdiplus::SolidBrush brush(Gdiplus::Color(28, 255, 255, 255));
                    g.FillPath(&brush, &path);
                }

                if (item.current) {
                    const Gdiplus::RectF check_rect(shadow + m_pad_x, row_rect.Y, static_cast<float>(m_check_w), row_rect.Height);
                    const Gdiplus::SolidBrush brush(Gdiplus::Color(255, 241, 241, 241));
                    g.DrawString(L"\xE73E", -1, &icon_font, check_rect, &center, &brush);
                }

                const auto tag_h = static_cast<float>(dip(tag_height));
                const Gdiplus::RectF tag_rect(shadow + m_tag_x, row_rect.Y + (row_rect.Height - tag_h) / 2.f,
                    static_cast<float>(m_tag_w), tag_h - 1.f);
                {
                    Gdiplus::GraphicsPath path;
                    add_round_rect(path, tag_rect, tag_h / 2.f);
                    const Gdiplus::Pen pen(tone_border_colour(item.tone), 1.f);
                    g.DrawPath(&pen, &path);
                }
                const Gdiplus::SolidBrush tag_brush(tone_text_colour(item.tone, Gdiplus::Color(199, 241, 241, 241)));
                g.DrawString(item.kind.c_str(), -1, &tag_font, tag_rect, &center, &tag_brush);

                const Gdiplus::RectF name_rect(shadow + m_name_x, row_rect.Y,
                    std::max(0.f, static_cast<float>(m_body_w - m_name_x - dip(14))), row_rect.Height);
                const Gdiplus::SolidBrush name_brush(
                    item.current || hover ? Gdiplus::Color(255, 241, 241, 241) : Gdiplus::Color(255, 196, 196, 196));
                g.DrawString(item.name.c_str(), -1, &name_font, name_rect, &left, &name_brush);
            }
        });
    }

    // 視窗座標（用了滑鼠捕捉，點到視窗外面時座標可能是負的）→ 第幾列；不在任何一列上傳回 npos
    size_t hit_row(int x, int y) const
    {
        const auto bx = x - m_shadow;
        const auto by = y - m_shadow;
        for (size_t i = 0; i < m_rows.size(); ++i) {
            const auto& row = m_rows[i];
            if (bx >= row.left && bx < row.right && by >= row.top && by < row.bottom)
                return i;
        }
        return npos;
    }

    bool inside_body(int x, int y) const
    {
        const auto bx = x - m_shadow;
        const auto by = y - m_shadow;
        return bx >= 0 && by >= 0 && bx < m_body_w && by < m_body_h;
    }

    void set_hover(size_t index)
    {
        if (index == m_hover)
            return;
        m_hover = index;
        render();
    }

    void hide(bool notify)
    {
        if (!m_wnd || !IsWindowVisible(m_wnd))
            return;

        if (GetCapture() == m_wnd)
            ReleaseCapture();
        ShowWindow(m_wnd, SW_HIDE);

        m_items.clear();
        m_on_choose = nullptr;
        const auto on_closed = std::exchange(m_on_closed, nullptr);
        if (notify && on_closed)
            on_closed();
    }

    void choose(size_t index)
    {
        if (index >= m_items.size())
            return;

        const auto on_choose = m_on_choose;
        hide(true);
        if (on_choose)
            on_choose(index);
    }

    LRESULT on_message(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_ACTIVATE;
        case WM_ACTIVATE:
            // 切到別的視窗（含按 Alt+Tab、點到別的程式）就關掉
            if (LOWORD(wp) == WA_INACTIVE) {
                hide(true);
                return 0;
            }
            break;
        case WM_MOUSEMOVE:
            set_hover(hit_row(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)));
            return 0;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN: {
            const auto x = GET_X_LPARAM(lp);
            const auto y = GET_Y_LPARAM(lp);
            if (!inside_body(x, y)) {
                // 點到外面：這一下點擊整個吞掉，放開時才關。一按下就放掉滑鼠捕捉的話，放開那一下會送給底下的播放列，
                // 變成展開／收合播放頁；點下方的裝置顯示本身也是同一個路徑，這樣它就只是把清單關起來、不會又打開
                m_dismiss_pending = true;
                return 0;
            }
            m_pressed = msg == WM_LBUTTONDOWN ? hit_row(x, y) : npos;
            return 0;
        }
        case WM_RBUTTONUP:
        case WM_MBUTTONUP:
            if (std::exchange(m_dismiss_pending, false))
                hide(true);
            return 0;
        case WM_LBUTTONUP: {
            if (std::exchange(m_dismiss_pending, false)) {
                hide(true);
                return 0;
            }
            const auto pressed = std::exchange(m_pressed, npos);
            if (pressed != npos && hit_row(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)) == pressed)
                choose(pressed);
            return 0;
        }
        case WM_KEYDOWN: {
            const auto count = m_items.size();
            if (wp == VK_ESCAPE) {
                hide(true);
            } else if (wp == VK_DOWN && count > 0) {
                set_hover(m_hover == npos || m_hover + 1 >= count ? 0 : m_hover + 1);
            } else if (wp == VK_UP && count > 0) {
                set_hover(m_hover == npos || m_hover == 0 ? count - 1 : m_hover - 1);
            } else if (wp == VK_HOME && count > 0) {
                set_hover(0);
            } else if (wp == VK_END && count > 0) {
                set_hover(count - 1);
            } else if ((wp == VK_RETURN || wp == VK_SPACE) && m_hover != npos) {
                choose(m_hover);
            }
            return 0;
        }
        case WM_SETCURSOR:
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
            return TRUE;
        }

        return DefWindowProc(wnd, msg, wp, lp);
    }

    HWND m_wnd{};
    HWND m_owner{};
    std::vector<MenuItem> m_items;
    std::function<void(size_t)> m_on_choose;
    std::function<void()> m_on_closed;
    size_t m_hover{npos};
    size_t m_pressed{npos};
    // 按在清單外面（要關掉清單）的那一下，等放開時才關
    bool m_dismiss_pending{};
    std::vector<RECT> m_rows;
    std::vector<int> m_separators;
    int m_title_top{};
    int m_title_h{};
    int m_pad_x{};
    int m_check_w{};
    int m_tag_w{};
    int m_tag_x{};
    int m_name_x{};
    int m_body_w{};
    int m_body_h{};
    int m_shadow{};
    int m_width{};
    int m_height{};
    POINT m_origin{};
};

// ---------------------------------------------------------------------------------------------------------------
// 裝置顯示本體：分割面板的子視窗
// ---------------------------------------------------------------------------------------------------------------
class Chip;
std::vector<Chip*>& live_chips()
{
    static std::vector<Chip*> chips;
    return chips;
}

cui::fb2k_utils::LeakyServicePtr<> g_callback_handle;

// 目前是否在播放（含暫停）：由播放回呼在事件當下更新。on_playback_stop 當下 playback_control::is_playing() 不一定已經是 false，
// 所以排版不直接問 fb2k，用這個旗標
bool g_playing{};
// 正在播的曲目有沒有一小時以上：時間欄文字是「h:mm:ss / h:mm:ss」還是「m:ss / m:ss」，寬度差一截，決定開始播放時往左推多少。
// 只在換歌（on_playback_new_track，這時曲長已知）時更新，同一種長度的歌之間換歌位置不動
bool g_long_track{};

// 裝置顯示的視窗是「分層子視窗」（每個像素自己的透明度，Windows 8 以後子視窗也能這樣）：
// 視窗固定罩住裝置顯示會滑動的整段範圍（開始播放時推到時間文字左邊、停止時縮回停止鈕旁），裝置顯示畫在視窗裡面、跟著動畫改位置，
// 其餘像素全透明，滑鼠會穿過去、底下的 ESLyric 畫面也不用重畫。
// 原本是每格都移動視窗本身：每移一格，底下露出來的那塊都要請 ESLyric 重畫，看起來又糊又不順（使用者 2026-11-05 回報 FPS 不夠）
class Chip {
public:
    static LRESULT CALLBACK s_wndproc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp);
    static Chip* from(HWND wnd) { return reinterpret_cast<Chip*>(GetWindowLongPtr(wnd, GWLP_USERDATA)); }

    void set_bar_rect(const RECT& rect) { m_bar_rect = rect; }
    // 裝置清單關掉了（選了、點到外面、Esc 都算）
    void on_menu_closed();

    // 開始播放／停止：位置的目標變了，用滑動動畫過去
    void refresh_layout()
    {
        const auto old_rel = m_current_rel;
        const auto had_position = m_rel_valid && IsWindowVisible(m_wnd);

        m_sliding = false;
        (void)compute_layout();

        if (had_position && old_rel != m_target_rel) {
            m_slide_back = m_target_rel > old_rel;
            m_slide_from = old_rel;
            m_slide_current = old_rel;
            m_slide_start = std::chrono::steady_clock::now();
            m_sliding = true;
            begin_high_resolution_timer();
            SetTimer(m_wnd, slide_timer_id, slide_interval_ms, nullptr);
        } else {
            stop_sliding_timer();
        }

        apply_layout();
    }

    // 滑動動畫的一格：往目標位置移，到了就停
    void slide_tick()
    {
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_slide_start).count();
        const auto t = std::clamp(elapsed / (m_slide_back ? slide_back_seconds : slide_out_seconds), 0.0, 1.0);
        // 推出去：快起慢停；縮回去：兩頭都慢的 smootherstep，比較柔和
        const auto eased = m_slide_back ? t * t * t * (t * (t * 6.0 - 15.0) + 10.0) : 1.0 - std::pow(1.0 - t, 3.0);
        m_slide_current = m_slide_from + static_cast<int>(std::lround((m_target_rel - m_slide_from) * eased));

        if (t >= 1.0) {
            m_sliding = false;
            stop_sliding_timer();
        }

        apply_layout();
    }

    // 給排版批次（defer_layout）用：視窗該放的位置，空矩形＝要藏起來
    RECT target_rect() { return compute_layout(); }
    void apply_layout();
    // 輸出裝置變了：重量文字、重排、重畫
    void on_state_changed()
    {
        m_measure_valid = false;
        apply_layout();
    }

    // 視窗被摧毀前把計時精度還回去
    void release_timer_resolution() { stop_sliding_timer(); }

private:
    LRESULT on_message(HWND wnd, UINT msg, WPARAM wp, LPARAM lp);
    void ensure_measure();
    void ensure_time_measure();
    // 算視窗該放的位置（分割面板座標）與裝置顯示在視窗裡的位置；放不下或沒有輸出資訊時傳回空矩形
    RECT compute_layout();
    void render();
    void draw_chip(Gdiplus::Graphics& g, int height) const;
    void show_menu();
    void show_panel();
    // 裝置顯示本身（不含透明的部分）在螢幕上的位置，滑過面板與清單用它對齊
    RECT chip_screen_rect() const
    {
        RECT rect{};
        GetWindowRect(m_wnd, &rect);
        rect.left += m_chip_x;
        rect.right = rect.left + m_chip_width;
        return rect;
    }

    void begin_high_resolution_timer()
    {
        if (!m_high_resolution_timer) {
            timeBeginPeriod(1);
            m_high_resolution_timer = true;
        }
    }

    void stop_sliding_timer()
    {
        if (m_wnd)
            KillTimer(m_wnd, slide_timer_id);
        if (m_high_resolution_timer) {
            timeEndPeriod(1);
            m_high_resolution_timer = false;
        }
    }

    HWND m_wnd{};
    RECT m_bar_rect{};
    bool m_hot{};
    // 是否已要求 WM_MOUSELEAVE（要求一次只送一次；清單開著時送來的那次被略過，關掉後要重新要求，否則反白會卡住不退）
    bool m_tracking{};
    bool m_pressed{};
    bool m_menu_open{};
    // 時間文字兩種版本量好的寬度（像素），縮放變了才重量：不到一小時「59:59 / 59:59」、一小時以上「11:52:32 / 11:52:32」
    int m_time_dpi{};
    float m_time_text_width_short{};
    float m_time_text_width_long{};
    // 水平位置（裝置顯示右緣離播放列右緣的距離，像素）：目標、目前顯示的，以及滑動動畫
    bool m_rel_valid{};
    int m_target_rel{};
    int m_current_rel{};
    bool m_sliding{};
    // 這一段是往右縮回去（停止）還是往左推出來（開始播放）
    bool m_slide_back{};
    int m_slide_from{};
    int m_slide_current{};
    std::chrono::steady_clock::time_point m_slide_start;
    bool m_high_resolution_timer{};
    // 裝置顯示在視窗裡的位置（左緣、寬度）
    int m_chip_x{};
    int m_chip_width{};
    // 量好的文字寬度（像素），換了裝置或縮放就重量
    bool m_measure_valid{};
    int m_measure_dpi{};
    float m_tag_text_width{};
    float m_name_text_width{};
    // 這一次排版有沒有放得下名字
    bool m_show_name{true};
};

void Chip::ensure_measure()
{
    const auto dpi = uih::scale_dpi_value(1000);
    if (m_measure_valid && m_measure_dpi == dpi)
        return;

    m_tag_text_width = measure_text(g_state.view.kind, dip_f(tag_font_dip));
    m_name_text_width = measure_text(g_state.view.name, dip_f(name_font_dip));
    m_measure_dpi = dpi;
    m_measure_valid = true;
}

// 播放中（含暫停）時間欄文字的寬度基準分兩種（使用者 2026-11-05 指示）：不到一小時用「59:59 / 59:59」，一小時以上用「11:52:32 / 11:52:32」
// （小時是兩位數，使用者最長的檔案 11:52:32）。不是每首歌各自依曲長算——只分「有沒有小時」兩版，同一版內換歌位置不動；
// 也只在換歌、曲長已知時才排版，開始播放不會先動一次、曲長出來又動一次
void Chip::ensure_time_measure()
{
    const auto dpi = uih::scale_dpi_value(1000);
    if (m_time_text_width_short > 0 && dpi == m_time_dpi)
        return;

    m_time_dpi = dpi;
    m_time_text_width_short = measure_text(L"59:59 / 59:59", dip_f(time_font_dip));
    m_time_text_width_long = measure_text(L"11:52:32 / 11:52:32", dip_f(time_font_dip));
}

RECT Chip::compute_layout()
{
    if (!g_state.valid || m_bar_rect.right <= m_bar_rect.left)
        return {};

    ensure_measure();
    ensure_time_measure();

    const auto bar_width = m_bar_rect.right - m_bar_rect.left;
    // 兩個端點：沒在播放（時間欄是空的）時縮到停止鈕旁邊，播放中推到最長時間文字的左邊
    const auto time_right = m_bar_rect.right - dip(time_col_right_from_bar_right) - dip(time_right_margin);
    const auto right_stopped = time_right - dip(time_gap);
    const auto time_width = g_long_track ? m_time_text_width_long : m_time_text_width_short;
    const auto right_playing = time_right - static_cast<int>(std::ceil(time_width)) - dip(time_gap);
    // 小時版（最左邊）的位置：視窗與名字放不放得下都以它為準，兩種曲長版本之間來回滑時裝置顯示才不會被視窗邊緣切掉
    // （原本以目前版本為準，小時版換成分鐘版時視窗左緣瞬間右移，還在舊位置的裝置顯示被切掉一截，要等滑進新視窗才看得到）
    const auto right_playing_long = time_right - static_cast<int>(std::ceil(m_time_text_width_long)) - dip(time_gap);

    m_target_rel = (g_playing ? right_playing : right_stopped) - m_bar_rect.right;
    m_current_rel = m_sliding ? m_slide_current : m_target_rel;
    m_rel_valid = true;

    // 名字放不放得下、放多寬，以「小時版」的位置（最左的那端）為準，各種狀態裝置顯示一樣寬
    const auto left_bound = m_bar_rect.left + bar_width / 2 + dip(center_clear);
    const auto available = right_playing_long - left_bound;

    const auto tag_width = static_cast<int>(std::ceil(m_tag_text_width)) + 2 * dip(tag_padding_x) + 2;
    const auto tag_only_width = 2 * dip(chip_padding) + tag_width;

    int width = tag_only_width;
    m_show_name = false;

    if (available >= tag_only_width + tag_gap_px() + dip(min_name_width)) {
        const auto natural = tag_only_width + tag_gap_px() + static_cast<int>(std::ceil(m_name_text_width)) + 2;
        width = std::min<int>(available, natural);
        m_show_name = true;
    } else if (available < tag_only_width) {
        return {};
    }

    // 視窗罩住整段範圍：左緣＝小時版時裝置顯示的左緣，右緣＝停止時裝置顯示的右緣
    const auto window_left = right_playing_long - width;
    const auto height = dip(chip_height);
    const auto top = m_bar_rect.top + (m_bar_rect.bottom - m_bar_rect.top - height) / 2 + dip(vertical_offset);

    m_chip_width = width;
    m_chip_x = (m_bar_rect.right + m_current_rel) - width - window_left;
    return {window_left, top, right_stopped, top + height};
}

void Chip::apply_layout()
{
    const auto target = compute_layout();

    if (IsRectEmpty(&target)) {
        if (IsWindowVisible(m_wnd)) {
            Panel::hide();
            ShowWindow(m_wnd, SW_HIDE);
        }
        return;
    }

    RECT current{};
    GetWindowRect(m_wnd, &current);
    MapWindowPoints(HWND_DESKTOP, GetParent(m_wnd), reinterpret_cast<POINT*>(&current), 2);
    const auto on_top = GetWindow(GetParent(m_wnd), GW_CHILD) == m_wnd;

    if (!(IsWindowVisible(m_wnd) && on_top && EqualRect(&current, &target))) {
        SetWindowPos(m_wnd, HWND_TOP, target.left, target.top, target.right - target.left, target.bottom - target.top,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    // 分層視窗的內容要自己貼上去：位置、大小、裝置顯示在視窗裡的位置、反白狀態變了都靠這裡重畫
    render();
}

void Chip::render()
{
    if (!m_wnd || !IsWindowVisible(m_wnd))
        return;

    RECT client{};
    GetClientRect(m_wnd, &client);
    if (client.right <= 0 || client.bottom <= 0)
        return;

    present_layered(m_wnd, nullptr, client.right, client.bottom,
        [&](Gdiplus::Graphics& g) { draw_chip(g, client.bottom); });
}

void Chip::draw_chip(Gdiplus::Graphics& g, int height) const
{
    const auto width = m_chip_width;
    g.TranslateTransform(static_cast<float>(m_chip_x), 0.f);

    // 沒有反白時背景完全透明（跟播放列的底色融在一起）；但分層視窗透明度是 0 的像素滑鼠會穿過去，
    // 所以裝置顯示範圍鋪一層幾乎看不見的底（透明度 1/255），滑鼠才碰得到
    {
        Gdiplus::GraphicsPath path;
        add_round_rect(path, Gdiplus::RectF(0.f, 0.f, static_cast<float>(width), static_cast<float>(height)),
            static_cast<float>(dip(6)));
        const Gdiplus::SolidBrush hit_brush(Gdiplus::Color(1, 24, 24, 24));
        g.FillPath(&hit_brush, &path);

        const auto lit = m_hot || m_menu_open;
        if (lit) {
            const Gdiplus::SolidBrush brush(Gdiplus::Color(m_pressed || m_menu_open ? 26 : 12, 255, 255, 255));
            g.FillPath(&brush, &path);
        }
    }

    const auto lit = m_hot || m_menu_open;
    const auto& view = g_state.view;
    const auto family = create_font_family();
    const auto style = pick_font_style(*family);
    const Gdiplus::Font tag_font(family.get(), dip_f(tag_font_dip), style, Gdiplus::UnitPixel);
    const Gdiplus::Font name_font(family.get(), dip_f(name_font_dip), style, Gdiplus::UnitPixel);

    // 類型標記：圓角膠囊外框＋字，ASIO／獨占是琥珀色
    const auto tag_width = static_cast<float>(static_cast<int>(std::ceil(m_tag_text_width)) + 2 * dip(tag_padding_x) + 2);
    const auto tag_h = static_cast<float>(dip(tag_height));
    const Gdiplus::RectF tag_rect(static_cast<float>(dip(chip_padding)), (height - tag_h) / 2.f, tag_width, tag_h - 1.f);
    {
        Gdiplus::GraphicsPath path;
        add_round_rect(path, tag_rect, tag_h / 2.f);
        const Gdiplus::Pen pen(tone_border_colour(view.tone), 1.f);
        g.DrawPath(&pen, &path);
    }

    Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
    format.SetAlignment(Gdiplus::StringAlignmentCenter);
    format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    const Gdiplus::SolidBrush tag_brush(tone_text_colour(view.tone, Gdiplus::Color(199, 241, 241, 241)));
    g.DrawString(view.kind.c_str(), -1, &tag_font, tag_rect, &format, &tag_brush);

    if (m_show_name) {
        Gdiplus::StringFormat name_format(Gdiplus::StringFormat::GenericTypographic());
        name_format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        name_format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
        name_format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
        const auto left = tag_rect.GetRight() + static_cast<float>(tag_gap_px());
        const Gdiplus::RectF name_rect(left, 0.f, std::max(0.f, width - dip(chip_padding) - left), static_cast<float>(height));
        const Gdiplus::SolidBrush name_brush(lit ? Gdiplus::Color(255, 241, 241, 241) : Gdiplus::Color(255, 150, 150, 150));
        g.DrawString(view.name.c_str(), -1, &name_font, name_rect, &name_format, &name_brush);
    }
}

void Chip::show_panel()
{
    if (!g_state.valid || !m_hot || m_menu_open)
        return;

    Panel::show(m_wnd, chip_screen_rect(), g_state.view);
}

void Chip::show_menu()
{
    if (!api_available())
        return;

    Panel::hide();

    const auto api = output_manager_v2::get();
    outputCoreConfig_t config{};
    api->getCoreConfig(config);

    std::vector<MenuItem> items;
    // 拿掉輸出名前綴與 [exclusive]、但保留括號的完整裝置名：兩個裝置外面看起來一樣時，清單改用它才分得出來
    std::vector<std::wstring> long_names;

    api->listDevices([&](const char* name, const GUID& output, const GUID& device) {
        const auto full = widen(name);
        const auto view = describe(output, full);

        auto long_name = full;
        if (const auto separator = long_name.find(L" : "); separator != std::wstring::npos)
            long_name = long_name.substr(separator + 3);
        if (ends_with(long_name, L"[exclusive]")) {
            long_name.erase(long_name.size() - 11);
            trim(long_name);
        }

        items.push_back({output, device, view.kind, view.tone, view.name,
            output == config.m_output && device == config.m_device, view.rank});
        long_names.push_back(strip_index_prefix(std::move(long_name)));
    });

    if (items.empty())
        return;

    // 照類型排序：預設、獨占、ASIO、其他、靜音；同一類裡維持 fb2k 原本的順序
    {
        std::vector<size_t> order(items.size());
        std::iota(order.begin(), order.end(), size_t{0});
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return items[a].rank < items[b].rank; });

        std::vector<MenuItem> sorted_items;
        std::vector<std::wstring> sorted_long_names;
        for (const auto index : order) {
            sorted_items.push_back(std::move(items[index]));
            sorted_long_names.push_back(std::move(long_names[index]));
        }
        items = std::move(sorted_items);
        long_names = std::move(sorted_long_names);
    }

    for (size_t i = 0; i < items.size(); ++i) {
        const auto duplicated = std::count_if(items.begin(), items.end(),
            [&](const MenuItem& other) { return other.kind == items[i].kind && other.name == items[i].name; });
        if (duplicated > 1)
            items[i].name = long_names[i];
    }

    std::vector<std::pair<GUID, GUID>> devices;
    for (const auto& item : items)
        devices.emplace_back(item.output, item.device);

    m_menu_open = true;
    render();

    const auto wnd = m_wnd;
    DeviceMenu::show(
        m_wnd, chip_screen_rect(), std::move(items),
        [devices = std::move(devices)](size_t index) {
            if (index < devices.size() && api_available())
                output_manager_v2::get()->setCoreConfigDevice(devices[index].first, devices[index].second);
        },
        [wnd] {
            if (IsWindow(wnd)) {
                if (const auto chip = Chip::from(wnd))
                    chip->on_menu_closed();
            }
        });

    // 沒能顯示出來（建不了視窗）就當作沒開
    if (!DeviceMenu::is_open())
        on_menu_closed();
}

void Chip::on_menu_closed()
{
    m_menu_open = false;
    // 選單開著時滑鼠可能已經離開，關掉後以游標實際位置為準
    POINT cursor{};
    GetCursorPos(&cursor);
    m_hot = WindowFromPoint(cursor) == m_wnd;
    render();
}

LRESULT Chip::on_message(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_NCCREATE:
        m_wnd = wnd;
        SetWindowLongPtr(wnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        live_chips().push_back(this);
        break;
    case WM_NCDESTROY: {
        auto& chips = live_chips();
        if (m_menu_open)
            DeviceMenu::close(false);
        release_timer_resolution();
        chips.erase(std::remove(chips.begin(), chips.end(), this), chips.end());
        if (chips.empty()) {
            Panel::destroy();
            DeviceMenu::destroy();
            g_callback_handle.reset();
        }
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
            Panel::hide();
        break;
    case WM_MOUSEMOVE:
        if (!m_tracking) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, wnd, 0};
            if (TrackMouseEvent(&tme))
                m_tracking = true;
        }
        if (!m_hot) {
            m_hot = true;
            SetTimer(wnd, hover_timer_id, hover_delay_ms, nullptr);
            render();
        }
        break;
    case WM_MOUSELEAVE:
        m_tracking = false;
        // 選單開著時游標移到選單上也會送 WM_MOUSELEAVE，不改狀態（選單關掉後由 show_menu 重新判斷）
        if (!m_menu_open) {
            m_hot = false;
            m_pressed = false;
            KillTimer(wnd, hover_timer_id);
            Panel::hide();
            render();
        }
        return 0;
    case WM_TIMER:
        if (wp == slide_timer_id) {
            slide_tick();
            return 0;
        }
        if (wp == hover_timer_id) {
            KillTimer(wnd, hover_timer_id);
            show_panel();
            return 0;
        }
        break;
    case WM_LBUTTONDOWN:
        KillTimer(wnd, hover_timer_id);
        Panel::hide();
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
        const RECT chip_rect{m_chip_x, 0, m_chip_x + m_chip_width, client.bottom};
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (was_pressed && PtInRect(&chip_rect, pt))
            show_menu();
        else
            render();
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

LRESULT CALLBACK Chip::s_wndproc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Chip* chip = from(wnd);

    if (msg == WM_NCCREATE) {
        chip = new Chip();
        return chip->on_message(wnd, msg, wp, lp);
    }

    if (!chip)
        return DefWindowProc(wnd, msg, wp, lp);

    return chip->on_message(wnd, msg, wp, lp);
}

void refresh_all()
{
    refresh_state();
    for (const auto chip : live_chips())
        chip->on_state_changed();
}

// 換歌、開始、停止時曲長變了，時間文字的寬度跟著變，裝置顯示要重排
class ChipPlayCallback : public play_callback_static {
public:
    unsigned get_flags() override
    {
        return flag_on_playback_new_track | flag_on_playback_stop;
    }

    void on_playback_new_track(metadb_handle_ptr track) override
    {
        g_playing = true;
        g_long_track = track.is_valid() && track->get_length() >= 3600.0;
        relayout();
    }
    // 開始播放時曲長還不知道，等 on_playback_new_track 才排版，才不會先動一次、曲長出來又動一次
    void on_playback_starting(play_control::t_track_command command, bool paused) override {}
    void on_playback_stop(play_control::t_stop_reason reason) override
    {
        g_playing = false;
        relayout();
    }
    void on_playback_seek(double time) override {}
    void on_playback_pause(bool state) override {}
    void on_playback_edited(metadb_handle_ptr track) override {}
    void on_playback_dynamic_info(const file_info& info) override {}
    void on_playback_dynamic_info_track(const file_info& info) override {}
    void on_playback_time(double time) override {}
    void on_volume_change(float new_value) override {}

private:
    static void relayout()
    {
        for (const auto chip : live_chips())
            chip->refresh_layout();
    }
};

play_callback_static_factory_t<ChipPlayCallback> chip_play_callback;

} // namespace

HWND create(HWND parent)
{
    static const auto class_registered = [] {
        WNDCLASSEX window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.style = CS_HREDRAW | CS_VREDRAW;
        window_class.lpfnWndProc = Chip::s_wndproc;
        window_class.hInstance = core_api::get_my_instance();
        window_class.hCursor = LoadCursor(nullptr, IDC_HAND);
        window_class.lpszClassName = chip_class_name;
        return RegisterClassEx(&window_class) != 0;
    }();

    if (!class_registered)
        return nullptr;

    refresh_state();
    g_playing = playback_control::get()->is_playing();
    g_long_track = g_playing && playback_control::get()->playback_get_length() >= 3600.0;

    const auto wnd = CreateWindowEx(WS_EX_LAYERED, chip_class_name, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, parent,
        nullptr, core_api::get_my_instance(), nullptr);

    // 輸出設定一變就更新（回呼可能在別的執行緒，轉回主執行緒）；最後一個視窗關掉時放掉
    if (wnd && g_callback_handle.get().is_empty() && api_available()) {
        g_callback_handle = output_manager_v2::get()->addCallback([] { fb2k::inMainThread([] { refresh_all(); }); });
    }

    return wnd;
}

void layout(HWND wnd, const RECT& bar_rect)
{
    if (const auto chip = Chip::from(wnd)) {
        chip->set_bar_rect(bar_rect);
        chip->apply_layout();
    }
}

HDWP defer_layout(HDWP dwp, HWND wnd, const RECT& bar_rect)
{
    const auto chip = Chip::from(wnd);
    if (!chip)
        return dwp;

    chip->set_bar_rect(bar_rect);
    const auto target = chip->target_rect();

    if (IsRectEmpty(&target)) {
        if (IsWindowVisible(wnd))
            return DeferWindowPos(dwp, wnd, nullptr, 0, 0, 0, 0,
                SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return dwp;
    }

    return DeferWindowPos(dwp, wnd, HWND_TOP, target.left, target.top, target.right - target.left,
        target.bottom - target.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

} // namespace cui::output_chip
