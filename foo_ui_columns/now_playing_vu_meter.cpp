#include "pch.h"

#include <cmath>
#include <unordered_map>

#include "now_playing_vu_meter.h"
#include "now_playing_cover_zoom.h"
#include "menu_items.h"
#include "next_track_toast.h"
#include "splitter.h"

// 展開頁自繪 VU 表，說明見 now_playing_vu_meter.h

namespace cui::vu_meter {

namespace {

constexpr wchar_t window_class_name[] = L"columns_ui_vu_meter";

constexpr UINT_PTR frame_timer_id = 1;
constexpr UINT frame_interval_ms = 15;
// 換歌後背景（封面虛化）會換，過一下再重截兩次：ESLyric 換圖要一點時間
constexpr UINT_PTR recapture_timer_id = 2;
constexpr UINT_PTR recapture_late_timer_id = 3;
constexpr UINT recapture_delay_ms = 600;
constexpr UINT recapture_late_delay_ms = 1800;

// 出現時淡入（同放大鈕）
constexpr ULONGLONG fade_in_ms = 180;

// ---- 校準與指針動態 ----
// 平均音量（RMS）−7 dBFS＝0 VU（使用者 2026-11-11 先用 C 組：外掛 VU 增益 +7）
constexpr double reference_dbfs = -7.0;
// 刻度最右邊是 +3 VU
constexpr double scale_top_vu = 3.0;
// 峰值燈：瞬間峰值到 −1 dBFS 就亮
constexpr double peak_lamp_dbfs = -1.0;
constexpr double lamp_hold_seconds = 0.15;
constexpr double lamp_fade_seconds = 0.4;
// 二階系統：99% 到位約 300 毫秒、過衝約 1.5%（阻尼比 0.8）
constexpr double damping_ratio = 0.8;
constexpr double natural_frequency = 4.6 / (damping_ratio * 0.3);

// ---- 外觀（以錶框高 157 為基準的像素，實際乘上 scale）----
constexpr float base_height = 157.f;
constexpr float gap_px = 10.f;          // 左右兩個錶框的間距
constexpr float radius_px = 16.f;       // 錶框圓角
constexpr float bezel_px = 10.f;        // 玻璃邊緣折射的寬度
constexpr float refraction_px = 8.f;    // 邊緣最大位移
constexpr float bezel_curvature = 2.2f; // 厚度曲線 (1−(1−t)^k)^(1/k)
constexpr float rim_strength = 0.70f;
constexpr float shadow_alpha = 0.42f;
constexpr float shadow_offset_px = 10.f;
constexpr float shadow_blur_px = 9.f;   // 三次方塊模糊的半徑
constexpr double half_span = 42.0 * 3.14159265358979323846 / 180.0; // 刻度半邊角度

// 視窗比 VU 位置多留的邊（給陰影）
constexpr float margin_x_px = 20.f;
constexpr float margin_top_px = 14.f;
constexpr float margin_bottom_px = 30.f;

constexpr double pi = 3.14159265358979323846;

double pos_of_db(double db)
{
    return std::pow(10.0, db / 20.0) / std::pow(10.0, scale_top_vu / 20.0);
}

double angle_of_pos(double pos)
{
    return -half_span + 2.0 * half_span * std::clamp(pos, -0.02, 1.04);
}

struct Needle {
    double pos{};
    double velocity{};
    double lamp{};
    double lamp_hold{};
};

struct State {
    RECT bar_rect{};
    RECT window_rect{}; // 分割面板座標
    int width{};
    int height{};
    float scale{1.f};
    Gdiplus::RectF panels[2];

    // 截到的背景（視窗大小，BGRA）
    std::vector<uint32_t> backdrop;
    bool has_backdrop{};
    // 不動的那一層：背景（錶框內已調飽和、壓暗）＋背光＋刻度與字
    std::vector<uint32_t> base;
    // 每格畫面：base＋指針、峰值燈、軸蓋
    std::vector<uint32_t> content;
    bool static_dirty{true};

    // 錶框外的陰影（0～1）
    std::vector<float> shadow;
    // 錶框裡的位移、覆蓋率、反光（兩個錶框一樣大，共用一份）
    int panel_w{};
    int panel_h{};
    std::vector<float> dx;
    std::vector<float> dy;
    std::vector<float> coverage;
    std::vector<float> specular;

    Needle needles[2];
    visualisation_stream::ptr stream;
    ULONGLONG last_tick{};
    ULONGLONG shown_at{};
    bool fading{};
    bool needs_present{true};

    wil::unique_hdc memory_dc;
    wil::unique_hbitmap dib;
    uint32_t* dib_bits{};
    int dib_w{};
    int dib_h{};
};

std::unordered_map<HWND, std::unique_ptr<State>>& states()
{
    static std::unordered_map<HWND, std::unique_ptr<State>> map;
    return map;
}

State* state_of(HWND wnd)
{
    const auto it = states().find(wnd);
    return it == states().end() ? nullptr : it->second.get();
}

Gdiplus::Color rgba(int r, int g, int b, double a)
{
    return Gdiplus::Color(static_cast<BYTE>(std::clamp(a, 0.0, 1.0) * 255.0 + 0.5), static_cast<BYTE>(r),
        static_cast<BYTE>(g), static_cast<BYTE>(b));
}

// 主題字型（同下一首提示）。GDI+ 字型表沒暖機完時傳回空的，先不畫字
std::unique_ptr<Gdiplus::FontFamily> theme_family()
{
    if (!next_track_toast::gdiplus_fonts_ready())
        return {};
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

Gdiplus::FontStyle bold_style(const Gdiplus::FontFamily& family)
{
    return family.IsStyleAvailable(Gdiplus::FontStyleBold) ? Gdiplus::FontStyleBold : regular_style(family);
}

void add_round_rect(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& rect, float radius)
{
    const auto d = radius * 2.f;
    path.AddArc(rect.X, rect.Y, d, d, 180.f, 90.f);
    path.AddArc(rect.X + rect.Width - d, rect.Y, d, d, 270.f, 90.f);
    path.AddArc(rect.X + rect.Width - d, rect.Y + rect.Height - d, d, d, 0.f, 90.f);
    path.AddArc(rect.X, rect.Y + rect.Height - d, d, d, 90.f, 90.f);
    path.CloseFigure();
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

// ---- 版面 ----

struct Geometry {
    float cx;
    float cy;
    float radius;
};

Geometry meter_geometry(const State& state, const Gdiplus::RectF& panel)
{
    const auto s = state.scale;
    const auto radius = panel.Width * 0.53f;
    return {panel.X + panel.Width / 2.f, panel.Y + 45.f * s + radius, radius};
}

Gdiplus::PointF point_at(const Geometry& m, double angle, float r)
{
    return {m.cx + r * static_cast<float>(std::sin(angle)), m.cy - r * static_cast<float>(std::cos(angle))};
}

RECT compute_window_rect(const RECT& bar_rect)
{
    if (bar_rect.right <= bar_rect.left)
        return {};

    RECT stage_screen{};
    HWND owner{};
    if (!panels::splitter::FlatSplitterPanel::s_get_now_playing_stage(stage_screen, owner))
        return {};
    if (main_menu::get_now_playing_mode() != main_menu::NowPlayingMode::vu)
        return {};

    const auto vu = cover_zoom::vu_rect_in_stage(bar_rect.right - bar_rect.left, bar_rect.top);
    if (!vu)
        return {};

    const auto s = static_cast<float>(vu->bottom - vu->top) / base_height;
    RECT rect{bar_rect.left + vu->left - static_cast<LONG>(margin_x_px * s),
        vu->top - static_cast<LONG>(margin_top_px * s), bar_rect.left + vu->right + static_cast<LONG>(margin_x_px * s),
        vu->bottom + static_cast<LONG>(margin_bottom_px * s)};
    rect.top = std::max(rect.top, 0L);
    rect.bottom = std::min(rect.bottom, bar_rect.top);
    return rect;
}

// 視窗大小變了：重算錶框位置、陰影、位移與反光
void rebuild_geometry(State& state, int width, int height, float scale)
{
    state.width = width;
    state.height = height;
    state.scale = scale;
    const auto s = scale;
    const auto mx = margin_x_px * s;
    const auto mt = margin_top_px * s;
    const auto vu_w = width - 2.f * mx;
    const auto panel_h = std::round(base_height * s);
    const auto panel_w = std::floor((vu_w - gap_px * s) / 2.f);
    state.panels[0] = {std::round(mx), std::round(mt), panel_w, panel_h};
    state.panels[1] = {std::round(mx + panel_w + gap_px * s), std::round(mt), panel_w, panel_h};

    const auto n = static_cast<size_t>(width) * height;
    state.backdrop.assign(n, 0xFF141618);
    state.has_backdrop = false;
    state.base.assign(n, 0);
    state.content.assign(n, 0);
    state.static_dirty = true;

    // 陰影：錶框形狀（內縮 2）往下移，三次方塊模糊
    std::vector<float> mask(n, 0.f);
    const auto radius = radius_px * s;
    for (const auto& panel : state.panels) {
        const auto hw = panel.Width / 2.f - 2.f * s;
        const auto hh = panel.Height / 2.f - 2.f * s;
        const auto cx = panel.X + panel.Width / 2.f;
        const auto cy = panel.Y + panel.Height / 2.f + shadow_offset_px * s;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                const auto d = rounded_rect_sdf(x + 0.5f - cx, y + 0.5f - cy, hw, hh, radius);
                if (d < 0.5f)
                    mask[static_cast<size_t>(y) * width + x] = std::max(mask[static_cast<size_t>(y) * width + x],
                        std::clamp(0.5f - d, 0.f, 1.f));
            }
    }
    const auto blur_r = std::max(1, static_cast<int>(std::lround(shadow_blur_px * s)));
    std::vector<float> temp(n);
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < height; ++y) {
            float sum = 0;
            const auto row = static_cast<size_t>(y) * width;
            for (int x = -blur_r; x <= blur_r; ++x)
                sum += mask[row + std::clamp(x, 0, width - 1)];
            for (int x = 0; x < width; ++x) {
                temp[row + x] = sum / (2 * blur_r + 1);
                sum += mask[row + std::clamp(x + blur_r + 1, 0, width - 1)] - mask[row + std::clamp(x - blur_r, 0, width - 1)];
            }
        }
        for (int x = 0; x < width; ++x) {
            float sum = 0;
            for (int y = -blur_r; y <= blur_r; ++y)
                sum += temp[static_cast<size_t>(std::clamp(y, 0, height - 1)) * width + x];
            for (int y = 0; y < height; ++y) {
                mask[static_cast<size_t>(y) * width + x] = sum / (2 * blur_r + 1);
                sum += temp[static_cast<size_t>(std::clamp(y + blur_r + 1, 0, height - 1)) * width + x]
                    - temp[static_cast<size_t>(std::clamp(y - blur_r, 0, height - 1)) * width + x];
            }
        }
    }
    for (auto& value : mask)
        value *= shadow_alpha;
    state.shadow = std::move(mask);

    // 玻璃：邊緣往內取樣（放大），反光照邊緣朝向（光從左上來）
    const auto pw = static_cast<int>(panel_w);
    const auto ph = static_cast<int>(panel_h);
    state.panel_w = pw;
    state.panel_h = ph;
    const auto pn = static_cast<size_t>(pw) * ph;
    state.dx.assign(pn, 0.f);
    state.dy.assign(pn, 0.f);
    state.coverage.assign(pn, 0.f);
    state.specular.assign(pn, 0.f);
    const auto hw = pw / 2.f;
    const auto hh = ph / 2.f;
    const auto bezel = bezel_px * s;
    const auto refraction = refraction_px * s;
    constexpr float light_x = -0.7071f;
    constexpr float light_y = -0.7071f;
    for (int y = 0; y < ph; ++y) {
        for (int x = 0; x < pw; ++x) {
            const auto i = static_cast<size_t>(y) * pw + x;
            const auto px = x + 0.5f - hw;
            const auto py = y + 0.5f - hh;
            const auto d = rounded_rect_sdf(px, py, hw, hh, radius);
            const auto c = std::clamp(0.5f - d, 0.f, 1.f);
            state.coverage[i] = c;
            if (c <= 0.f)
                continue;

            // 上半部很淡的光澤
            float sheen = y < ph / 2 ? 0.085f * (1.f - y / (ph / 2.f)) : 0.f;
            float rim = 0.f;
            const auto inside = -d;
            const auto t = std::clamp(inside / bezel, 0.f, 1.f);
            if (t < 1.f) {
                auto gx = rounded_rect_sdf(px + 1.f, py, hw, hh, radius) - rounded_rect_sdf(px - 1.f, py, hw, hh, radius);
                auto gy = rounded_rect_sdf(px, py + 1.f, hw, hh, radius) - rounded_rect_sdf(px, py - 1.f, hw, hh, radius);
                const auto length = std::sqrt(gx * gx + gy * gy);
                if (length > 0.f) {
                    gx /= length;
                    gy /= length;
                }
                const auto profile = std::pow(1.f - std::pow(1.f - t, bezel_curvature), 1.f / bezel_curvature);
                const auto magnitude = refraction * (1.f - profile);
                state.dx[i] = -gx * magnitude;
                state.dy[i] = -gy * magnitude;

                const auto facing = gx * light_x + gy * light_y;
                const auto edge = std::pow(1.f - t, 2.4f);
                rim = rim_strength * edge
                    * (std::pow(std::max(0.f, facing), 1.6f) * 0.95f + std::pow(std::max(0.f, -facing), 2.f) * 0.38f);
                if (inside < 1.6f * s)
                    rim += 0.28f * (0.55f + 0.45f * facing);
                rim += 0.06f * (1.f - t);
            }
            const auto a = 1.f - (1.f - std::clamp(rim, 0.f, 0.95f)) * (1.f - sheen);
            state.specular[i] = a;
        }
    }
}

// 取錶框後面的虛化背景
void capture_backdrop(HWND wnd, State& state)
{
    RECT stage_screen{};
    HWND owner{};
    const auto bitmap = panels::splitter::FlatSplitterPanel::s_capture_now_playing_stage(stage_screen, owner);
    if (!bitmap)
        return;

    RECT screen = state.window_rect;
    MapWindowPoints(GetParent(wnd), HWND_DESKTOP, reinterpret_cast<POINT*>(&screen), 2);

    BITMAP info{};
    GetObject(bitmap.get(), sizeof(info), &info);
    const auto stage_w = static_cast<int>(info.bmWidth);
    const auto stage_h = static_cast<int>(info.bmHeight);
    if (stage_w <= 0 || stage_h <= 0)
        return;

    std::vector<uint32_t> pixels(static_cast<size_t>(stage_w) * stage_h);
    BITMAPINFO header{};
    header.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    header.bmiHeader.biWidth = stage_w;
    header.bmiHeader.biHeight = -stage_h;
    header.bmiHeader.biPlanes = 1;
    header.bmiHeader.biBitCount = 32;
    header.bmiHeader.biCompression = BI_RGB;
    const auto screen_dc = wil::GetDC(nullptr);
    if (!GetDIBits(screen_dc.get(), bitmap.get(), 0, stage_h, pixels.data(), &header, DIB_RGB_COLORS))
        return;

    // 截圖一定會連這個視窗自己（還有截圖當下剛好還在的歌詞）一起截進去（PrintWindow 主視窗或 ESLyric 視窗都一樣，2026-11-11 實測），
    // 所以不直接用視窗裡面那塊：只取視窗外圍一圈（往外 3～10 像素）的背景色，再用 Coons 曲面從四邊往內補滿。
    // 封面虛化背景很平滑，補出來的跟真的幾乎一樣，而且不用把自己藏起來再截（換歌時不會閃）
    const auto ox = static_cast<int>(screen.left - stage_screen.left);
    const auto oy = static_cast<int>(screen.top - stage_screen.top);
    const auto w = state.width;
    const auto h = state.height;
    struct Rgb {
        float r{}, g{}, b{};
    };
    const auto at = [&](int x, int y) {
        const auto p = pixels[static_cast<size_t>(std::clamp(y, 0, stage_h - 1)) * stage_w + std::clamp(x, 0, stage_w - 1)];
        return Rgb{static_cast<float>((p >> 16) & 0xFF), static_cast<float>((p >> 8) & 0xFF), static_cast<float>(p & 0xFF)};
    };
    constexpr int band_near = 3;
    constexpr int band_far = 10;
    const auto band = [&](int x0, int y0, int step_x, int step_y) {
        Rgb sum{};
        for (int d = band_near; d <= band_far; ++d) {
            const auto c = at(x0 + step_x * d, y0 + step_y * d);
            sum.r += c.r;
            sum.g += c.g;
            sum.b += c.b;
        }
        const auto n = static_cast<float>(band_far - band_near + 1);
        return Rgb{sum.r / n, sum.g / n, sum.b / n};
    };
    std::vector<Rgb> top(w), bottom(w), left(h), right(h);
    for (int x = 0; x < w; ++x) {
        top[x] = band(ox + x, oy, 0, -1);
        bottom[x] = band(ox + x, oy + h - 1, 0, 1);
    }
    for (int y = 0; y < h; ++y) {
        left[y] = band(ox, oy + y, -1, 0);
        right[y] = band(ox + w - 1, oy + y, 1, 0);
    }
    // 四邊再各自抹平（背景若有雜點或字的邊緣，不要拉成一條線）
    const auto smooth = [](std::vector<Rgb>& edge) {
        const auto n = static_cast<int>(edge.size());
        constexpr int r = 12;
        for (int pass = 0; pass < 2; ++pass) {
            std::vector<Rgb> out(edge.size());
            for (int i = 0; i < n; ++i) {
                Rgb sum{};
                int count = 0;
                for (int k = std::max(0, i - r); k <= std::min(n - 1, i + r); ++k, ++count) {
                    sum.r += edge[k].r;
                    sum.g += edge[k].g;
                    sum.b += edge[k].b;
                }
                out[i] = {sum.r / count, sum.g / count, sum.b / count};
            }
            edge.swap(out);
        }
    };
    smooth(top);
    smooth(bottom);
    smooth(left);
    smooth(right);
    const auto c00 = top.front();
    const auto c10 = top.back();
    const auto c01 = bottom.front();
    const auto c11 = bottom.back();
    for (int y = 0; y < h; ++y) {
        const auto v = h > 1 ? static_cast<float>(y) / (h - 1) : 0.f;
        for (int x = 0; x < w; ++x) {
            const auto u = w > 1 ? static_cast<float>(x) / (w - 1) : 0.f;
            const auto coons = [&](float Rgb::*channel) {
                const auto value = (1 - v) * top[x].*channel + v * bottom[x].*channel + (1 - u) * left[y].*channel
                    + u * right[y].*channel
                    - ((1 - u) * (1 - v) * c00.*channel + u * (1 - v) * c10.*channel + (1 - u) * v * c01.*channel
                        + u * v * c11.*channel);
                return static_cast<uint32_t>(std::clamp(value, 0.f, 255.f) + 0.5f);
            };
            state.backdrop[static_cast<size_t>(y) * w + x]
                = 0xFF000000 | (coons(&Rgb::r) << 16) | (coons(&Rgb::g) << 8) | coons(&Rgb::b);
        }
    }
    state.has_backdrop = true;
    state.static_dirty = true;
}

// ---- 錶面（不動的部分）----

void draw_scale(Gdiplus::Graphics& g, const State& state, const Gdiplus::RectF& panel, int index,
    const Gdiplus::FontFamily* family)
{
    const auto s = state.scale;
    const auto m = meter_geometry(state, panel);
    const Gdiplus::Color ink(214, 244, 240, 230);
    const Gdiplus::Color ink_soft(107, 244, 240, 230);
    const Gdiplus::Color red(242, 240, 98, 74);

    const auto arc = [&](const Gdiplus::Pen& pen, double from_pos, double to_pos, float r) {
        const auto a1 = angle_of_pos(from_pos);
        const auto a2 = angle_of_pos(to_pos);
        g.DrawArc(&pen, m.cx - r, m.cy - r, 2 * r, 2 * r, static_cast<float>(-90.0 + a1 * 180.0 / pi),
            static_cast<float>((a2 - a1) * 180.0 / pi));
    };

    Gdiplus::Pen main_pen(ink, 1.5f * s);
    arc(main_pen, pos_of_db(-20), pos_of_db(0), m.radius);
    Gdiplus::Pen red_pen(Gdiplus::Color(224, 240, 98, 74), 4.5f * s);
    arc(red_pen, pos_of_db(0), pos_of_db(3), m.radius + 2.6f * s);

    const auto tick = [&](double db, float length, const Gdiplus::Color& colour, float width) {
        const auto a = angle_of_pos(pos_of_db(db));
        const auto p1 = point_at(m, a, m.radius);
        const auto p2 = point_at(m, a, m.radius + length);
        Gdiplus::Pen pen(colour, width);
        g.DrawLine(&pen, p1, p2);
    };

    const double marks[] = {-20, -10, -7, -5, -3, -2, -1, 0, 1, 2, 3};
    for (double db = -20; db <= 3.0001; db += 0.5) {
        if (std::find(std::begin(marks), std::end(marks), db) != std::end(marks))
            continue;
        if (db < -10 && std::fmod(-db, 2.0) != 0)
            continue;
        tick(db, 3.5f * s, db > 0 ? Gdiplus::Color(166, 240, 98, 74) : ink_soft, 1.f * s);
    }
    for (const auto db : marks) {
        const auto big = std::fmod(-db, 5.0) == 0 || db == -7 || db == -3 || db == 3;
        tick(db, (big ? 9.f : 6.f) * s, db > 0 ? red : ink, 1.2f * s);
    }

    if (!family)
        return;

    Gdiplus::StringFormat centre;
    centre.SetAlignment(Gdiplus::StringAlignmentCenter);
    centre.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    const auto draw_text = [&](const wchar_t* text, const Gdiplus::Font& font, const Gdiplus::Color& colour,
                               Gdiplus::PointF at, Gdiplus::StringAlignment align) {
        Gdiplus::StringFormat format;
        format.SetAlignment(align);
        format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
        const auto box_w = 200.f * s;
        const auto box_h = 60.f * s;
        auto left = at.X - box_w / 2.f;
        if (align == Gdiplus::StringAlignmentNear)
            left = at.X;
        else if (align == Gdiplus::StringAlignmentFar)
            left = at.X - box_w;
        const Gdiplus::SolidBrush brush(colour);
        g.DrawString(text, -1, &font, Gdiplus::RectF(left, at.Y - box_h / 2.f, box_w, box_h), &format, &brush);
    };

    const auto style = regular_style(*family);
    const Gdiplus::Font number_font(family, 14.f * s, style, Gdiplus::UnitPixel);
    for (const auto db : marks) {
        const auto a = angle_of_pos(pos_of_db(db));
        const auto at = point_at(m, a, m.radius + 18.f * s);
        const auto text = std::to_wstring(static_cast<int>(std::abs(db)));
        draw_text(text.c_str(), number_font, db > 0 ? red : ink, at, Gdiplus::StringAlignmentCenter);
    }

    // 刻度兩端的正負號（紅區數字不寫 +）
    const Gdiplus::Font sign_font(family, 15.f * s, bold_style(*family), Gdiplus::UnitPixel);
    draw_text(L"−", sign_font, Gdiplus::Color(178, 244, 240, 230),
        point_at(m, angle_of_pos(pos_of_db(-20)) - 0.10, m.radius + 4.f * s), Gdiplus::StringAlignmentCenter);
    draw_text(L"+", sign_font, red, point_at(m, angle_of_pos(pos_of_db(3)) + 0.09, m.radius + 4.f * s),
        Gdiplus::StringAlignmentCenter);

    const Gdiplus::Font vu_font(family, 24.f * s, bold_style(*family), Gdiplus::UnitPixel);
    draw_text(L"VU", vu_font, Gdiplus::Color(204, 244, 240, 230), {m.cx, panel.Y + panel.Height * 0.64f},
        Gdiplus::StringAlignmentCenter);

    const Gdiplus::Font small_font(family, 13.f * s, bold_style(*family), Gdiplus::UnitPixel);
    draw_text(index == 0 ? L"L" : L"R", small_font, Gdiplus::Color(158, 244, 240, 230),
        {panel.X + 18.f * s, panel.Y + panel.Height - 18.f * s}, Gdiplus::StringAlignmentNear);

    wchar_t reference[64]{};
    swprintf_s(reference, L"0 VU = −%d dBFS", static_cast<int>(std::lround(-reference_dbfs)));
    const Gdiplus::Font tiny_font(family, 11.f * s, style, Gdiplus::UnitPixel);
    draw_text(reference, tiny_font, Gdiplus::Color(122, 244, 240, 230),
        {panel.X + panel.Width - 16.f * s, panel.Y + panel.Height - 18.f * s}, Gdiplus::StringAlignmentFar);

    const Gdiplus::Font lamp_font(family, 9.5f * s, style, Gdiplus::UnitPixel);
    draw_text(L"PEAK", lamp_font, Gdiplus::Color(122, 244, 240, 230),
        {panel.X + panel.Width - 30.f * s, panel.Y + 22.f * s}, Gdiplus::StringAlignmentFar);
}

void rebuild_static(State& state)
{
    const auto w = state.width;
    const auto h = state.height;
    state.base = state.backdrop;

    // 錶框裡：背景調飽和、依亮度壓暗
    for (const auto& panel : state.panels) {
        const auto x0 = static_cast<int>(panel.X);
        const auto y0 = static_cast<int>(panel.Y);
        const auto x1 = std::min(w, static_cast<int>(panel.X + panel.Width));
        const auto y1 = std::min(h, static_cast<int>(panel.Y + panel.Height));

        double sum = 0;
        int count = 0;
        for (int y = y0; y < y1; y += 4)
            for (int x = x0; x < x1; x += 4) {
                const auto p = state.backdrop[static_cast<size_t>(y) * w + x];
                sum += (0.2126 * ((p >> 16) & 0xFF) + 0.7152 * ((p >> 8) & 0xFF) + 0.0722 * (p & 0xFF)) / 255.0;
                ++count;
            }
        const auto lum = count ? sum / count : 0.1;
        const auto tint = static_cast<float>(std::clamp(0.30 + (lum - 0.12) * 1.1, 0.30, 0.62));

        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                auto& p = state.base[static_cast<size_t>(y) * w + x];
                const auto r = static_cast<float>((p >> 16) & 0xFF);
                const auto g = static_cast<float>((p >> 8) & 0xFF);
                const auto b = static_cast<float>(p & 0xFF);
                const auto luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                const auto sat = [&](float c) { return std::clamp(luma + (c - luma) * 1.35f, 0.f, 255.f); };
                const auto mix = [&](float c, float target) { return c * (1.f - tint) + target * tint; };
                const auto nr = static_cast<uint32_t>(mix(sat(r), 10.f) + 0.5f);
                const auto ng = static_cast<uint32_t>(mix(sat(g), 11.f) + 0.5f);
                const auto nb = static_cast<uint32_t>(mix(sat(b), 14.f) + 0.5f);
                p = 0xFF000000 | (nr << 16) | (ng << 8) | nb;
            }
    }

    Gdiplus::Bitmap canvas(w, h, w * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(state.base.data()));
    Gdiplus::Graphics g(&canvas);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    const auto family = theme_family();
    const auto s = state.scale;

    for (int index = 0; index < 2; ++index) {
        const auto& panel = state.panels[index];
        Gdiplus::GraphicsPath clip;
        add_round_rect(clip, panel, radius_px * s);
        g.SetClip(&clip);

        if (is_backlight_enabled()) {
            // 暖色背光：從下緣中間往上散開
            const auto cx = panel.X + panel.Width / 2.f;
            const auto cy = panel.Y + panel.Height + 8.f * s;
            const auto r = panel.Width * 0.62f;
            Gdiplus::GraphicsPath glow_path;
            glow_path.AddEllipse(cx - r, cy - r, 2 * r, 2 * r);
            Gdiplus::PathGradientBrush glow(&glow_path);
            glow.SetCenterPoint(Gdiplus::PointF(cx, cy));
            Gdiplus::Color colours[] = {rgba(255, 190, 120, 0), rgba(255, 190, 120, 0.07), rgba(255, 190, 120, 0.20)};
            Gdiplus::REAL positions[] = {0.f, 0.5f, 1.f};
            glow.SetInterpolationColors(colours, positions, 3);
            g.FillRectangle(&glow, panel);
        }

        draw_scale(g, state, panel, index, family.get());

        // 峰值燈（熄著的樣子）
        const auto lx = panel.X + panel.Width - 22.f * s;
        const auto ly = panel.Y + 22.f * s;
        const auto lr = 3.6f * s;
        const Gdiplus::SolidBrush off(rgba(90, 20, 16, 0.85));
        g.FillEllipse(&off, lx - lr, ly - lr, 2 * lr, 2 * lr);
        g.ResetClip();
    }
    state.static_dirty = false;
}

// ---- 每格 ----

void draw_dynamic(State& state)
{
    const auto w = state.width;
    const auto h = state.height;
    state.content = state.base;
    Gdiplus::Bitmap canvas(w, h, w * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(state.content.data()));
    Gdiplus::Graphics g(&canvas);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    const auto s = state.scale;

    for (int index = 0; index < 2; ++index) {
        const auto& panel = state.panels[index];
        const auto& needle = state.needles[index];
        const auto m = meter_geometry(state, panel);
        Gdiplus::GraphicsPath clip;
        add_round_rect(clip, panel, radius_px * s);
        g.SetClip(&clip);

        // 峰值燈亮
        if (needle.lamp > 0.001) {
            const auto v = needle.lamp;
            const auto lx = panel.X + panel.Width - 22.f * s;
            const auto ly = panel.Y + 22.f * s;
            const auto glow_r = 13.f * s;
            Gdiplus::GraphicsPath glow_path;
            glow_path.AddEllipse(lx - glow_r, ly - glow_r, 2 * glow_r, 2 * glow_r);
            Gdiplus::PathGradientBrush glow(&glow_path);
            glow.SetCenterPoint(Gdiplus::PointF(lx, ly));
            glow.SetCenterColor(rgba(255, 70, 50, 0.55 * v));
            Gdiplus::Color surround = rgba(255, 70, 50, 0);
            int surround_count = 1;
            glow.SetSurroundColors(&surround, &surround_count);
            g.FillEllipse(&glow, lx - glow_r, ly - glow_r, 2 * glow_r, 2 * glow_r);
            const auto lr = 3.6f * s;
            const Gdiplus::SolidBrush lit(rgba(255, static_cast<int>(60 + 80 * v), static_cast<int>(40 + 60 * v), 0.35 + 0.65 * v));
            g.FillEllipse(&lit, lx - lr, ly - lr, 2 * lr, 2 * lr);
        }
        {
            const auto lx = panel.X + panel.Width - 22.f * s;
            const auto ly = panel.Y + 22.f * s;
            const Gdiplus::SolidBrush spark(rgba(255, 255, 255, 0.35));
            g.FillEllipse(&spark, lx - 2.1f * s, ly - 2.3f * s, 2.2f * s, 2.2f * s);
        }

        // 指針：上粗下細，浮在錶面上方，影子往右下偏
        const auto a = angle_of_pos(needle.pos);
        const auto base_r = m.cy - (panel.Y + panel.Height) + 2.f * s;
        const auto tip_r = m.radius + 13.f * s;
        const auto nx = static_cast<float>(std::cos(a));
        const auto ny = static_cast<float>(std::sin(a));
        const auto needle_polygon = [&](float ox, float oy, float widen, Gdiplus::PointF* points) {
            const auto b = point_at(m, a, base_r);
            const auto t = point_at(m, a, tip_r);
            const auto bw = (1.3f + widen) * s;
            const auto tw = (0.45f + widen) * s;
            points[0] = {b.X - nx * bw + ox, b.Y - ny * bw + oy};
            points[1] = {t.X - nx * tw + ox, t.Y - ny * tw + oy};
            points[2] = {t.X + nx * tw + ox, t.Y + ny * tw + oy};
            points[3] = {b.X + nx * bw + ox, b.Y + ny * bw + oy};
        };
        Gdiplus::PointF points[4];
        const Gdiplus::SolidBrush shadow(rgba(0, 0, 0, 0.14));
        for (const auto offset : {0.8f, 1.f, 1.2f}) {
            needle_polygon(3.2f * s * offset, 4.2f * s * offset, 0.8f, points);
            g.FillPolygon(&shadow, points, 4);
        }
        needle_polygon(0, 0, 0, points);
        const Gdiplus::SolidBrush needle_brush(rgba(250, 247, 240, 0.97));
        g.FillPolygon(&needle_brush, points, 4);

        // 霧面軸蓋
        const auto cx = m.cx;
        const auto cy = panel.Y + panel.Height;
        const auto rx = 30.f * s;
        const auto ry = 19.f * s;
        Gdiplus::GraphicsPath cap;
        cap.AddArc(cx - rx, cy - ry, 2 * rx, 2 * ry, 180.f, 180.f);
        cap.CloseFigure();
        Gdiplus::LinearGradientBrush cap_brush(Gdiplus::PointF(cx, cy - ry - 1.f), Gdiplus::PointF(cx, cy + 1.f),
            rgba(70, 72, 78, 0.94), rgba(34, 35, 39, 0.96));
        Gdiplus::Color cap_colours[] = {rgba(70, 72, 78, 0.94), rgba(48, 50, 55, 0.95), rgba(34, 35, 39, 0.96)};
        Gdiplus::REAL cap_positions[] = {0.f, 0.45f, 1.f};
        cap_brush.SetInterpolationColors(cap_colours, cap_positions, 3);
        g.FillPath(&cap_brush, &cap);
        Gdiplus::Pen cap_rim(rgba(255, 255, 255, 0.28), 1.f * s);
        g.DrawArc(&cap_rim, cx - rx + 0.5f * s, cy - ry + 0.5f * s, 2 * rx - s, 2 * ry - s, 201.6f, 136.8f);

        g.ResetClip();
    }
}

uint32_t sample_bilinear(const std::vector<uint32_t>& src, int w, int h, float fx, float fy)
{
    fx = std::clamp(fx, 0.f, w - 1.001f);
    fy = std::clamp(fy, 0.f, h - 1.001f);
    const auto x0 = static_cast<int>(fx);
    const auto y0 = static_cast<int>(fy);
    const auto ax = fx - x0;
    const auto ay = fy - y0;
    const auto p00 = src[static_cast<size_t>(y0) * w + x0];
    const auto p10 = src[static_cast<size_t>(y0) * w + x0 + 1];
    const auto p01 = src[static_cast<size_t>(y0 + 1) * w + x0];
    const auto p11 = src[static_cast<size_t>(y0 + 1) * w + x0 + 1];
    uint32_t out = 0xFF000000;
    for (int shift = 0; shift <= 16; shift += 8) {
        const auto c = ((p00 >> shift) & 0xFF) * (1 - ax) * (1 - ay) + ((p10 >> shift) & 0xFF) * ax * (1 - ay)
            + ((p01 >> shift) & 0xFF) * (1 - ax) * ay + ((p11 >> shift) & 0xFF) * ax * ay;
        out |= static_cast<uint32_t>(c + 0.5f) << shift;
    }
    return out;
}

bool ensure_dib(State& state)
{
    if (state.dib && state.dib_w == state.width && state.dib_h == state.height)
        return true;
    state.dib.reset();
    state.memory_dc.reset();
    state.dib_bits = nullptr;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = state.width;
    info.bmiHeader.biHeight = -state.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    const auto screen_dc = wil::GetDC(nullptr);
    void* bits{};
    state.dib.reset(CreateDIBSection(screen_dc.get(), &info, DIB_RGB_COLORS, &bits, nullptr, 0));
    if (!state.dib || !bits)
        return false;
    state.memory_dc.reset(CreateCompatibleDC(screen_dc.get()));
    SelectObject(state.memory_dc.get(), state.dib.get());
    state.dib_bits = static_cast<uint32_t*>(bits);
    state.dib_w = state.width;
    state.dib_h = state.height;
    return true;
}

void present(HWND wnd, State& state)
{
    if (state.width <= 0 || state.height <= 0 || !ensure_dib(state))
        return;
    if (state.static_dirty)
        rebuild_static(state);
    draw_dynamic(state);

    const auto w = state.width;
    const auto h = state.height;
    auto* out = state.dib_bits;
    // 陰影（黑色，預乘）
    for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i)
        out[i] = static_cast<uint32_t>(state.shadow[i] * 255.f + 0.5f) << 24;

    // 玻璃
    for (const auto& panel : state.panels) {
        const auto px0 = static_cast<int>(panel.X);
        const auto py0 = static_cast<int>(panel.Y);
        for (int y = 0; y < state.panel_h; ++y) {
            const auto wy = py0 + y;
            if (wy < 0 || wy >= h)
                continue;
            for (int x = 0; x < state.panel_w; ++x) {
                const auto wx = px0 + x;
                if (wx < 0 || wx >= w)
                    continue;
                const auto i = static_cast<size_t>(y) * state.panel_w + x;
                const auto c = state.coverage[i];
                if (c <= 0.f)
                    continue;
                const auto dx = state.dx[i];
                const auto dy = state.dy[i];
                const auto colour = (dx == 0.f && dy == 0.f)
                    ? state.content[static_cast<size_t>(wy) * w + wx]
                    : sample_bilinear(state.content, w, h, wx + dx, wy + dy);
                const auto a = state.specular[i];
                const auto light = [&](uint32_t channel) { return channel * (1.f - a) + 255.f * a; };
                const auto r = light((colour >> 16) & 0xFF);
                const auto g = light((colour >> 8) & 0xFF);
                const auto b = light(colour & 0xFF);
                auto& dst = out[static_cast<size_t>(wy) * w + wx];
                const auto under_a = static_cast<float>(dst >> 24);
                const auto alpha = 255.f * c + under_a * (1.f - c);
                dst = (static_cast<uint32_t>(alpha + 0.5f) << 24) | (static_cast<uint32_t>(r * c + 0.5f) << 16)
                    | (static_cast<uint32_t>(g * c + 0.5f) << 8) | static_cast<uint32_t>(b * c + 0.5f);
            }
        }
    }

    BYTE opacity = 255;
    if (state.fading) {
        const auto elapsed = GetTickCount64() - state.shown_at;
        if (elapsed >= fade_in_ms)
            state.fading = false;
        else
            opacity = static_cast<BYTE>(255 * elapsed / fade_in_ms);
    }

    const auto screen_dc = wil::GetDC(nullptr);
    SIZE size{w, h};
    POINT source{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, opacity, AC_SRC_ALPHA};
    UpdateLayeredWindow(wnd, screen_dc.get(), nullptr, &size, state.memory_dc.get(), &source, 0, &blend, ULW_ALPHA);
    state.needs_present = false;
}

// ---- 音量 ----

// 讀這一格的音量，更新兩支指針；有動到要重畫就傳回 true
bool update_needles(State& state, double dt)
{
    double targets[2]{};
    double peaks[2]{};
    const auto playback = play_control::get();
    if (playback->is_playing() && !playback->is_paused()) {
        if (!state.stream) {
            try {
                visualisation_manager::get()->create_stream(state.stream, 0);
            } catch (const std::exception&) {
            }
        }
        double now{};
        audio_chunk_impl chunk;
        const auto window = std::clamp(dt, 0.016, 0.05);
        if (state.stream && state.stream->get_absolute_time(now)
            && state.stream->get_chunk_absolute(chunk, now - window, window)) {
            const auto channels = static_cast<int>(chunk.get_channels());
            const auto samples = static_cast<size_t>(chunk.get_sample_count());
            const auto* data = chunk.get_data();
            if (channels > 0 && samples > 0 && data) {
                const auto gain = std::pow(10.0, -reference_dbfs / 20.0) / std::pow(10.0, scale_top_vu / 20.0);
                for (int side = 0; side < 2; ++side) {
                    const auto channel = std::min(side, channels - 1);
                    double sum = 0;
                    double peak = 0;
                    for (size_t n = 0; n < samples; ++n) {
                        const double value = data[n * channels + channel];
                        sum += value * value;
                        peak = std::max(peak, std::abs(value));
                    }
                    targets[side] = std::sqrt(sum / samples) * gain;
                    peaks[side] = peak;
                }
            }
        }
    }

    const auto lamp_threshold = std::pow(10.0, peak_lamp_dbfs / 20.0);
    const auto omega = natural_frequency;
    bool changed = false;
    for (int side = 0; side < 2; ++side) {
        auto& needle = state.needles[side];
        const auto before_pos = needle.pos;
        const auto before_lamp = needle.lamp;
        const auto target = std::min(targets[side], 1.06);
        constexpr int steps = 8;
        const auto h = dt / steps;
        for (int step = 0; step < steps; ++step) {
            const auto acceleration
                = omega * omega * (target - needle.pos) - 2.0 * damping_ratio * omega * needle.velocity;
            needle.velocity += acceleration * h;
            needle.pos += needle.velocity * h;
            if (needle.pos < -0.02) {
                needle.pos = -0.02;
                needle.velocity = std::max(0.0, needle.velocity);
            } else if (needle.pos > 1.06) {
                needle.pos = 1.06;
                needle.velocity = std::min(0.0, needle.velocity);
            }
        }
        if (peaks[side] >= lamp_threshold) {
            needle.lamp = 1.0;
            needle.lamp_hold = lamp_hold_seconds;
        } else if (needle.lamp_hold > 0) {
            needle.lamp_hold -= dt;
        } else {
            needle.lamp = std::max(0.0, needle.lamp - dt / lamp_fade_seconds);
        }
        if (std::abs(needle.pos - before_pos) > 0.0004 || needle.lamp != before_lamp)
            changed = true;
    }
    return changed;
}

// ---- 視窗 ----

void schedule_recapture(HWND wnd);

void apply_layout(HWND wnd)
{
    auto* state = state_of(wnd);
    if (!state)
        return;

    const auto target = compute_window_rect(state->bar_rect);
    if (IsRectEmpty(&target)) {
        if (IsWindowVisible(wnd)) {
            KillTimer(wnd, frame_timer_id);
            ShowWindow(wnd, SW_HIDE);
        }
        return;
    }

    const auto visible = IsWindowVisible(wnd) != FALSE;
    const auto width = static_cast<int>(target.right - target.left);
    const auto height = static_cast<int>(target.bottom - target.top);
    const auto moved = !EqualRect(&state->window_rect, &target);

    if (!visible || moved) {
        // 先藏起來再截背景，免得截到自己
        if (visible)
            ShowWindow(wnd, SW_HIDE);
        state->window_rect = target;
        if (width != state->width || height != state->height || moved) {
            const auto vu = cover_zoom::vu_rect_in_stage(
                state->bar_rect.right - state->bar_rect.left, state->bar_rect.top);
            const auto scale = vu ? static_cast<float>(vu->bottom - vu->top) / base_height : 1.f;
            rebuild_geometry(*state, width, height, scale);
        }
        SetWindowPos(wnd, HWND_TOP, target.left, target.top, width, height, SWP_NOACTIVATE);
        capture_backdrop(wnd, *state);
        state->shown_at = GetTickCount64();
        state->fading = !visible;
        state->last_tick = GetTickCount64();
        present(wnd, *state);
        SetWindowPos(wnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SetTimer(wnd, frame_timer_id, frame_interval_ms, nullptr);
        // 剛切到 VU 模式時，背景可能還在交叉淡化、或封面虛化圖還沒換好，過一下再截兩次
        schedule_recapture(wnd);
        return;
    }

    state->static_dirty = true;
    present(wnd, *state);
}

// 重截背景（只取視窗外圍一圈，見 capture_backdrop，所以不用把自己藏起來）
void recapture(HWND wnd)
{
    auto* state = state_of(wnd);
    if (!state || !IsWindowVisible(wnd))
        return;
    capture_backdrop(wnd, *state);
    present(wnd, *state);
}

void schedule_recapture(HWND wnd)
{
    SetTimer(wnd, recapture_timer_id, recapture_delay_ms, nullptr);
    SetTimer(wnd, recapture_late_timer_id, recapture_late_delay_ms, nullptr);
}

std::vector<HWND>& live_windows()
{
    static std::vector<HWND> windows;
    return windows;
}

LRESULT CALLBACK window_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_NCCREATE:
        live_windows().push_back(wnd);
        states()[wnd] = std::make_unique<State>();
        break;
    case WM_NCDESTROY: {
        auto& windows = live_windows();
        windows.erase(std::remove(windows.begin(), windows.end(), wnd), windows.end());
        states().erase(wnd);
        break;
    }
    case WM_NCHITTEST:
        // 不擋滑鼠：點擊照常落到底下的 ESLyric
        return HTTRANSPARENT;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(wnd, &ps);
        EndPaint(wnd, &ps);
        return 0;
    }
    case WM_TIMER: {
        auto* state = state_of(wnd);
        if (!state)
            break;
        if (wp == frame_timer_id) {
            if (!IsWindowVisible(wnd)) {
                KillTimer(wnd, frame_timer_id);
                return 0;
            }
            const auto now = GetTickCount64();
            const auto dt = std::clamp((now - state->last_tick) / 1000.0, 0.001, 0.1);
            state->last_tick = now;
            const auto changed = update_needles(*state, dt);
            if (changed || state->fading || state->needs_present)
                present(wnd, *state);
            return 0;
        }
        if (wp == recapture_timer_id || wp == recapture_late_timer_id) {
            KillTimer(wnd, wp);
            recapture(wnd);
            return 0;
        }
        break;
    }
    case WM_SHOWWINDOW:
        if (!wp)
            KillTimer(wnd, frame_timer_id);
        break;
    }
    return DefWindowProc(wnd, msg, wp, lp);
}

// 換歌：背景會換，排兩次重截
class NewTrackCallback : public play_callback_static {
public:
    unsigned get_flags() override { return flag_on_playback_new_track; }
    void on_playback_new_track(metadb_handle_ptr track) override
    {
        for (const auto wnd : live_windows()) {
            if (!IsWindowVisible(wnd))
                continue;
            SetTimer(wnd, recapture_timer_id, recapture_delay_ms, nullptr);
            SetTimer(wnd, recapture_late_timer_id, recapture_late_delay_ms, nullptr);
        }
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

bool is_backlight_enabled()
{
    return fb2k::configStore::get()->getConfigBool(backlight_config_key, backlight_default);
}

void set_backlight_enabled(bool enabled)
{
    fb2k::configStore::get()->setConfigBool(backlight_config_key, enabled);
    for (const auto& [wnd, state] : states())
        state->static_dirty = true;
    refresh_layouts();
}

HWND create(HWND parent)
{
    static const auto class_registered = [] {
        WNDCLASSEX window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = window_proc;
        window_class.hInstance = core_api::get_my_instance();
        window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
        window_class.lpszClassName = window_class_name;
        return RegisterClassEx(&window_class) != 0;
    }();

    if (!class_registered)
        return nullptr;

    const auto wnd = CreateWindowEx(WS_EX_LAYERED, window_class_name, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0,
        parent, nullptr, core_api::get_my_instance(), nullptr);

    // 主題字型暖機做完要重畫字（沒做完時先不畫字）
    if (wnd) {
        static const auto registered = [] {
            next_track_toast::when_gdiplus_fonts_ready([] {
                for (const auto& [wnd, state] : states())
                    state->static_dirty = true;
                refresh_layouts();
            });
            return true;
        }();
        (void)registered;
    }
    return wnd;
}

void layout(HWND wnd, const RECT& bar_rect)
{
    if (auto* state = state_of(wnd)) {
        state->bar_rect = bar_rect;
        apply_layout(wnd);
    }
}

HDWP defer_layout(HDWP dwp, HWND wnd, const RECT& bar_rect)
{
    auto* state = state_of(wnd);
    if (!state)
        return dwp;
    state->bar_rect = bar_rect;
    const auto target = compute_window_rect(bar_rect);

    if (IsRectEmpty(&target)) {
        if (IsWindowVisible(wnd))
            return DeferWindowPos(
                dwp, wnd, nullptr, 0, 0, 0, 0, SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return dwp;
    }
    // 位置或大小要變的、還藏著的都交給之後的 layout()（要先截背景再顯示）
    return dwp;
}

void refresh_layouts()
{
    for (const auto wnd : live_windows())
        apply_layout(wnd);
}

} // namespace cui::vu_meter
