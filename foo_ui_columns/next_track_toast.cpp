#include "pch.h"

#include <shlwapi.h>
#include <timeapi.h>
#include <thread>

#include "next_track_toast.h"
#include "font_utils.h"
#include "menu_items.h"
#include "nocturne_text.h"
#include "splitter.h"

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "winmm.lib")

namespace cui::next_track_toast {

fbh::ConfigBool enabled(GUID{0x6f1d7a40, 0x2c19, 0x4b8e, {0x9a, 0x35, 0x0d, 0xc4, 0x71, 0xe8, 0x52, 0x1b}}, true);
fbh::ConfigUint32 seconds(
    GUID{0x3a92e6b1, 0x8d47, 0x4f60, {0xb1, 0x2e, 0x5c, 0x07, 0x9f, 0xa3, 0x68, 0xd4}}, default_seconds);
fbh::ConfigBool show_seconds_in_ring(
    GUID{0xc84b0f27, 0x5e13, 0x49aa, {0x8f, 0x61, 0xd2, 0x3b, 0x90, 0x17, 0xea, 0x45}}, true);
fbh::ConfigBool show_cover(GUID{0x15e7d9c3, 0xa046, 0x4d12, {0xbb, 0x78, 0x31, 0xf4, 0x6a, 0x8c, 0x02, 0xe9}}, true);
fbh::ConfigBool show_source_tag(
    GUID{0x9d03b6e8, 0x7f2a, 0x4c55, {0xa4, 0x19, 0x68, 0xe0, 0x2d, 0xb7, 0x53, 0xfc}}, true);
fbh::ConfigUint32 position(
    GUID{0x5d2f8c17, 0x4a63, 0x4e9b, {0xb0, 0xd1, 0x86, 0xc3, 0xe7, 0xa2, 0xf5, 0x94}}, position_bottom_right);

namespace {

using panels::splitter::FlatSplitterPanel;
using Clock = std::chrono::steady_clock;

constexpr wchar_t toast_class_name[] = L"columns_ui_next_track_toast";
constexpr wchar_t timer_class_name[] = L"columns_ui_next_track_toast_timer";
constexpr UINT_PTR timer_id = 1;
constexpr UINT interval_active_ms = 16;
constexpr UINT interval_idle_ms = 150;
// 淡入拉長到 0.8 秒、淡出 0.5 秒，原本 0.3 秒太突然（2026-11-02）
constexpr double fade_in_seconds = 0.8;
constexpr double fade_out_seconds = 0.5;
// 單次動畫最多推進這麼多秒：主執行緒卡住（例如換曲）時，淡入淡出順著時間拖長，不會一次跳一大截（原本 0.1 秒）
constexpr double max_step_seconds = 0.04;
// 換曲後這段時間不顯示提示，避免換曲瞬間播放位置還沒更新而誤判
constexpr auto suppress_after_new_track = std::chrono::milliseconds(1500);
// 倒數圈用的播放位置要保持連續：暫停→繼續時 fb2k 回報的位置會往前跳約 0.2～0.3 秒（ASIO+DSD，錄影量到進度條紅線跳 2 px、
// 圈內數字早翻一格），狂按暫停就一直小卡。單次不超過這個秒數的跳動當成回報誤差吸收掉，更大的（跳轉、換曲）照實顯示
constexpr double max_absorb_seconds = 0.6;
// 吸收掉的偏差用這個時間常數慢慢歸零，免得每次暫停繼續都累積落後
constexpr double offset_decay_seconds = 0.7;
// 提示淡入時從上方多少 DIP 滑下來
constexpr double slide_dip = 6;
constexpr auto next_refresh_interval = std::chrono::seconds(1);

std::atomic<bool> g_alive{false};

// 版面（DIP，乘上主視窗的縮放比例才是實際像素）
// 2026-11-02 219（125%）版：實際約 620×120，預設放右下；離左右邊 60（右邊對齊播放列「0.00 dB」文字的右緣）、離上緣 115、離下緣 50；169 之後再調
struct Layout {
    int width, height, margin_side, margin_top, margin_bottom;
    int pad_left, pad_right, gap, radius;
    int ring, cover, cover_radius, tag_pad_x, tag_pad_y;
    float stroke, font_caption, font_tag, font_title, font_sub, font_number, line_gap;

    explicit Layout(double scale)
    {
        const auto px = [scale](double dip) { return static_cast<int>(std::lround(dip * scale)); };
        const auto fpx = [scale](double dip) { return static_cast<float>(dip * scale); };
        width = px(496);
        height = px(96);
        margin_side = px(48);
        margin_top = px(92);
        margin_bottom = px(40);
        pad_left = px(21);
        pad_right = px(24);
        gap = px(16);
        radius = px(16);
        ring = px(46);
        cover = px(67);
        cover_radius = px(8);
        tag_pad_x = px(7);
        tag_pad_y = px(3);
        stroke = std::max(2.f, fpx(2.6));
        font_caption = fpx(15);
        font_tag = fpx(13);
        font_title = fpx(24);
        font_sub = fpx(16);
        font_number = fpx(19);
        line_gap = fpx(2);
    }
};

// 每行字的行框高度（字級的倍數）
constexpr float line_box_ratio = 1.25f;
// 字的視覺中心在基線上方多少（字級的倍數），用來把字放在行框正中央
constexpr float ink_center_above_baseline = 0.38f;

struct NextTrack {
    metadb_handle_ptr handle;
    bool from_queue{};
};

enum class OrderKind { sequential, repeat_playlist, repeat_track, random };

OrderKind get_order_kind(const service_ptr_t<playlist_manager>& playlists)
{
    const auto index = playlists->playback_order_get_active();
    const std::string_view name = playlists->playback_order_get_name(index);

    if (name.find("Repeat (track)") != std::string_view::npos)
        return OrderKind::repeat_track;
    if (name.find("Repeat (playlist)") != std::string_view::npos)
        return OrderKind::repeat_playlist;
    if (name.find("Random") != std::string_view::npos || name.find("Shuffle") != std::string_view::npos)
        return OrderKind::random;
    if (name == "Default")
        return OrderKind::sequential;

    // 名稱不認得（外掛加的順序，或名稱不是英文）：照核心內建順序的編號
    switch (index) {
    case 0:
        return OrderKind::sequential;
    case 1:
        return OrderKind::repeat_playlist;
    case 2:
        return OrderKind::repeat_track;
    default:
        return OrderKind::random;
    }
}

// 決定下一首（行為邊界 B，見 docs 預覽頁）；沒有下一首或猜不準（隨機、單曲循環）傳回空
// now_playing 為空表示沒在播放，只認佇列
std::optional<NextTrack> resolve_next(const metadb_handle_ptr& now_playing)
{
    const auto playlists = playlist_manager::get();

    if (playlists->queue_get_count() > 0) {
        pfc::list_t<t_playback_queue_item> queue;
        playlists->queue_get_contents(queue);
        if (queue.get_count() > 0 && queue[0].m_handle.is_valid())
            return NextTrack{queue[0].m_handle, true};
    }

    if (!now_playing.is_valid())
        return {};

    const auto order = get_order_kind(playlists);
    if (order == OrderKind::random || order == OrderKind::repeat_track)
        return {};

    size_t playlist = pfc_infinite;
    size_t index = pfc_infinite;

    if (!playlists->get_playing_item_location(&playlist, &index)) {
        // 核心記的「正在播的那一項」已經不在清單裡（例如 Album list 換過資料夾）：在播放中那份清單用檔案找同一首
        playlist = playlists->get_playing_playlist();
        if (playlist == pfc_infinite)
            return {};

        pfc::list_t<metadb_handle_ptr> items;
        playlists->playlist_get_all_items(playlist, items);
        index = items.find_item(now_playing);
        if (index == pfc_infinite)
            return {};
    }

    // 播放跟隨游標：游標（反白）停在別的項目時，下一首就是它（這個行為我是照印象寫的，要實測確認）
    if (config_object::g_get_data_bool_simple(standard_config_objects::bool_playback_follows_cursor, false)) {
        const auto focus = playlists->playlist_get_focus_item(playlist);
        if (focus != pfc_infinite && focus != index) {
            metadb_handle_ptr handle;
            if (playlists->playlist_get_item_handle(handle, playlist, focus))
                return NextTrack{std::move(handle), false};
        }
    }

    const auto count = playlists->playlist_get_item_count(playlist);
    size_t next = index + 1;

    if (next >= count) {
        if (order != OrderKind::repeat_playlist || count == 0)
            return {};
        next = 0;
    }

    metadb_handle_ptr handle;
    if (!playlists->playlist_get_item_handle(handle, playlist, next))
        return {};

    return NextTrack{std::move(handle), false};
}

std::wstring format_track_text(const metadb_handle_ptr& handle, const char* script)
{
    service_ptr_t<titleformat_object> object;
    titleformat_compiler::get()->compile_safe(object, script);

    pfc::string8 text;
    handle->format_title(nullptr, text, object, nullptr);
    return pfc::stringcvt::string_wide_from_utf8(text.c_str()).get_ptr();
}

// 提示畫面上要顯示的文字與開關（內容變了才重畫靜態的那一層）
struct Content {
    bool stopped{};
    bool show_number{};
    bool show_cover{};
    bool show_tag{};
    std::wstring caption;
    std::wstring tag;
    std::wstring title;
    std::wstring artist;
};

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

// 暖機 GDI+ 的字型表：進程裡第一次依名稱建立字型家族時，GDI+ 要列舉整台電腦的字型、建內部字型表，
// 實測 219 要 1.4 秒、169 要 3 秒左右（跟裝了多少字型有關），之後都有快取。不先做的話，第一次顯示提示就在
// 主執行緒上做這一步，整個介面（進度條、播放時間）停住，提示也晚出現（待辦第 10 項）。
// 啟動時放到背景執行緒做。這裡自己握一份 GDI+ 的啟動計數直到結束：主視窗那份還沒啟動時，暖機用的那份一關，
// 剛建好的字型表會跟著被丟掉
namespace font_warm_up {

ULONG_PTR token{};
bool started{};
std::thread worker;

void start()
{
    // 字型名稱在主執行緒讀，跟畫提示時用的是同一個字型（Common (labels)）
    std::wstring face_name;
    try {
        face_name = fonts::get_log_font_with_fallback(fonts::labels_font_id).lfFaceName;
    } catch (...) {
        return;
    }

    const Gdiplus::GdiplusStartupInput input;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok)
        return;
    started = true;

    worker = std::thread([face_name = std::move(face_name)] {
        const Gdiplus::FontFamily family(face_name.c_str());
        if (family.GetLastStatus() == Gdiplus::Ok) {
            // 順便建一個字型，把字型的度量資料也載進來
            const Gdiplus::Font font(&family, 16.f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            (void)font;
        }
    });
}

void stop()
{
    if (worker.joinable())
        worker.join();

    if (started) {
        Gdiplus::GdiplusShutdown(token);
        started = false;
    }
}

} // namespace font_warm_up

Gdiplus::FontStyle pick_font_style(const Gdiplus::FontFamily& family)
{
    if (family.IsStyleAvailable(Gdiplus::FontStyleRegular))
        return Gdiplus::FontStyleRegular;
    if (family.IsStyleAvailable(Gdiplus::FontStyleBold))
        return Gdiplus::FontStyleBold;
    return Gdiplus::FontStyleItalic;
}

// 靜態那一層：卡片底、封面、文字（換歌或換設定才重畫）
void draw_static_layer(Gdiplus::Bitmap& bitmap, const Layout& layout, const Content& content, Gdiplus::Bitmap* cover)
{
    Gdiplus::Graphics g(&bitmap);
    g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    g.Clear(Gdiplus::Color(0, 0, 0, 0));
    g.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

    const auto width = static_cast<float>(layout.width);
    const auto height = static_cast<float>(layout.height);

    Gdiplus::GraphicsPath card;
    add_round_rect(card, Gdiplus::RectF(0.5f, 0.5f, width - 1, height - 1), static_cast<float>(layout.radius));
    Gdiplus::SolidBrush background(rgba(242, 30, 31, 33));
    g.FillPath(&background, &card);
    Gdiplus::Pen border(rgba(26, 255, 255, 255), 1.f);
    g.DrawPath(&border, &card);

    auto x = static_cast<float>(layout.pad_left + layout.ring + layout.gap);

    if (content.show_cover) {
        const Gdiplus::RectF box(x, (height - layout.cover) / 2.f, static_cast<float>(layout.cover),
            static_cast<float>(layout.cover));
        Gdiplus::GraphicsPath cover_path;
        add_round_rect(cover_path, box, static_cast<float>(layout.cover_radius));

        if (cover) {
            g.SetClip(&cover_path);
            g.DrawImage(cover, box);
            g.ResetClip();
        } else {
            Gdiplus::SolidBrush placeholder(rgba(255, 43, 43, 43));
            g.FillPath(&placeholder, &cover_path);
        }
        x += static_cast<float>(layout.cover + layout.gap);
    }

    const auto text_width = width - layout.pad_right - x;
    if (text_width <= 0)
        return;

    const auto family = create_font_family();
    const auto style = pick_font_style(*family);
    const Gdiplus::Font caption_font(family.get(), layout.font_caption, style, Gdiplus::UnitPixel);
    const Gdiplus::Font tag_font(family.get(), layout.font_tag, style, Gdiplus::UnitPixel);
    const Gdiplus::Font title_font(family.get(), layout.font_title, style, Gdiplus::UnitPixel);
    const Gdiplus::Font sub_font(family.get(), layout.font_sub, style, Gdiplus::UnitPixel);

    // 每行用固定行框，不用字型自己的行高：主題字型（DFHanziPen）行高是字級的 1.5 倍，三行加起來會塞滿卡片、文字貼上緣
    const auto caption_box = layout.font_caption * line_box_ratio;
    const auto title_box = layout.font_title * line_box_ratio;
    const auto sub_box = layout.font_sub * line_box_ratio;
    const auto block_height = caption_box + title_box + sub_box + layout.line_gap * 2;
    auto y = (height - block_height) / 2.f;

    // GDI+ 畫字的 y 是字高頂端，基線在其下方 ascent 處；這裡換算成讓字的視覺中心落在行框正中央的 y
    const auto em_height = static_cast<float>(family->GetEmHeight(style));
    const auto text_top = [&](float box_top, float box_height, float font_size) {
        const auto ascent = font_size * family->GetCellAscent(style) / em_height;
        return box_top + box_height / 2.f + font_size * ink_center_above_baseline - ascent;
    };
    const auto cell_height = [&](float font_size) {
        return font_size * (family->GetCellAscent(style) + family->GetCellDescent(style)) / em_height;
    };

    Gdiplus::StringFormat caption_format(Gdiplus::StringFormat::GenericTypographic());
    caption_format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap | Gdiplus::StringFormatFlagsMeasureTrailingSpaces);
    Gdiplus::StringFormat line_format;
    line_format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    line_format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);

    const Gdiplus::SolidBrush muted(rgba(255, 170, 170, 170));
    const Gdiplus::SolidBrush bright(rgba(255, 241, 241, 241));

    // 第一行：「下一首」＋來源標籤
    g.DrawString(content.caption.c_str(), -1, &caption_font,
        Gdiplus::PointF(x, text_top(y, caption_box, layout.font_caption)), &caption_format, &muted);

    if (content.show_tag && !content.tag.empty()) {
        Gdiplus::RectF caption_bounds;
        g.MeasureString(
            content.caption.c_str(), -1, &caption_font, Gdiplus::PointF(0, 0), &caption_format, &caption_bounds);

        Gdiplus::RectF tag_bounds;
        g.MeasureString(content.tag.c_str(), -1, &tag_font, Gdiplus::PointF(0, 0), &caption_format, &tag_bounds);

        const auto pill_height = layout.font_tag + layout.tag_pad_y * 2;
        const auto pill_x = x + caption_bounds.Width + layout.gap * 0.6f;
        const auto pill_y = y + (caption_box - pill_height) / 2.f;
        Gdiplus::GraphicsPath pill;
        add_round_rect(pill,
            Gdiplus::RectF(pill_x, pill_y, tag_bounds.Width + layout.tag_pad_x * 2, pill_height), pill_height / 2.f);
        Gdiplus::Pen pill_border(rgba(56, 255, 255, 255), 1.f);
        g.DrawPath(&pill_border, &pill);

        const Gdiplus::SolidBrush tag_brush(rgba(255, 207, 207, 207));
        g.DrawString(content.tag.c_str(), -1, &tag_font,
            Gdiplus::PointF(pill_x + layout.tag_pad_x, text_top(pill_y, pill_height, layout.font_tag)),
            &caption_format, &tag_brush);
    }
    y += caption_box + layout.line_gap;

    // 第二行：歌名；第三行：歌手
    g.DrawString(content.title.c_str(), -1, &title_font,
        Gdiplus::RectF(x, text_top(y, title_box, layout.font_title), text_width, cell_height(layout.font_title)),
        &line_format, &bright);
    y += title_box + layout.line_gap;

    g.DrawString(content.artist.c_str(), -1, &sub_font,
        Gdiplus::RectF(x, text_top(y, sub_box, layout.font_sub), text_width, cell_height(layout.font_sub)),
        &line_format, &muted);
}

// 動態那一層：倒數圈（每格重畫）
void draw_ring(Gdiplus::Bitmap& bitmap, const Layout& layout, const Content& content, double ratio, int number)
{
    Gdiplus::Graphics g(&bitmap);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);

    const auto center_x = layout.pad_left + layout.ring / 2.f;
    const auto center_y = layout.height / 2.f;
    const auto radius = (layout.ring - layout.stroke) / 2.f;
    const Gdiplus::RectF rect(center_x - radius, center_y - radius, radius * 2, radius * 2);

    Gdiplus::Pen track(rgba(255, 61, 61, 61), layout.stroke);
    g.DrawEllipse(&track, rect);

    if (content.stopped) {
        // 已停止：圈裡放一個播放符號，不倒數
        const auto t = layout.ring * 0.36f;
        const Gdiplus::PointF points[3]{{center_x - t * 0.32f, center_y - t * 0.5f},
            {center_x - t * 0.32f, center_y + t * 0.5f}, {center_x + t * 0.58f, center_y}};
        const Gdiplus::SolidBrush glyph(rgba(255, 241, 241, 241));
        g.FillPolygon(&glyph, points, 3);
        return;
    }

    // 從 12 點鐘方向順時針畫剩餘的弧，剩餘變少時弧的末端往逆時針退
    Gdiplus::Pen arc(rgba(255, 247, 11, 11), layout.stroke);
    arc.SetStartCap(Gdiplus::LineCapRound);
    arc.SetEndCap(Gdiplus::LineCapRound);

    if (ratio >= 0.9995)
        g.DrawEllipse(&arc, rect);
    else if (ratio > 0.0005)
        g.DrawArc(&arc, rect, -90.f, static_cast<float>(360.0 * ratio));

    if (content.show_number) {
        const auto family = create_font_family();
        const Gdiplus::Font font(family.get(), layout.font_number, pick_font_style(*family), Gdiplus::UnitPixel);
        Gdiplus::StringFormat format;
        format.SetAlignment(Gdiplus::StringAlignmentCenter);
        format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        const Gdiplus::SolidBrush brush(rgba(255, 241, 241, 241));
        const auto text = std::to_wstring(std::max(number, 0));
        g.DrawString(text.c_str(), -1, &font, rect, &format, &brush);
    }
}

// 32 位元預乘 alpha 的 DIB，給 UpdateLayeredWindow 用；Gdiplus::Bitmap 直接畫在它的記憶體上
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
        m_canvas = std::make_unique<Gdiplus::Bitmap>(width, height, width * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(m_bits));
        m_static_pixels.assign(static_cast<size_t>(width) * height, 0);
        m_static = std::make_unique<Gdiplus::Bitmap>(
            width, height, width * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(m_static_pixels.data()));
        return true;
    }

    void reset()
    {
        m_canvas.reset();
        m_static.reset();
        m_static_pixels.clear();
        if (m_dc && m_old_bitmap)
            SelectObject(m_dc.get(), m_old_bitmap);
        m_old_bitmap = nullptr;
        m_dc.reset();
        m_bitmap.reset();
        m_bits = nullptr;
        m_width = m_height = 0;
    }

    // 靜態層畫好之後，每格先把它複製到要送出的畫面上
    void copy_static_to_canvas()
    {
        if (m_bits && !m_static_pixels.empty())
            memcpy(m_bits, m_static_pixels.data(), m_static_pixels.size() * sizeof(uint32_t));
    }

    HDC dc() const { return m_dc.get(); }
    Gdiplus::Bitmap& canvas() { return *m_canvas; }
    Gdiplus::Bitmap& static_layer() { return *m_static; }
    bool is_valid() const { return m_canvas && m_static; }

    ~Surface() { reset(); }

private:
    wil::unique_hbitmap m_bitmap;
    wil::unique_hdc m_dc;
    HGDIOBJ m_old_bitmap{};
    void* m_bits{};
    int m_width{};
    int m_height{};
    std::vector<uint32_t> m_static_pixels;
    std::unique_ptr<Gdiplus::Bitmap> m_canvas;
    std::unique_ptr<Gdiplus::Bitmap> m_static;
};

LRESULT CALLBACK toast_wndproc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
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

// 餘弦緩動：起步與收尾都很緩，比原本的 smoothstep 柔
double ease(double progress)
{
    constexpr double pi = 3.14159265358979323846;
    progress = std::clamp(progress, 0.0, 1.0);
    return 0.5 - 0.5 * std::cos(progress * pi);
}

// 把卡片放到主視窗正上方：比主視窗自己擁有的其他浮動視窗（變暗頁、動畫截圖）高，但比蓋在主視窗前面的別的程式低。
// 使用者回報 LINE 蓋在 fb2k 前面時卡片會浮在 LINE 上面，所以顯示後明確指定層級，不靠系統預設的位置
void place_above_owner(HWND wnd, HWND owner)
{
    // 從主視窗往上找第一個不是主視窗自己擁有的視窗
    auto above = GetWindow(owner, GW_HWNDPREV);

    while (above && GetWindow(above, GW_OWNER) == owner)
        above = GetWindow(above, GW_HWNDPREV);

    auto insert_after = HWND_TOP;

    if (above) {
        const auto is_topmost = [](HWND window) { return (GetWindowLongPtr(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0; };

        // 上面那個是「永遠在最上面」的視窗、主視窗不是：排在它後面會讓卡片也變成最上層，改排在一般視窗的最上面
        insert_after = is_topmost(above) && !is_topmost(owner) ? HWND_NOTOPMOST : above;
    }

    SetWindowPos(wnd, insert_after, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

class Controller {
public:
    void start();
    void stop();
    void tick();
    void invalidate_next() { m_next_valid = false; }
    void on_new_track();
    void on_cover_loaded(uint64_t generation, std::shared_ptr<Gdiplus::Bitmap> cover);

private:
    enum class Mode { countdown, stopped };

    struct Want {
        RECT stage{};
        HWND owner{};
        Mode mode{Mode::countdown};
        double remaining{};
        double ratio{};
        NextTrack next;
    };

    std::optional<Want> evaluate();
    double smooth_position(double source, bool paused);
    void update_next(const metadb_handle_ptr& now_playing);
    bool ensure_window(HWND owner, const Layout& layout);
    void destroy_window();
    void rebuild_content(const Want& want, const Layout& layout);
    void start_cover_load(const metadb_handle_ptr& track, int size_px);
    void render_and_present(const Want& want, const Layout& layout, double scale);
    void set_interval(UINT ms);

    HWND m_timer_wnd{};
    HWND m_wnd{};
    HWND m_owner{};
    UINT m_interval{};
    // 倒數期間是否已把系統計時精度調成 1 ms（要成對還原）
    bool m_high_resolution_timer{};
    double m_alpha{};
    Clock::time_point m_last_tick{Clock::now()};
    // 換曲後到這個時間點之前不顯示
    Clock::time_point m_suppress_until{};
    // 播放位置濾波：上一次的來源位置與時間、當時是否暫停，以及目前吸收掉的偏差（顯示位置＝來源位置＋偏差）
    bool m_position_valid{};
    bool m_position_paused{};
    double m_position_source{};
    double m_position_offset{};
    Clock::time_point m_position_time{};

    std::optional<NextTrack> m_next;
    metadb_handle_ptr m_next_for;
    Clock::time_point m_next_time{};
    bool m_next_valid{};

    Surface m_surface;
    Content m_content;
    bool m_content_valid{};
    // 封面讀到了、或畫面重建了：靜態層要重畫
    bool m_static_dirty{true};
    // 內容的來源：下一首、模式、設定與縮放；任何一項變了就重畫靜態層
    metadb_handle_ptr m_content_track;
    uint32_t m_content_key{};
    int m_content_scale_key{};
    std::shared_ptr<Gdiplus::Bitmap> m_cover;
    uint64_t m_cover_generation{};
    std::optional<Want> m_last_want;
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

        WNDCLASSEX toast_class{};
        toast_class.cbSize = sizeof(toast_class);
        toast_class.lpfnWndProc = toast_wndproc;
        toast_class.hInstance = core_api::get_my_instance();
        toast_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
        toast_class.lpszClassName = toast_class_name;

        return RegisterClassEx(&timer_class) != 0 && RegisterClassEx(&toast_class) != 0;
    }();

    if (!classes_registered)
        return;

    m_timer_wnd = CreateWindowEx(0, timer_class_name, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
        core_api::get_my_instance(), nullptr);
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

    if (m_high_resolution_timer) {
        timeEndPeriod(1);
        m_high_resolution_timer = false;
    }
}

void Controller::set_interval(UINT ms)
{
    if (!m_timer_wnd)
        return;

    // 只有顯示動畫的期間才把系統計時精度調成 1 ms：預設 15.6 ms 的精度下，16 ms 的計時器常常要等到 31 ms 才觸發，
    // 動畫實測只剩 33～41 Hz；閒置時還原，不影響耗電
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

// 換曲了：提示講的下一首已經在播，舊的提示不用淡出，直接收掉；錄影量到原本換曲後舊提示還會殘留 0.2～0.3 秒
void Controller::on_new_track()
{
    m_suppress_until = Clock::now() + suppress_after_new_track;
    m_next_valid = false;
    m_position_valid = false;

    if (m_wnd) {
        destroy_window();
        set_interval(interval_idle_ms);
    }
}

void Controller::destroy_window()
{
    if (m_wnd) {
        DestroyWindow(m_wnd);
        m_wnd = nullptr;
    }
    m_owner = nullptr;
    m_surface.reset();
    m_content_valid = false;
    m_static_dirty = true;
    m_content_key = 0;
    m_content_track.reset();
    m_cover.reset();
    ++m_cover_generation;
    m_alpha = 0;
    m_last_want.reset();
}

void Controller::update_next(const metadb_handle_ptr& now_playing)
{
    const auto now = Clock::now();

    if (m_next_valid && m_next_for == now_playing && now - m_next_time < next_refresh_interval)
        return;

    m_next = resolve_next(now_playing);
    m_next_for = now_playing;
    m_next_time = now;
    m_next_valid = true;
}

// 讓倒數用的播放位置保持連續。顯示位置＝來源位置＋偏差；每次比較「來源位置實際前進多少」與「這段時間理應前進多少
// （播放中＝經過的時間，暫停中＝0）」，差距在 max_absorb_seconds 以內就算回報誤差，記進偏差讓顯示位置不跳，
// 差距更大（跳轉）就重設偏差照實顯示；偏差再以 offset_decay_seconds 慢慢歸零
double Controller::smooth_position(double source, bool paused)
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

std::optional<Controller::Want> Controller::evaluate()
{
    if (!enabled.get())
        return {};

    if (Clock::now() < m_suppress_until)
        return {};

    Want want;
    if (!FlatSplitterPanel::s_get_now_playing_stage(want.stage, want.owner))
        return {};

    if (IsIconic(want.owner) || !IsWindowVisible(want.owner))
        return {};

    const auto playback = playback_control::get();
    const auto count = static_cast<int>(std::clamp(seconds.get(), min_seconds, max_seconds));
    metadb_handle_ptr now_playing;

    if (playback->is_playing()) {
        if (!playback->get_now_playing(now_playing)) {
            m_position_valid = false;
            return {};
        }

        const auto length = playback->playback_get_length();
        // 長度未知（串流）或太短（不到 2N 秒）不顯示，免得一開始就出現
        if (length <= 0 || length < 2.0 * count) {
            m_position_valid = false;
            return {};
        }

        // 暫停中 ASIO+DSD 回報的位置會前後跳（見 menu_items.cpp），改用按下暫停時記下的位置，倒數圈才不會抖
        const auto paused = playback->is_paused();
        auto source_position = playback->playback_get_position();
        if (paused) {
            if (const auto paused_position = main_menu::get_paused_position())
                source_position = *paused_position;
        }

        // 再把暫停→繼續瞬間位置往前跳的小誤差吸收掉，圈與數字才不會在狂按暫停時一直跳
        const auto current_position = smooth_position(source_position, paused);

        // 最後 fade_out_seconds 秒就開始淡出，讓提示剛好在歌曲結束時消失（原本到歌曲結束才開始淡出，換曲後還會殘留）
        const auto remaining = length - current_position;
        if (remaining <= fade_out_seconds || remaining > count)
            return {};

        want.mode = Mode::countdown;
        want.remaining = remaining;
        want.ratio = remaining / count;
    } else {
        m_position_valid = false;

        if (playlist_manager::get()->queue_get_count() == 0)
            return {};

        want.mode = Mode::stopped;
    }

    update_next(now_playing);
    if (!m_next)
        return {};

    want.next = *m_next;
    return want;
}

bool Controller::ensure_window(HWND owner, const Layout& layout)
{
    if (m_wnd && m_owner != owner)
        destroy_window();

    if (m_wnd)
        return true;

    m_wnd = CreateWindowEx(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, toast_class_name,
        L"", WS_POPUP, 0, 0, layout.width, layout.height, owner, nullptr, core_api::get_my_instance(), nullptr);

    if (!m_wnd)
        return false;

    m_owner = owner;
    BOOL disable_transitions = TRUE;
    DwmSetWindowAttribute(m_wnd, DWMWA_TRANSITIONS_FORCEDISABLED, &disable_transitions, sizeof(disable_transitions));
    m_alpha = 0;
    return true;
}

void Controller::start_cover_load(const metadb_handle_ptr& track, int size_px)
{
    const auto generation = ++m_cover_generation;
    m_cover.reset();

    std::thread([track, size_px, generation] {
        std::shared_ptr<Gdiplus::Bitmap> result;

        try {
            abort_callback_dummy abort;
            pfc::list_single_ref_t<metadb_handle_ptr> items(track);
            pfc::list_single_ref_t<GUID> ids(album_art_ids::cover_front);
            const auto instance = album_art_manager_v2::get()->open(items, ids, abort);

            album_art_data_ptr data;
            if (instance->query(album_art_ids::cover_front, data, abort) && data.is_valid() && data->get_size() > 0) {
                wil::com_ptr<IStream> stream;
                stream.attach(SHCreateMemStream(static_cast<const BYTE*>(data->get_ptr()), gsl::narrow<UINT>(data->get_size())));

                if (stream) {
                    Gdiplus::Bitmap source(stream.get());
                    const auto width = static_cast<int>(source.GetWidth());
                    const auto height = static_cast<int>(source.GetHeight());

                    if (source.GetLastStatus() == Gdiplus::Ok && width > 0 && height > 0) {
                        auto scaled = std::make_shared<Gdiplus::Bitmap>(size_px, size_px, PixelFormat32bppPARGB);
                        Gdiplus::Graphics g(scaled.get());
                        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
                        // 取中間的正方形
                        const auto side = std::min(width, height);
                        g.DrawImage(&source, Gdiplus::Rect(0, 0, size_px, size_px), (width - side) / 2,
                            (height - side) / 2, side, side, Gdiplus::UnitPixel);
                        result = std::move(scaled);
                    }
                }
            }
        }
        catch (...) {
        }

        fb2k::inMainThread([result, generation] {
            if (g_alive)
                controller().on_cover_loaded(generation, result);
        });
    }).detach();
}

void Controller::on_cover_loaded(uint64_t generation, std::shared_ptr<Gdiplus::Bitmap> cover)
{
    if (generation != m_cover_generation)
        return;

    m_cover = std::move(cover);
    // 封面讀到了，靜態層要重畫
    m_static_dirty = true;
}

void Controller::rebuild_content(const Want& want, const Layout& layout)
{
    const auto stopped = want.mode == Mode::stopped;
    const uint32_t key = (stopped ? 1u : 0u) | (show_seconds_in_ring.get() ? 2u : 0u) | (show_cover.get() ? 4u : 0u)
        | (show_source_tag.get() ? 8u : 0u) | (want.next.from_queue ? 16u : 0u);

    const auto unchanged = m_content_valid && m_content_key == key && m_content_track == want.next.handle
        && m_content_scale_key == layout.width;

    if (unchanged && !m_static_dirty)
        return;

    if (!unchanged) {
        const auto track_changed = m_content_track != want.next.handle;
        const auto cover_wanted = show_cover.get();
        const auto cover_toggled_on = cover_wanted && (m_content_key & 4u) == 0;

        m_content.stopped = stopped;
        m_content.show_number = show_seconds_in_ring.get();
        m_content.show_cover = cover_wanted;
        m_content.show_tag = show_source_tag.get();
        m_content.caption = stopped ? nocturne_text::next_caption_stopped : nocturne_text::next_caption;
        m_content.tag = want.next.from_queue ? nocturne_text::next_source_queue : nocturne_text::next_source_list;
        m_content.title = format_track_text(want.next.handle, "$if2(%title%,%filename%)");
        m_content.artist = format_track_text(want.next.handle, "[%artist%]");

        if (cover_wanted && (track_changed || cover_toggled_on || m_content_scale_key != layout.width))
            start_cover_load(want.next.handle, layout.cover);
        else if (!cover_wanted)
            m_cover.reset();

        m_content_track = want.next.handle;
        m_content_key = key;
        m_content_scale_key = layout.width;
        m_content_valid = true;
    }

    m_static_dirty = false;
    draw_static_layer(m_surface.static_layer(), layout, m_content, m_cover.get());
}

void Controller::render_and_present(const Want& want, const Layout& layout, double scale)
{
    const auto eased = ease(m_alpha);
    m_surface.copy_static_to_canvas();
    draw_ring(m_surface.canvas(), layout, m_content, want.ratio, static_cast<int>(std::ceil(want.remaining - 0.0001)));

    // 淡入時從靠近邊緣那一側滑進來：放上面就從上往下，放下面就從下往上
    const auto corner = std::min(position.get(), position_count - 1);
    const auto on_left = corner == position_top_left || corner == position_bottom_left;
    const auto on_bottom = corner == position_bottom_right || corner == position_bottom_left;
    const auto slide = static_cast<int>(std::lround((1 - eased) * slide_dip * scale));

    POINT origin{on_left ? want.stage.left + layout.margin_side : want.stage.right - layout.margin_side - layout.width,
        on_bottom ? want.stage.bottom - layout.margin_bottom - layout.height + slide
                  : want.stage.top + layout.margin_top - slide};
    SIZE size{layout.width, layout.height};
    POINT source{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, static_cast<BYTE>(std::lround(eased * 255)), AC_SRC_ALPHA};

    UpdateLayeredWindow(m_wnd, nullptr, &origin, &size, m_surface.dc(), &source, 0, &blend, ULW_ALPHA);

    if (!IsWindowVisible(m_wnd)) {
        ShowWindow(m_wnd, SW_SHOWNOACTIVATE);
        place_above_owner(m_wnd, m_owner);
    }
}

void Controller::tick()
{
    const auto now = Clock::now();
    const auto elapsed = std::min(std::chrono::duration<double>(now - m_last_tick).count(), max_step_seconds);
    m_last_tick = now;

    const auto want = evaluate();

    if (want) {
        m_last_want = want;
    } else if (m_wnd && m_last_want
        && (!IsWindow(m_last_want->owner) || IsIconic(m_last_want->owner) || !IsWindowVisible(m_last_want->owner))) {
        // 主視窗縮小或不見了：不淡出，直接收掉
        destroy_window();
        set_interval(interval_idle_ms);
        return;
    }

    if (!want && !m_wnd) {
        set_interval(interval_idle_ms);
        return;
    }

    if (!m_last_want)
        return;

    // 淡出時沿用最後一次的內容與位置
    const auto& shown = want ? *want : *m_last_want;
    const auto dpi = std::max<UINT>(GetDpiForWindow(shown.owner), USER_DEFAULT_SCREEN_DPI);
    const auto scale = static_cast<double>(dpi) / USER_DEFAULT_SCREEN_DPI;
    const Layout layout(scale);

    // 第一次出現的這一格要建視窗、畫圖，會花一段時間；這一格先停在全透明，時間從下一格才開始算，
    // 不然淡入的第一格就已經有兩成多亮度（錄影量到 21～27%）
    const auto creating = !m_wnd;

    if (!ensure_window(shown.owner, layout) || !m_surface.ensure(layout.width, layout.height))
        return;

    if (want) {
        rebuild_content(shown, layout);
        if (!creating)
            m_alpha = std::min(1.0, m_alpha + elapsed / fade_in_seconds);
    } else {
        m_alpha = std::max(0.0, m_alpha - elapsed / fade_out_seconds);

        if (m_alpha <= 0) {
            destroy_window();
            set_interval(interval_idle_ms);
            return;
        }
    }

    if (!m_content_valid)
        return;

    render_and_present(shown, layout, scale);
    set_interval(interval_active_ms);

    if (creating)
        m_last_tick = Clock::now();
}

class NextTrackToastInitQuit : public initquit {
public:
    void on_init() override
    {
        g_alive = true;
        controller().start();
        font_warm_up::start();
    }

    void on_quit() override
    {
        g_alive = false;
        font_warm_up::stop();
        controller().stop();
    }
};

initquit_factory_t<NextTrackToastInitQuit> next_track_toast_initquit;

// 佇列有變動：馬上重算下一首。停止中使用者把佇列清空，展開頁就沒有東西可以顯示，自動收合（跟「停止就收」一致）
// 只看使用者移除的那一種：播放推進時佇列變空，不能收（那時下一首正要開始播）
class QueueChangeCallback : public playback_queue_callback {
public:
    void on_changed(t_change_origin origin) override
    {
        controller().invalidate_next();

        if (origin == changed_user_removed && !play_control::get()->is_playing()
            && playlist_manager::get()->queue_get_count() == 0 && FlatSplitterPanel::s_is_now_playing_expanded())
            FlatSplitterPanel::s_toggle_now_playing();
    }
};

service_factory_single_t<QueueChangeCallback> queue_change_callback;

// 換曲時馬上收掉提示（見 Controller::on_new_track）
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

} // namespace cui::next_track_toast
