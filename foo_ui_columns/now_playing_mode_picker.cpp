#include "pch.h"

#include "now_playing_mode_picker.h"

#include "nocturne_segment.h"
#include "next_track_toast.h"
#include "popup_dismiss.h"
#include "nocturne_surface.h"
#include "nocturne_text.h"
#include "splitter.h"
#include "wic.h"

// 展開頁模式的版面縮圖卡，說明見 now_playing_mode_picker.h

namespace cui::mode_picker {

namespace {

constexpr wchar_t class_name[] = L"columns_ui_now_playing_mode_picker";

// ---- 尺寸（DIP），照預覽頁 B ----
constexpr float shadow_margin_dip = 30.f; // 卡片四周留給陰影的邊
constexpr float shadow_offset_dip = 10.f; // 陰影往下
constexpr float shadow_blur_dip = 12.f;
constexpr float shadow_strength = 0.42f;
constexpr float pad_dip = 12.f;
constexpr float gap_dip = 10.f;
constexpr float tile_pad_dip = 6.f;
constexpr float thumb_w_dip = 138.f;
constexpr float thumb_h_dip = 68.f;
constexpr float label_gap_dip = 7.f;
constexpr float label_h_dip = 20.f;
constexpr float card_radius_dip = nocturne_surface::card_radius_dip; // 統一圓角（原本 16），霧面玻璃材質保留
constexpr float tile_radius_dip = 11.f;
constexpr float thumb_radius_dip = 7.f;
constexpr float label_font_dip = 15.f;
constexpr float note_font_dip = 11.5f;
// 左上角按鈕在展開頁是 margin 12、56×56（main.xml 的 mode-button）：卡片左上角放在 (14, 70)，貼著按鈕下緣
constexpr float anchor_x_dip = 14.f;
constexpr float anchor_y_dip = 70.f;
constexpr int max_columns = 3;
// 縮圖的設計座標（同預覽頁的 SVG viewBox 160×79）
constexpr float design_w = 160.f;
constexpr float design_h = 79.f;
// 霧面玻璃：模糊半徑、飽和度、壓暗的底色與比例（預覽頁 rgba(28,29,35,.58) 疊在 blur＋saturate(1.25) 上）
constexpr float glass_blur_dip = 14.f;
constexpr float glass_saturation = 1.25f;
constexpr float glass_tint_alpha = 0.58f;
constexpr int glass_tint_r = 28, glass_tint_g = 29, glass_tint_b = 35;
// 出現：淡入＋往下滑
constexpr ULONGLONG appear_ms = 160;
constexpr float appear_slide_dip = 6.f;
// 點外面關掉後這麼短時間內又被叫開＝使用者是點左上角按鈕想把它關掉
constexpr ULONGLONG reopen_guard_ms = 400;

// 自動關閉的檢查計時器在 popup_dismiss 裡
constexpr UINT_PTR appear_timer_id = 2;
// 關卡時淡出（2026-11-14 跟其他右鍵卡一致，100 ms）；選了模式那條路不淡出（切換時的交叉淡化會截展開頁，卡片不能還在）
constexpr UINT_PTR fade_timer_id = 3;
constexpr ULONGLONG fade_ms = 100;

// 縮圖裡歌詞色條的長短（同預覽頁）
constexpr float lyric_widths[] = {0.78f, 0.62f, 0.9f, 0.7f, 0.84f, 0.58f, 0.74f};

Gdiplus::Color rgba(int r, int g, int b, double a)
{
    return Gdiplus::Color(static_cast<BYTE>(std::clamp(a, 0.0, 1.0) * 255.0 + 0.5), static_cast<BYTE>(r),
        static_cast<BYTE>(g), static_cast<BYTE>(b));
}

// 主題字型（同 VU 表與下一首提示）
std::unique_ptr<Gdiplus::FontFamily> theme_family()
{
    const auto log_font = fonts::get_log_font_with_fallback(fonts::labels_font_id);
    auto family = std::make_unique<Gdiplus::FontFamily>(log_font.lfFaceName);
    if (family->GetLastStatus() != Gdiplus::Ok)
        family.reset(Gdiplus::FontFamily::GenericSansSerif()->Clone());
    return family;
}

Gdiplus::FontStyle regular_style(const Gdiplus::FontFamily& family)
{
    if (family.IsStyleAvailable(Gdiplus::FontStyleRegular))
        return Gdiplus::FontStyleRegular;
    if (family.IsStyleAvailable(Gdiplus::FontStyleBold))
        return Gdiplus::FontStyleBold;
    return Gdiplus::FontStyleItalic;
}

void add_rounded(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& rect, float radius)
{
    radius = std::min({radius, rect.Width / 2.f, rect.Height / 2.f});
    if (radius <= 0.01f) {
        path.AddRectangle(rect);
        return;
    }
    const auto d = radius * 2.f;
    path.AddArc(rect.X, rect.Y, d, d, 180.f, 90.f);
    path.AddArc(rect.X + rect.Width - d, rect.Y, d, d, 270.f, 90.f);
    path.AddArc(rect.X + rect.Width - d, rect.Y + rect.Height - d, d, d, 0.f, 90.f);
    path.AddArc(rect.X, rect.Y + rect.Height - d, d, d, 90.f, 90.f);
    path.CloseFigure();
}

void fill_rounded(Gdiplus::Graphics& g, const Gdiplus::Brush& brush, const Gdiplus::RectF& rect, float radius)
{
    Gdiplus::GraphicsPath path;
    add_rounded(path, rect, radius);
    g.FillPath(&brush, &path);
}

void stroke_rounded(Gdiplus::Graphics& g, const Gdiplus::Pen& pen, const Gdiplus::RectF& rect, float radius)
{
    Gdiplus::GraphicsPath path;
    add_rounded(path, rect, radius);
    g.DrawPath(&pen, &path);
}

// 圓角矩形的有號距離（裡面是負的），以中心為原點
float rounded_rect_sdf(float px, float py, float half_w, float half_h, float radius)
{
    const auto qx = std::abs(px) - (half_w - radius);
    const auto qy = std::abs(py) - (half_h - radius);
    const auto ox = std::max(qx, 0.f);
    const auto oy = std::max(qy, 0.f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.f) - radius;
}

// 三次方塊模糊（近似高斯），邊緣外當作跟邊緣一樣
void box_blur(std::vector<float>& plane, int width, int height, int blur_r)
{
    if (blur_r <= 0 || width <= 0 || height <= 0)
        return;
    std::vector<float> temp(plane.size());
    const auto span = static_cast<float>(2 * blur_r + 1);
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < height; ++y) {
            const auto row = static_cast<size_t>(y) * width;
            float sum = 0;
            for (int x = -blur_r; x <= blur_r; ++x)
                sum += plane[row + std::clamp(x, 0, width - 1)];
            for (int x = 0; x < width; ++x) {
                temp[row + x] = sum / span;
                sum += plane[row + std::clamp(x + blur_r + 1, 0, width - 1)] - plane[row + std::clamp(x - blur_r, 0, width - 1)];
            }
        }
        for (int x = 0; x < width; ++x) {
            float sum = 0;
            for (int y = -blur_r; y <= blur_r; ++y)
                sum += temp[static_cast<size_t>(std::clamp(y, 0, height - 1)) * width + x];
            for (int y = 0; y < height; ++y) {
                plane[static_cast<size_t>(y) * width + x] = sum / span;
                sum += temp[static_cast<size_t>(std::clamp(y + blur_r + 1, 0, height - 1)) * width + x]
                    - temp[static_cast<size_t>(std::clamp(y - blur_r, 0, height - 1)) * width + x];
            }
        }
    }
}

void draw_text(Gdiplus::Graphics& g, const wchar_t* text, const Gdiplus::Font& font, const Gdiplus::Color& colour,
    const Gdiplus::RectF& box)
{
    Gdiplus::StringFormat format;
    format.SetAlignment(Gdiplus::StringAlignmentCenter);
    format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    const Gdiplus::SolidBrush brush(colour);
    g.DrawString(text, -1, &font, box, &format, &brush);
}

// ---- 正在播放這首的封面：縮成縮圖要的大小，每首只解一次 ----
struct CoverCache {
    album_art_data::ptr data;
    int size{};
    std::vector<uint32_t> pixels; // BGRA（非預乘），size×size
    std::array<int, 3> tint{236, 150, 170};
    bool valid{};
};

CoverCache& cover_cache()
{
    static CoverCache cache;
    return cache;
}

// 封面平均色調亮成頻譜縮圖的光（飽和度上限 0.85，同封面虛化背景）
std::array<int, 3> tone_up(double r, double g, double b)
{
    r /= 255.0;
    g /= 255.0;
    b /= 255.0;
    const auto mx = std::max({r, g, b});
    const auto mn = std::min({r, g, b});
    double h = 0, s = 0;
    const auto l = (mx + mn) / 2.0;
    if (mx != mn) {
        const auto d = mx - mn;
        s = l > 0.5 ? d / (2.0 - mx - mn) : d / (mx + mn);
        if (mx == r)
            h = (g - b) / d + (g < b ? 6.0 : 0.0);
        else if (mx == g)
            h = (b - r) / d + 2.0;
        else
            h = (r - g) / d + 4.0;
        h /= 6.0;
    }
    s = std::clamp(s * 2.0, 0.45, 0.85);
    constexpr double lightness = 0.68;
    const auto q = lightness < 0.5 ? lightness * (1 + s) : lightness + s - lightness * s;
    const auto p = 2 * lightness - q;
    const auto channel = [p, q](double t) {
        if (t < 0)
            t += 1;
        if (t > 1)
            t -= 1;
        if (t < 1.0 / 6)
            return p + (q - p) * 6 * t;
        if (t < 0.5)
            return q;
        if (t < 2.0 / 3)
            return p + (q - p) * (2.0 / 3 - t) * 6;
        return p;
    };
    return {static_cast<int>(std::lround(channel(h + 1.0 / 3) * 255)), static_cast<int>(std::lround(channel(h) * 255)),
        static_cast<int>(std::lround(channel(h - 1.0 / 3) * 255))};
}

const CoverCache& load_cover(int size)
{
    auto& cache = cover_cache();
    album_art_data::ptr data;
    try {
        data = now_playing_album_art_notify_manager::get()->current();
    } catch (const std::exception&) {
    }

    if (cache.valid && cache.size == size && album_art_data::equals(cache.data, data))
        return cache;

    cache = {};
    cache.data = data;
    cache.size = size;
    if (!data.is_valid() || size <= 0)
        return cache;

    try {
        const auto factory = wic::create_factory();
        const auto decoder = wic::create_decoder_from_data(data->get_ptr(), data->get_size(), factory);
        const auto frame = wic::get_image_frame(decoder);
        UINT w{}, h{};
        wic::check_hresult(frame->GetSize(&w, &h));
        if (w == 0 || h == 0)
            return cache;
        // 短邊縮到 size，再裁中間的正方形（同預覽頁的 slice）
        const auto scale = static_cast<double>(size) / std::min(w, h);
        const auto rw = std::max(size, static_cast<int>(std::lround(w * scale)));
        const auto rh = std::max(size, static_cast<int>(std::lround(h * scale)));
        const auto resized = wic::resize_bitmap_source(frame, rw, rh, factory);
        const WICRect crop{(rw - size) / 2, (rh - size) / 2, size, size};
        cache.pixels.resize(static_cast<size_t>(size) * size);
        wic::check_hresult(resized->CopyPixels(&crop, size * 4, gsl::narrow<UINT>(cache.pixels.size() * 4),
            reinterpret_cast<BYTE*>(cache.pixels.data())));
        double r = 0, g = 0, b = 0;
        for (const auto pixel : cache.pixels) {
            b += pixel & 0xFF;
            g += (pixel >> 8) & 0xFF;
            r += (pixel >> 16) & 0xFF;
        }
        const auto n = static_cast<double>(cache.pixels.size());
        cache.tint = tone_up(r / n, g / n, b / n);
        cache.valid = true;
    } catch (const std::exception&) {
        cache.pixels.clear();
    }
    return cache;
}

// ---- 卡片 ----

struct Picker {
    HWND wnd{};
    std::vector<Item> items;
    main_menu::NowPlayingMode current{};
    std::function<void(main_menu::NowPlayingMode)> on_choose;
    bool on_stage{};
    float k{1.f};
    int columns{1};
    int width{};
    int height{};
    Gdiplus::RectF card;
    std::vector<Gdiplus::RectF> tiles; // 每格（縮圖＋名稱），視窗座標
    std::vector<uint32_t> backdrop;    // 卡片範圍的霧面底（不透明 BGRA，卡片大小）；空的＝用實心深色
    int backdrop_w{};
    int backdrop_h{};
    std::vector<float> shadow;
    std::vector<uint32_t> cover_pixels;
    int cover_size{};
    std::array<int, 3> tint{236, 150, 170};
    int hover{-1};
    int focus{-1};
    bool keyboard{};
    bool tracking{};
    POINT origin{}; // 停好時視窗左上角（螢幕座標）
    ULONGLONG appear_start{};
    BYTE alpha{};
    bool closing{};
    ULONGLONG fade_start{};
    BYTE fade_from{};
};

std::unique_ptr<Picker>& current_picker()
{
    static std::unique_ptr<Picker> picker;
    return picker;
}

ULONGLONG& last_outside_close()
{
    static ULONGLONG tick{};
    return tick;
}

float px(const Picker& p, float dip)
{
    return std::round(dip * p.k);
}

void build_layout(Picker& p)
{
    const auto count = static_cast<int>(p.items.size());
    p.columns = std::clamp(count, 1, max_columns);
    const auto rows = (count + p.columns - 1) / p.columns;
    const auto tile_w = px(p, thumb_w_dip) + 2 * px(p, tile_pad_dip);
    const auto tile_h = px(p, tile_pad_dip) + px(p, thumb_h_dip) + px(p, label_gap_dip) + px(p, label_h_dip) + px(p, tile_pad_dip);
    const auto gap = px(p, gap_dip);
    const auto pad = px(p, pad_dip);
    const auto card_w = 2 * pad + p.columns * tile_w + (p.columns - 1) * gap;
    const auto card_h = 2 * pad + rows * tile_h + (rows - 1) * gap;
    const auto margin = px(p, shadow_margin_dip);
    p.card = {margin, margin, card_w, card_h};
    p.width = static_cast<int>(card_w + 2 * margin);
    p.height = static_cast<int>(card_h + 2 * margin);
    p.tiles.clear();
    for (int i = 0; i < count; ++i)
        p.tiles.push_back({p.card.X + pad + (i % p.columns) * (tile_w + gap), p.card.Y + pad + (i / p.columns) * (tile_h + gap),
            tile_w, tile_h});

    // 陰影：卡片形狀往下移、模糊
    std::vector<float> mask(static_cast<size_t>(p.width) * p.height, 0.f);
    const auto hw = card_w / 2.f;
    const auto hh = card_h / 2.f;
    const auto cx = p.card.X + hw;
    const auto cy = p.card.Y + hh + px(p, shadow_offset_dip);
    const auto radius = px(p, card_radius_dip);
    for (int y = 0; y < p.height; ++y)
        for (int x = 0; x < p.width; ++x)
            mask[static_cast<size_t>(y) * p.width + x]
                = std::clamp(0.5f - rounded_rect_sdf(x + 0.5f - cx, y + 0.5f - cy, hw, hh, radius), 0.f, 1.f);
    box_blur(mask, p.width, p.height, std::max(1, static_cast<int>(px(p, shadow_blur_dip))));
    for (auto& value : mask)
        value *= shadow_strength;
    p.shadow = std::move(mask);
}

// 截下卡片後面那塊展開頁，模糊、加飽和、壓暗成霧面玻璃（要在卡片顯示前做）
void build_backdrop(Picker& p)
{
    p.backdrop.clear();
    if (!p.on_stage)
        return;
    RECT stage{};
    HWND owner{};
    const auto stage_bitmap = panels::splitter::FlatSplitterPanel::s_capture_now_playing_stage(stage, owner);
    if (!stage_bitmap)
        return;

    const auto w = static_cast<int>(p.card.Width);
    const auto h = static_cast<int>(p.card.Height);
    if (w <= 0 || h <= 0)
        return;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = -h;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    const auto screen_dc = wil::GetDC(nullptr);
    void* bits{};
    const wil::unique_hbitmap dib(CreateDIBSection(screen_dc.get(), &info, DIB_RGB_COLORS, &bits, nullptr, 0));
    if (!dib || !bits)
        return;
    {
        const wil::unique_hdc source_dc(CreateCompatibleDC(screen_dc.get()));
        const wil::unique_hdc target_dc(CreateCompatibleDC(screen_dc.get()));
        const auto _select_source = wil::SelectObject(source_dc.get(), stage_bitmap.get());
        const auto _select_target = wil::SelectObject(target_dc.get(), dib.get());
        // 卡片在螢幕上的位置（停好時）扣掉展開頁左上角
        const auto left = p.origin.x + static_cast<int>(p.card.X) - stage.left;
        const auto top = p.origin.y + static_cast<int>(p.card.Y) - stage.top;
        BitBlt(target_dc.get(), 0, 0, w, h, source_dc.get(), left, top, SRCCOPY);
    }
    GdiFlush();

    const auto* source = static_cast<const uint32_t*>(bits);
    const auto count = static_cast<size_t>(w) * h;
    std::vector<float> planes[3];
    for (auto& plane : planes)
        plane.resize(count);
    for (size_t i = 0; i < count; ++i) {
        planes[0][i] = static_cast<float>(source[i] & 0xFF);
        planes[1][i] = static_cast<float>((source[i] >> 8) & 0xFF);
        planes[2][i] = static_cast<float>((source[i] >> 16) & 0xFF);
    }
    const auto blur_r = std::max(1, static_cast<int>(px(p, glass_blur_dip)));
    for (auto& plane : planes)
        box_blur(plane, w, h, blur_r);

    p.backdrop.resize(count);
    const float tint_bgr[3] = {static_cast<float>(glass_tint_b), static_cast<float>(glass_tint_g), static_cast<float>(glass_tint_r)};
    for (size_t i = 0; i < count; ++i) {
        const auto luma = 0.114f * planes[0][i] + 0.587f * planes[1][i] + 0.299f * planes[2][i];
        uint32_t out = 0xFF000000;
        for (int c = 0; c < 3; ++c) {
            const auto saturated = std::clamp(luma + (planes[c][i] - luma) * glass_saturation, 0.f, 255.f);
            const auto mixed = saturated * (1.f - glass_tint_alpha) + tint_bgr[c] * glass_tint_alpha;
            out |= static_cast<uint32_t>(std::clamp(mixed, 0.f, 255.f) + 0.5f) << (8 * c);
        }
        p.backdrop[i] = out;
    }
    p.backdrop_w = w;
    p.backdrop_h = h;
}

// 一種模式的版面縮圖（照預覽頁 B 的 SVG：160×79 的設計座標）
void draw_thumbnail(Gdiplus::Graphics& g, const Picker& p, const Gdiplus::RectF& r, main_menu::NowPlayingMode mode,
    Gdiplus::Bitmap* cover)
{
    using main_menu::NowPlayingMode;
    const auto sx = r.Width / design_w;
    const auto sy = r.Height / design_h;
    const auto X = [&](float u) { return r.X + u * sx; };
    const auto Y = [&](float v) { return r.Y + v * sy; };
    const auto pill = [&](float x, float y, float w, float h, const Gdiplus::Color& colour) {
        const Gdiplus::SolidBrush brush(colour);
        fill_rounded(g, brush, {X(x), Y(y), w * sx, h * sy}, h * sy / 2.f);
    };
    const auto cover_at = [&](float x, float y, float size) {
        const Gdiplus::RectF rect{X(x), Y(y), size * sx, size * sx};
        if (cover) {
            g.DrawImage(cover, rect);
        } else {
            const Gdiplus::SolidBrush placeholder(rgba(255, 255, 255, 0.16));
            g.FillRectangle(&placeholder, rect);
        }
    };
    const auto lyric_lines = [&](float y0, int count, float gap) {
        const auto highlight = count / 2 - (count > 4 ? 1 : 0);
        for (int i = 0; i < count; ++i)
            pill(91.f, y0 + i * gap, 50.f * lyric_widths[i % std::size(lyric_widths)], 2.6f,
                i == highlight ? rgba(255, 92, 82, 0.95) : rgba(255, 255, 255, 0.40));
    };
    const auto meter = [&](float x, float y, float w, float h) {
        const Gdiplus::RectF box{X(x), Y(y), w * sx, h * sy};
        const Gdiplus::SolidBrush fill(rgba(255, 200, 140, 0.08));
        fill_rounded(g, fill, box, 2.f * sx);
        const Gdiplus::Pen border(rgba(255, 255, 255, 0.30), std::max(1.f, 0.6f * sx));
        stroke_rounded(g, border, box, 2.f * sx);
        const auto cx = X(x + w / 2.f);
        const auto cy = Y(y + h * 0.92f);
        const auto radius = std::min(w * 0.42f, h * 0.78f) * sx;
        constexpr float to_degrees = 57.2957795f;
        const Gdiplus::Pen arc(rgba(255, 255, 255, 0.60), std::max(1.f, 0.7f * sx));
        g.DrawArc(&arc, cx - radius, cy - radius, 2 * radius, 2 * radius, -2.4f * to_degrees, (2.4f - 0.74f) * to_degrees);
        const Gdiplus::Pen needle(rgba(255, 255, 255, 0.95), std::max(1.f, 0.7f * sx));
        g.DrawLine(&needle, cx, cy, cx + radius * std::cos(-1.25f), cy + radius * std::sin(-1.25f));
    };
    const auto bars = [&](float base_y, float x_step, float bar_w, float min_h, float range) {
        const Gdiplus::SolidBrush brush(rgba(p.tint[0], p.tint[1], p.tint[2], 0.85));
        for (int i = 0; i < 14; ++i) {
            const auto bar_h = min_h + range * std::abs(std::sin(i * 1.7f + 0.6f)) * (1.f - i / 22.f);
            fill_rounded(g, brush, {X(91.f + i * x_step), Y(base_y - bar_h), bar_w * sx, bar_h * sy}, 1.f * sx);
        }
    };

    if (mode == NowPlayingMode::cover) {
        cover_at(61.f, 9.f, 38.f);
        pill(58.f, 53.f, 44.f, 3.4f, rgba(255, 255, 255, 0.75));
        pill(64.f, 60.f, 32.f, 2.6f, rgba(255, 255, 255, 0.40));
        return;
    }

    cover_at(27.f, 9.f, 45.f);
    pill(27.f, 58.f, 34.f, 3.2f, rgba(255, 255, 255, 0.75));
    pill(27.f, 64.5f, 26.f, 2.4f, rgba(255, 255, 255, 0.42));
    pill(27.f, 70.f, 20.f, 2.f, rgba(255, 255, 255, 0.28));

    switch (mode) {
    case NowPlayingMode::lyrics:
        lyric_lines(18.f, 7, 6.6f);
        break;
    case NowPlayingMode::vu:
        meter(91.f, 26.f, 23.f, 15.f);
        meter(117.f, 26.f, 23.f, 15.f);
        break;
    case NowPlayingMode::spectrum:
        bars(46.f, 3.6f, 2.4f, 6.f, 18.f);
        break;
    case NowPlayingMode::lyrics_vu:
        lyric_lines(10.f, 6, 6.4f);
        meter(91.f, 58.f, 22.f, 13.f);
        meter(116.f, 58.f, 22.f, 13.f);
        break;
    case NowPlayingMode::lyrics_spectrum:
        lyric_lines(10.f, 6, 6.4f);
        bars(71.f, 3.4f, 2.2f, 3.f, 9.f);
        break;
    default:
        break;
    }
}

void present(Picker& p, const std::vector<uint32_t>& pixels)
{
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = p.width;
    info.bmiHeader.biHeight = -p.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    const auto screen_dc = wil::GetDC(nullptr);
    void* bits{};
    const wil::unique_hbitmap dib(CreateDIBSection(screen_dc.get(), &info, DIB_RGB_COLORS, &bits, nullptr, 0));
    if (!dib || !bits)
        return;
    memcpy(bits, pixels.data(), pixels.size() * 4);
    const wil::unique_hdc memory_dc(CreateCompatibleDC(screen_dc.get()));
    const auto _select = wil::SelectObject(memory_dc.get(), dib.get());
    SIZE size{p.width, p.height};
    POINT source{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, p.alpha, AC_SRC_ALPHA};
    UpdateLayeredWindow(p.wnd, screen_dc.get(), nullptr, &size, memory_dc.get(), &source, 0, &blend, ULW_ALPHA);
}

void render(Picker& p)
{
    const auto w = p.width;
    const auto h = p.height;
    std::vector<uint32_t> pixels(static_cast<size_t>(w) * h);

    // 陰影＋卡片底（霧面玻璃或實心深色），自己合成：卡片邊緣依覆蓋率反鋸齒
    {
        const auto hw = p.card.Width / 2.f;
        const auto hh = p.card.Height / 2.f;
        const auto cx = p.card.X + hw;
        const auto cy = p.card.Y + hh;
        const auto radius = px(p, card_radius_dip);
        const auto card_left = static_cast<int>(p.card.X);
        const auto card_top = static_cast<int>(p.card.Y);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const auto index = static_cast<size_t>(y) * w + x;
                const auto shadow_a = p.shadow[index];
                const auto coverage = std::clamp(0.5f - rounded_rect_sdf(x + 0.5f - cx, y + 0.5f - cy, hw, hh, radius), 0.f, 1.f);
                if (coverage <= 0.f) {
                    pixels[index] = static_cast<uint32_t>(shadow_a * 255.f + 0.5f) << 24;
                    continue;
                }
                // 截不到背後畫面時的實心底，跟其他浮動元件統一（nocturne_surface.h，預乘前）
                uint32_t fill = static_cast<uint32_t>(nocturne_surface::fill_alpha) << 24
                    | static_cast<uint32_t>(nocturne_surface::fill_grey) << 16
                    | static_cast<uint32_t>(nocturne_surface::fill_grey) << 8 | nocturne_surface::fill_grey;
                float fill_a = nocturne_surface::fill_alpha / 255.f;
                const auto bx = x - card_left;
                const auto by = y - card_top;
                if (!p.backdrop.empty() && bx >= 0 && by >= 0 && bx < p.backdrop_w && by < p.backdrop_h) {
                    fill = p.backdrop[static_cast<size_t>(by) * p.backdrop_w + bx];
                    fill_a = 1.f;
                }
                const auto a = coverage * fill_a;
                const auto out_a = a + shadow_a * (1.f - a);
                uint32_t out = static_cast<uint32_t>(std::clamp(out_a, 0.f, 1.f) * 255.f + 0.5f) << 24;
                for (int c = 0; c < 3; ++c) {
                    const auto value = ((fill >> (8 * c)) & 0xFF) * a; // 陰影是黑色，預乘後只剩卡片的顏色
                    out |= static_cast<uint32_t>(std::clamp(value, 0.f, 255.f) + 0.5f) << (8 * c);
                }
                pixels[index] = out;
            }
    }

    {
        Gdiplus::Bitmap canvas(w, h, w * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(pixels.data()));
        Gdiplus::Graphics g(&canvas);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);

        // 卡片細白邊＋上緣一道亮線
        const auto card_radius = px(p, card_radius_dip);
        {
            const Gdiplus::Pen border(nocturne_surface::edge_colour(), 1.f);
            stroke_rounded(g, border, {p.card.X + 0.5f, p.card.Y + 0.5f, p.card.Width - 1.f, p.card.Height - 1.f}, card_radius);
            Gdiplus::GraphicsPath card_path;
            add_rounded(card_path, p.card, card_radius);
            g.SetClip(&card_path);
            const Gdiplus::Pen sheen(rgba(255, 255, 255, 0.12), 1.f);
            g.DrawLine(&sheen, p.card.X + card_radius, p.card.Y + 1.f, p.card.X + p.card.Width - card_radius, p.card.Y + 1.f);
            g.ResetClip();
        }

        const auto family = theme_family();
        const Gdiplus::Font label_font(family.get(), label_font_dip * p.k, regular_style(*family), Gdiplus::UnitPixel);
        const Gdiplus::Font note_font(family.get(), note_font_dip * p.k, regular_style(*family), Gdiplus::UnitPixel);

        std::unique_ptr<Gdiplus::Bitmap> cover;
        if (!p.cover_pixels.empty())
            cover = std::make_unique<Gdiplus::Bitmap>(p.cover_size, p.cover_size, p.cover_size * 4, PixelFormat32bppARGB,
                reinterpret_cast<BYTE*>(p.cover_pixels.data()));

        const auto tile_pad = px(p, tile_pad_dip);
        const auto thumb_w = px(p, thumb_w_dip);
        const auto thumb_h = px(p, thumb_h_dip);
        const auto thumb_radius = px(p, thumb_radius_dip);
        const auto tile_radius = px(p, tile_radius_dip);
        for (size_t i = 0; i < p.items.size(); ++i) {
            const auto& item = p.items[i];
            const auto& tile = p.tiles[i];
            const auto selected = item.mode == p.current;
            const auto hovered = item.available && static_cast<int>(i) == p.hover;

            // 2026-11-14 分段切換樣式（同 VU 表／頻譜卡）：目前的模式整格鋪亮底（白 13%），滑過或鍵盤焦點鋪淡底（白 6%），沒有紅框
            const auto focused = p.keyboard && static_cast<int>(i) == p.focus;
            if (selected || hovered || focused) {
                const Gdiplus::SolidBrush plate(rgba(255, 255, 255, selected ? 0.13 : 0.06));
                fill_rounded(g, plate, tile, tile_radius);
            }

            const Gdiplus::RectF thumb{tile.X + tile_pad, tile.Y + tile_pad, thumb_w, thumb_h};
            Gdiplus::GraphicsPath thumb_path;
            add_rounded(thumb_path, thumb, thumb_radius);
            g.SetClip(&thumb_path);
            const Gdiplus::SolidBrush thumb_fill(rgba(255, 255, 255, 0.06));
            g.FillRectangle(&thumb_fill, thumb);
            draw_thumbnail(g, p, thumb, item.mode, cover.get());
            if (!item.available) {
                const Gdiplus::SolidBrush veil(rgba(16, 17, 21, 0.66));
                g.FillRectangle(&veil, thumb);
            } else if (!selected) {
                // 沒選的淡一點（約 72%），滑過時亮一點
                const Gdiplus::SolidBrush dim(rgba(glass_tint_r, glass_tint_g, glass_tint_b, hovered || focused ? 0.12 : 0.28));
                g.FillRectangle(&dim, thumb);
            }
            g.ResetClip();
            if (!item.available)
                draw_text(g, nocturne_text::mode_picker_unavailable, note_font, rgba(244, 240, 230, 0.72), thumb);


            const Gdiplus::RectF label_box{tile.X, thumb.Y + thumb.Height + px(p, label_gap_dip), tile.Width, px(p, label_h_dip)};
            const auto label_colour = !item.available ? rgba(255, 255, 255, 0.32)
                : selected                            ? rgba(255, 255, 255, 0.98)
                : hovered || focused                  ? rgba(255, 255, 255, 0.9)
                                                      : rgba(255, 255, 255, 0.6);
            draw_text(g, item.name, label_font, label_colour, label_box);
        }
    }

    present(p, pixels);
}

int hit_test(const Picker& p, POINT pt)
{
    const auto fx = static_cast<float>(pt.x);
    const auto fy = static_cast<float>(pt.y);
    for (size_t i = 0; i < p.tiles.size(); ++i) {
        const auto& t = p.tiles[i];
        if (fx >= t.X && fx < t.X + t.Width && fy >= t.Y && fy < t.Y + t.Height)
            return static_cast<int>(i);
    }
    return -1;
}

void choose(Picker& p, int index)
{
    if (index < 0 || index >= static_cast<int>(p.items.size()) || !p.items[index].available)
        return;
    const auto mode = p.items[index].mode;
    auto callback = std::move(p.on_choose);
    // 先關卡片再切換（切換時的交叉淡化會截展開頁，卡片不能還在）
    DestroyWindow(p.wnd);
    if (callback)
        callback(mode);
}

// 鍵盤：在能選的格子之間移動
void move_focus(Picker& p, int step_x, int step_y)
{
    const auto count = static_cast<int>(p.items.size());
    if (count == 0)
        return;
    auto index = p.focus < 0 ? 0 : p.focus;
    const auto step = step_y != 0 ? step_y * p.columns : step_x;
    for (int tries = 0; tries < count; ++tries) {
        auto next = index + step;
        if (step_y != 0) {
            if (next < 0 || next >= count)
                break; // 上下到底就停
        } else {
            next = (next + count) % count;
        }
        index = next;
        if (p.items[index].available) {
            p.focus = index;
            break;
        }
    }
    p.keyboard = true;
    render(p);
}

void update_appear(Picker& p)
{
    const auto elapsed = GetTickCount64() - p.appear_start;
    const auto t = std::clamp(static_cast<float>(elapsed) / appear_ms, 0.f, 1.f);
    const auto eased = 1.f - (1.f - t) * (1.f - t) * (1.f - t);
    p.alpha = static_cast<BYTE>(std::lround(255.f * eased));
    POINT position{p.origin.x, p.origin.y - static_cast<LONG>(std::lround(px(p, appear_slide_dip) * (1.f - eased)))};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, p.alpha, AC_SRC_ALPHA};
    UpdateLayeredWindow(p.wnd, nullptr, &position, nullptr, nullptr, nullptr, 0, &blend, ULW_ALPHA);
    if (t >= 1.f)
        KillTimer(p.wnd, appear_timer_id);
}

// 淡出：只改整張的透明度，不重畫
void begin_fade(Picker& p)
{
    if (p.closing)
        return;
    if (!nocturne_segment::animations_enabled()) {
        DestroyWindow(p.wnd);
        return;
    }
    p.closing = true;
    p.fade_start = GetTickCount64();
    p.fade_from = p.alpha;
    KillTimer(p.wnd, appear_timer_id);
    SetTimer(p.wnd, fade_timer_id, 15, nullptr);
}

void update_fade(Picker& p)
{
    const auto elapsed = static_cast<float>(GetTickCount64() - p.fade_start);
    const auto t = std::clamp(elapsed / (fade_ms * nocturne_segment::slow_factor()), 0.f, 1.f);
    if (t >= 1.f) {
        DestroyWindow(p.wnd);
        return;
    }
    p.alpha = static_cast<BYTE>(std::lround(p.fade_from * (1.f - t)));
    BLENDFUNCTION blend{AC_SRC_OVER, 0, p.alpha, AC_SRC_ALPHA};
    UpdateLayeredWindow(p.wnd, nullptr, nullptr, nullptr, nullptr, nullptr, 0, &blend, ULW_ALPHA);
}

LRESULT CALLBACK window_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    // 點外面、切走程式、失去啟用、展開頁收合時關（見 popup_dismiss.h）
    if (popup_dismiss::handle_message(wnd, msg, wp, lp))
        return 0;

    auto& picker = current_picker();
    if (!picker || picker->wnd != wnd)
        return DefWindowProc(wnd, msg, wp, lp);
    auto& p = *picker;

    if (p.closing && (msg == WM_MOUSEMOVE || msg == WM_LBUTTONUP || msg == WM_KEYDOWN))
        return 0;

    switch (msg) {
    case WM_MOUSEMOVE: {
        if (!p.tracking) {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, wnd, 0};
            p.tracking = TrackMouseEvent(&track) != FALSE;
        }
        auto hit = hit_test(p, {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        if (hit >= 0 && !p.items[hit].available)
            hit = -1;
        if (hit != p.hover || p.keyboard) {
            p.hover = hit;
            p.keyboard = false;
            render(p);
        }
        SetCursor(LoadCursor(nullptr, hit >= 0 ? IDC_HAND : IDC_ARROW));
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
    case WM_LBUTTONUP:
        choose(p, hit_test(p, {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}));
        return 0;
    case WM_KEYDOWN:
        switch (wp) {
        case VK_ESCAPE:
            begin_fade(p);
            return 0;
        case VK_LEFT:
            move_focus(p, -1, 0);
            return 0;
        case VK_RIGHT:
        case VK_TAB:
            move_focus(p, 1, 0);
            return 0;
        case VK_UP:
            move_focus(p, 0, -1);
            return 0;
        case VK_DOWN:
            move_focus(p, 0, 1);
            return 0;
        case VK_HOME:
            p.focus = -1;
            move_focus(p, 1, 0);
            return 0;
        case VK_END:
            p.focus = static_cast<int>(p.items.size());
            move_focus(p, -1, 0);
            return 0;
        case VK_RETURN:
        case VK_SPACE:
            choose(p, p.focus);
            return 0;
        }
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS;
    case WM_TIMER:
        if (wp == appear_timer_id)
            update_appear(p);
        else if (wp == fade_timer_id)
            update_fade(p);
        return 0;
    case WM_CLOSE:
        begin_fade(p);
        return 0;
    case WM_NCDESTROY:
        KillTimer(wnd, appear_timer_id);
        KillTimer(wnd, fade_timer_id);
        picker.reset();
        return DefWindowProc(wnd, msg, wp, lp);
    }
    return DefWindowProc(wnd, msg, wp, lp);
}

} // namespace

void close()
{
    if (auto& picker = current_picker(); picker && picker->wnd)
        DestroyWindow(picker->wnd);
}

bool open(std::vector<Item> items, main_menu::NowPlayingMode current,
    std::function<void(main_menu::NowPlayingMode)> on_choose)
{
    // 開著時再叫一次＝關掉；剛被點外面關掉（點的就是左上角按鈕）也一樣，不要關了又開
    if (current_picker()) {
        close();
        return true;
    }
    if (GetTickCount64() - last_outside_close() < reopen_guard_ms)
        return true;
    if (items.empty() || !next_track_toast::gdiplus_fonts_ready())
        return false;

    static const auto class_registered = [] {
        WNDCLASSEX window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = window_proc;
        window_class.hInstance = core_api::get_my_instance();
        window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
        window_class.lpszClassName = class_name;
        return RegisterClassEx(&window_class) != 0;
    }();
    if (!class_registered)
        return false;

    nocturne_segment::refresh_settings();
    auto p = std::make_unique<Picker>();
    p->items = std::move(items);
    p->current = current;
    p->on_choose = std::move(on_choose);
    p->k = static_cast<float>(uih::scale_dpi_value(1000)) / 1000.f;
    build_layout(*p);
    for (size_t i = 0; i < p->items.size(); ++i)
        if (p->items[i].mode == current)
            p->focus = static_cast<int>(i);

    // 位置：在展開頁就貼著左上角按鈕下面，不然在游標位置
    RECT stage{};
    HWND owner{};
    p->on_stage = panels::splitter::FlatSplitterPanel::s_get_now_playing_stage(stage, owner);
    POINT anchor{};
    if (p->on_stage) {
        anchor = {stage.left + static_cast<LONG>(px(*p, anchor_x_dip)), stage.top + static_cast<LONG>(px(*p, anchor_y_dip))};
    } else {
        GetCursorPos(&anchor);
        owner = core_api::get_main_window();
    }
    // 跟其他右鍵卡同一套位置規則：限制在 fb2k 視窗內，下面放不下就往上翻（2026-11-14）
    p->origin = nocturne_segment::place_card(owner, anchor, anchor.y, p->card);

    build_backdrop(*p);

    // 封面縮圖：縮成縮圖裡最大那張（45／160 的縮圖寬）
    const auto cover_size = static_cast<int>(std::ceil(px(*p, thumb_w_dip) * 45.f / design_w)) + 1;
    const auto& cover = load_cover(cover_size);
    if (cover.valid) {
        p->cover_pixels = cover.pixels;
        p->cover_size = cover.size;
        p->tint = cover.tint;
    }

    current_picker() = std::move(p);
    auto& picker = *current_picker();
    const auto wnd = CreateWindowEx(WS_EX_LAYERED | WS_EX_TOOLWINDOW, class_name, L"", WS_POPUP, picker.origin.x,
        picker.origin.y - static_cast<LONG>(px(picker, appear_slide_dip)), picker.width, picker.height, owner, nullptr,
        core_api::get_my_instance(), nullptr);
    if (!wnd) {
        current_picker().reset();
        return false;
    }
    picker.wnd = wnd;
    picker.alpha = 0;
    // 自動關閉：在展開頁開的，展開頁收合／換版面時也關；點外面關掉時記下時間（緊接著的左上角按鈕指令當成「關掉」）
    popup_dismiss::attach(
        wnd,
        [on_stage = picker.on_stage] {
            RECT stage{};
            HWND stage_owner{};
            return !on_stage || panels::splitter::FlatSplitterPanel::s_get_now_playing_stage(stage, stage_owner);
        },
        &last_outside_close());
    render(picker);
    ShowWindow(wnd, SW_SHOW);
    SetForegroundWindow(wnd);
    SetFocus(wnd);
    picker.appear_start = GetTickCount64();
    SetTimer(wnd, appear_timer_id, 15, nullptr);
    return true;
}

} // namespace cui::mode_picker
