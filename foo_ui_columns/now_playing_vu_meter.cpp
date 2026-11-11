#include "pch.h"

#include <cmath>
#include <unordered_map>

#if __has_include("nocturne_build_info.h")
#include "nocturne_build_info.h"
#endif

#include "now_playing_vu_meter.h"
#include "now_playing_cover_zoom.h"
#include "menu_items.h"
#include "next_track_toast.h"
#include "nocturne_text.h"
#include "splitter.h"

// 展開頁自繪 VU 表，說明見 now_playing_vu_meter.h

namespace cui::vu_meter {

namespace {

constexpr wchar_t window_class_name[] = L"columns_ui_vu_meter";

constexpr double pi = 3.14159265358979323846;

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
// 刻度最右邊是 +3 VU
constexpr double scale_top_vu = 3.0;
// 峰值燈：固定校準的設定是瞬間峰值到 −1 dBFS 就亮（破音警示）；「自動」改看短時間音量，見 auto_lamp_vu
constexpr double peak_lamp_dbfs = -1.0;
constexpr double lamp_hold_seconds = 0.15;
constexpr double lamp_fade_seconds = 0.4;
// 指針設定（右鍵挑選卡「指針」那排，2026-11-12）：0 VU 對到多少 dBFS（平均音量 RMS），與二階系統的自然頻率、阻尼比。
// - 標準：−7 dBFS（使用者 2026-11-11 先用的 C 組：外掛 VU 增益 +7）、VU 規格 300 毫秒到 99%
//   （2026-11-12 實測修正：原本用 4.6／(ζ·0.3)「1% 包絡線」公式算出 19.2，約 210 毫秒就到 99%，比規格快三成；ζ 0.8 時 13.2 才剛好 300 毫秒、過衝 1.3%）
// - 活潑：−10 dBFS、300 毫秒（響亮的歌副歌會碰到 0、偶爾進紅區）
// - 靈敏：−10 dBFS、150 毫秒到 99%（ζ 0.72、過衝約 3%），擺幅約大一倍，比較不像真 VU
// - 自動：0 VU 跟著歌曲長時間平均音量走（update_auto_reference），反應同標準
struct NeedleTuning {
    double reference_dbfs;
    double natural_frequency;
    double damping_ratio;
    bool automatic;
};
constexpr NeedleTuning needle_tunings[needle_preset_count] = {
    {-7.0, 13.2, 0.8, false},
    {-10.0, 13.2, 0.8, false},
    {-10.0, 22.2, 0.72, false},
    {-7.0, 13.2, 0.8, true},
};
// 自動校準
constexpr double auto_gate_dbfs = -45.0;       // 比這小聲的片段不算進平均
constexpr double auto_average_vu_offset = 1.75; // 平均音量對到 −1.75 VU
// 0 VU 最低可以對到 −30 dBFS（原本 −20：很安靜的錄音如 Don Quixote 大提琴獨奏、DSD 的悲しい酒會卡在下限，指針中位只有 −13～−16 VU）
constexpr double auto_reference_min = -30.0;
constexpr double auto_reference_max = -4.0;
constexpr double auto_fast_seconds = 1.5;
constexpr double auto_slow_seconds = 8.0;
constexpr double auto_rise_seconds = 3.0; // 變大聲時跟得快一點
constexpr double auto_warm_seconds = 3.0;
// 「自動」的峰值燈：最近約 1/8 秒（auto_lamp_frames 格）的音量超過 +3.5 VU（刻度右端再高半格）就亮＝「這一下大聲到爆表」。
// 2026-11-12 使用者要燈跟著自動一起調整。不看瞬間峰值：169 的歌峰值常卡在同一個值（母帶壓滿，nazonazo 全在 −4.2 dBFS），
// 門檻設在那之下就一直亮、之上就完全不亮。第一版看單格（16 毫秒）音量 +3 VU，三首歌調的；
// 同天用 55 首實錄（常聽、Vocaloid、OST、古典、DSD…）重調：單格太敏感，44 首每分鐘閃超過 10 次、34 首亮著超過 8% 的時間；
// 改成約 1/8 秒、+3.5 VU 後每首中位每分鐘 2～3 次，動態大的曲子（鋼琴、吹奏樂、OST 重擊）閃得多，壓滿的流行歌偶爾才閃
constexpr double auto_lamp_vu = 3.5;
constexpr int auto_lamp_frames = 8;
// 指針吃的音量：最近 needle_frames 格（約 50 毫秒）的平均功率。單格（16 毫秒）對低音太短，指針會細細抖動；
// 55 首實錄比較：3 格時細抖少約兩成，其他表現幾乎不變
constexpr int needle_frames = 3;
constexpr int power_history = 8; // 留幾格的功率（取上面兩個的大者）
// 沒訊號（暫停、停止）時的回落：阻尼比 0.8、自然頻率 3.4。2026-11-12 實測（從 −11 VU 暫停）：約 0.35 秒落到一半、1.1 秒回到左邊停住、不反彈。
// 第一版用「1.1 秒停穩」算，但大部分路程在前 0.3 秒就走完，看起來還是一下就掉回去（2026-11-11 實測）
constexpr double rest_damping_ratio = 0.8;
constexpr double rest_frequency = 3.4;
// 兩端擋針的位置與反彈
constexpr double stop_low = -0.02;
constexpr double stop_high = 1.06;
constexpr double stop_bounce = 0.3;

// 大小：照歌詞欄算出的位置（cover_zoom::vu_rect_in_stage）以中心放大這麼多倍，
// 但左邊至少離封面 min_cover_gap_dip、右邊不超出展開頁（2026-11-11 使用者說可以再放大一點）。
// 這個範圍叫 band，是方框雙錶的位置；其他樣式在 band 裡面擺（style_rect）
constexpr double size_scale = 1.25;
constexpr int min_cover_gap_dip = 56;
constexpr int min_right_gap_dip = 32;

// ---- 外觀：以方框錶框高 157 為基準的像素，實際乘上 scale（＝band 高 ÷ 157）----
// 三種樣式的字、刻度線、指針粗細都用同一個 scale，換樣式時字的大小不變
// （2026-11-12 使用者說圓形預覽「有點擠」：預覽的字跟著圓一起放大，圓變大也一樣擠，所以字改成不跟著圓走）
constexpr float base_height = 157.f;
constexpr float bezel_curvature = 2.2f; // 厚度曲線 (1−(1−t)^k)^(1/k)
constexpr float rim_strength = 0.70f;
constexpr float shadow_alpha = 0.42f;
constexpr float shadow_offset_px = 10.f;
constexpr float shadow_blur_px = 9.f; // 三次方塊模糊的半徑

// 方框雙錶
constexpr float box_gap_px = 22.f;        // 左右兩個錶框的間距（2026-11-11 使用者要再分開一點，10→22）
constexpr float box_radius_px = 16.f;     // 錶框圓角
constexpr float box_bezel_px = 10.f;      // 玻璃邊緣折射的寬度
constexpr float box_refraction_px = 8.f;  // 邊緣最大位移
constexpr double box_half_span = 42.0 * pi / 180.0; // 刻度半邊角度

// 圓形錶頭（2026-11-12）：使用者看預覽說「有點擠、可以放大點」→
// 1. 兩顆圓用滿方框雙錶的寬度（直徑約是預覽的 1.5 倍）；2. 字和線條粗細照方框的倍率，不跟著圓放大；
// 3. 指針軸移到圓心下方 0.46 半徑，刻度弧半徑照「兩端數字剛好碰不到玻璃邊」反推（dial_of），弧比預覽長約三成
constexpr float round_gap_px = 34.f;
// 2026-11-12 使用者實際看了說圓形「太大、等比例縮小一點」：整組（圓、字、線條、間距、陰影）乘這個倍數，
// 寬度從用滿方框雙錶的範圍縮成 0.82 倍（直徑約 414→340 像素＠1080p），鬆緊度不變
constexpr float round_size_ratio = 0.82f;
constexpr float round_pivot_drop = 0.46f; // 指針軸在圓心下方幾倍半徑
constexpr double round_half_span = 44.0 * pi / 180.0;
constexpr float round_bezel_px = 12.f;
constexpr float round_refraction_px = 10.f;
constexpr float round_label_clearance_px = 11.f; // 兩端數字中心離玻璃折射邊至少這麼遠

// 單錶雙指針：寬是方框雙錶的 0.8 倍、高寬比 0.35（照預覽 600×210）；刻度與指針軸的位置以錶寬 600 為基準
constexpr double dual_width_ratio = 0.8;
constexpr double dual_aspect = 0.35;
constexpr float dual_radius_px = 20.f;
constexpr float dual_bezel_px = 10.f;
constexpr float dual_refraction_px = 8.f;
constexpr double dual_half_span = 36.0 * pi / 180.0;

// 視窗比錶本體多留的邊（給陰影）
constexpr float margin_x_px = 20.f;
constexpr float margin_top_px = 14.f;
constexpr float margin_bottom_px = 30.f;

double pos_of_db(double db)
{
    return std::pow(10.0, db / 20.0) / std::pow(10.0, scale_top_vu / 20.0);
}

struct Needle {
    double pos{};
    double velocity{};
    double lamp{};
    double lamp_hold{};
};

struct State {
    Style style{Style::box};
    RECT bar_rect{};
    RECT window_rect{}; // 分割面板座標
    int width{};
    int height{};
    float scale{1.f};
    // 玻璃錶框：方框與圓形兩個、雙指針一個；同一個樣式的錶框一樣大
    int panel_count{};
    Gdiplus::RectF panels[2];
    float panel_radius{};
    float bezel{};
    float refraction{};

    // 截到的背景（視窗大小，BGRA）
    std::vector<uint32_t> backdrop;
    bool has_backdrop{};
    // 不動的那一層：背景（錶框內已調飽和、壓暗）＋背光＋刻度與字
    std::vector<uint32_t> base;
    // 每格畫面：base＋指針、峰值燈、軸蓋
    std::vector<uint32_t> content;
    bool static_dirty{true};

    // 錶框外的陰影（0～1）與換算好的預乘像素（每格直接拷貝）
    std::vector<float> shadow;
    std::vector<uint32_t> shadow_px;
    // 只重畫有變的地方（2026-11-12：4K 時整張重畫每格要 5～8 毫秒）：上一格每個錶框指針涵蓋的範圍；
    // full_redraw＝下一格要整張重畫（大小、背景、錶面變了）
    RECT needle_rects[2]{};
    bool full_redraw{true};
    // 錶框裡的位移、覆蓋率、反光（錶框一樣大，共用一份）
    int panel_w{};
    int panel_h{};
    std::vector<float> dx;
    std::vector<float> dy;
    std::vector<float> coverage;
    std::vector<float> specular;

    Needle needles[2];
    // 「自動」校準：長時間平均功率、這首歌已經量了幾秒、目前 0 VU 對到的 dBFS
    double auto_power{};
    double auto_warm{};
    double auto_reference{-7.0};
    // 最近幾格的功率（每聲道，環狀），指針與「自動」峰值燈取平均用
    double power_history_[2][power_history]{};
    int power_index{};
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

// 錶框外形：圓角矩形；圓角半徑到一半寬高時是圓（或膠囊）
void add_panel_path(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& rect, float radius)
{
    if (radius * 2.f >= std::min(rect.Width, rect.Height) - 0.01f && std::abs(rect.Width - rect.Height) < 0.5f) {
        path.AddEllipse(rect);
        return;
    }
    radius = std::min({radius, rect.Width / 2.f, rect.Height / 2.f});
    const auto d = radius * 2.f;
    path.AddArc(rect.X, rect.Y, d, d, 180.f, 90.f);
    path.AddArc(rect.X + rect.Width - d, rect.Y, d, d, 270.f, 90.f);
    path.AddArc(rect.X + rect.Width - d, rect.Y + rect.Height - d, d, d, 0.f, 90.f);
    path.AddArc(rect.X, rect.Y + rect.Height - d, d, d, 90.f, 90.f);
    path.CloseFigure();
}

// 圓角矩形的有號距離（裡面是負的），以中心為原點；radius＝半寬＝半高時就是圓
float rounded_rect_sdf(float px, float py, float half_w, float half_h, float radius)
{
    const auto qx = std::abs(px) - (half_w - radius);
    const auto qy = std::abs(py) - (half_h - radius);
    const auto ox = std::max(qx, 0.f);
    const auto oy = std::max(qy, 0.f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.f) - radius;
}

// ---- 樣式設定 ----

Style& cached_style()
{
    static Style style = [] {
        const auto value = fb2k::configStore::get()->getConfigInt(style_config_key, static_cast<int>(style_default));
        return value >= 0 && value < style_count ? static_cast<Style>(value) : style_default;
    }();
    return style;
}

NeedlePreset& cached_needle_preset()
{
    static NeedlePreset preset = [] {
        const auto value
            = fb2k::configStore::get()->getConfigInt(needle_preset_config_key, static_cast<int>(needle_preset_default));
        return value >= 0 && value < needle_preset_count ? static_cast<NeedlePreset>(value) : needle_preset_default;
    }();
    return preset;
}

// ---- 版面 ----

// 刻度弧：指針軸 (cx, cy)、弧半徑、半邊角度
struct Dial {
    float cx;
    float cy;
    float radius;
    double half_span;
};

double angle_of_pos(const Dial& dial, double pos)
{
    return -dial.half_span + 2.0 * dial.half_span * std::clamp(pos, -0.02, 1.04);
}

Gdiplus::PointF point_at(const Dial& dial, double angle, float r)
{
    return {dial.cx + r * static_cast<float>(std::sin(angle)), dial.cy - r * static_cast<float>(std::cos(angle))};
}

Dial dial_of(const State& state, const Gdiplus::RectF& panel)
{
    const auto s = state.scale;
    switch (state.style) {
    case Style::round: {
        const auto rc = panel.Width / 2.f;
        const auto drop = round_pivot_drop * rc;
        // 刻度數字在弧外 18 的地方；兩端那兩個數字的中心要在玻璃折射邊往內 round_label_clearance_px 以內：
        // 以圓心為原點，數字半徑 L 在角度 θ 的點離圓心 √(L² − 2·drop·L·cosθ + drop²) ≤ limit，解出最大的 L
        const auto limit = rc - state.bezel - round_label_clearance_px * s;
        const auto c = static_cast<float>(std::cos(round_half_span));
        const auto sn = static_cast<float>(std::sin(round_half_span));
        const auto label = drop * c + std::sqrt(std::max(0.f, limit * limit - drop * drop * sn * sn));
        return {panel.X + rc, panel.Y + rc + drop, std::max(label - 18.f * s, rc * 0.4f), round_half_span};
    }
    case Style::dual: {
        const auto u = panel.Width / 600.f;
        const auto r = 300.f * u;
        return {panel.X + panel.Width / 2.f, panel.Y + 52.f * u + r, r, dual_half_span};
    }
    default: {
        const auto r = panel.Width * 0.53f;
        return {panel.X + panel.Width / 2.f, panel.Y + 45.f * s + r, r, box_half_span};
    }
    }
}

// 方框雙錶的位置（band，展開頁左上角為原點）：歌詞欄算出的 VU 位置以中心放大 size_scale 倍，
// 左右放不下就縮回、再往空的那邊挪
std::optional<RECT> band_rect_in_stage(int stage_width, int stage_height)
{
    const auto base = cover_zoom::vu_rect_in_stage(stage_width, stage_height);
    if (!base)
        return {};

    auto min_left = 0;
    if (const auto cover = cover_zoom::cover_rect_in_stage(stage_width, stage_height))
        min_left = cover->right + uih::scale_dpi_value(min_cover_gap_dip);
    min_left = std::min(min_left, static_cast<int>(base->left));
    const auto max_right = std::max(stage_width - uih::scale_dpi_value(min_right_gap_dip), static_cast<int>(base->right));

    const auto base_w = static_cast<int>(base->right - base->left);
    const auto room = max_right - min_left;
    const auto width = std::clamp(static_cast<int>(std::lround(base_w * size_scale)), base_w, std::max(base_w, room));
    const auto height = MulDiv(width, static_cast<int>(base->bottom - base->top), base_w);
    if (height <= 0 || height > stage_height)
        return base;

    const auto centre = (base->left + base->right) / 2;
    auto left = static_cast<int>(centre - width / 2);
    left = std::clamp(left, min_left, std::max(min_left, max_right - width));
    const auto top = (stage_height - height) / 2;
    return RECT{left, top, left + width, top + height};
}

// 錶本體（不含陰影邊）在展開頁裡的位置：方框＝band；圓形＝兩顆圓用滿 band 的寬；雙指針＝band 寬的 0.8 倍。
// 都在 band 裡水平置中、在展開頁垂直置中；太高時（視窗很扁）縮到放得下
// 這個樣式的字、線條粗細倍率（band 高 ÷ 157；圓形再乘 round_size_ratio）
float style_scale(Style style, float band_scale)
{
    return style == Style::round ? band_scale * round_size_ratio : band_scale;
}

RECT style_rect(Style style, const RECT& band, int stage_height)
{
    const auto full_w = static_cast<float>(band.right - band.left);
    auto band_w = full_w;
    const auto band_h = static_cast<float>(band.bottom - band.top);
    const auto s = style_scale(style, band_h / base_height);
    if (style == Style::round)
        band_w *= round_size_ratio;
    const auto max_h = std::max(1.f, stage_height - (margin_top_px + margin_bottom_px) * s);
    float w{};
    float h{};
    switch (style) {
    case Style::round: {
        const auto gap = round_gap_px * s;
        auto d = std::floor((band_w - gap) / 2.f);
        d = std::min(d, std::floor(max_h));
        w = 2.f * d + gap;
        h = d;
        break;
    }
    case Style::dual:
        w = std::round(band_w * static_cast<float>(dual_width_ratio));
        h = std::min(std::round(w * static_cast<float>(dual_aspect)), std::floor(max_h));
        break;
    default:
        return band;
    }
    const auto left = band.left + static_cast<LONG>(std::lround((full_w - w) / 2.f));
    const auto top = static_cast<LONG>(std::lround((stage_height - h) / 2.f));
    return RECT{left, top, left + static_cast<LONG>(w), top + static_cast<LONG>(h)};
}

// 錶本體＋陰影邊（展開頁座標）
struct WindowPlan {
    RECT rect;
    float scale;
};

std::optional<WindowPlan> window_plan_in_stage(Style style, int stage_width, int stage_height)
{
    const auto band = band_rect_in_stage(stage_width, stage_height);
    if (!band)
        return {};
    const auto s = style_scale(style, static_cast<float>(band->bottom - band->top) / base_height);
    const auto vu = style_rect(style, *band, stage_height);
    RECT rect{vu.left - static_cast<LONG>(margin_x_px * s), vu.top - static_cast<LONG>(margin_top_px * s),
        vu.right + static_cast<LONG>(margin_x_px * s), vu.bottom + static_cast<LONG>(margin_bottom_px * s)};
    rect.top = std::max(rect.top, 0L);
    rect.bottom = std::min(rect.bottom, static_cast<LONG>(stage_height));
    return WindowPlan{rect, s};
}

RECT compute_window_rect(const RECT& bar_rect, Style style)
{
    if (bar_rect.right <= bar_rect.left)
        return {};

    RECT stage_screen{};
    HWND owner{};
    if (!panels::splitter::FlatSplitterPanel::s_get_now_playing_stage(stage_screen, owner))
        return {};
    if (main_menu::get_now_playing_mode() != main_menu::NowPlayingMode::vu)
        return {};

    const auto plan = window_plan_in_stage(style, bar_rect.right - bar_rect.left, bar_rect.top);
    if (!plan)
        return {};
    auto rect = plan->rect;
    OffsetRect(&rect, bar_rect.left, 0);
    return rect;
}

// 三次方塊模糊（近似高斯），邊緣外當作跟邊緣一樣
void box_blur(std::vector<float>& mask, int width, int height, int blur_r)
{
    std::vector<float> temp(mask.size());
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
}

// 視窗大小變了：重算錶框位置、陰影、位移與反光（state.style 要先設好）
void rebuild_geometry(State& state, int width, int height, float scale)
{
    state.width = width;
    state.height = height;
    state.scale = scale;
    const auto s = scale;
    const auto mx = margin_x_px * s;
    const auto mt = margin_top_px * s;
    const auto vu_w = width - 2.f * mx;
    const auto max_h = std::max(1.f, std::floor(height - mt));

    switch (state.style) {
    case Style::round: {
        const auto gap = round_gap_px * s;
        const auto d = std::max(1.f, std::min(std::floor((vu_w - gap) / 2.f), max_h));
        state.panel_count = 2;
        state.panels[0] = {std::round(mx), std::round(mt), d, d};
        state.panels[1] = {std::round(mx + d + gap), std::round(mt), d, d};
        state.panel_radius = d / 2.f;
        state.bezel = round_bezel_px * s;
        state.refraction = round_refraction_px * s;
        break;
    }
    case Style::dual: {
        const auto pw = std::max(1.f, std::floor(vu_w));
        const auto ph = std::max(1.f, std::min(std::round(pw * static_cast<float>(dual_aspect)), max_h));
        state.panel_count = 1;
        state.panels[0] = {std::round(mx), std::round(mt), pw, ph};
        state.panels[1] = state.panels[0];
        state.panel_radius = dual_radius_px * s;
        state.bezel = dual_bezel_px * s;
        state.refraction = dual_refraction_px * s;
        break;
    }
    default: {
        const auto ph = std::max(1.f, std::min(std::round(base_height * s), max_h));
        const auto pw = std::max(1.f, std::floor((vu_w - box_gap_px * s) / 2.f));
        state.panel_count = 2;
        state.panels[0] = {std::round(mx), std::round(mt), pw, ph};
        state.panels[1] = {std::round(mx + pw + box_gap_px * s), std::round(mt), pw, ph};
        state.panel_radius = box_radius_px * s;
        state.bezel = box_bezel_px * s;
        state.refraction = box_refraction_px * s;
        break;
    }
    }

    const auto n = static_cast<size_t>(width) * height;
    state.backdrop.assign(n, 0xFF141618);
    state.has_backdrop = false;
    state.base.assign(n, 0);
    state.content.assign(n, 0);
    state.static_dirty = true;

    // 陰影：錶框形狀（內縮 2）往下移，三次方塊模糊
    std::vector<float> mask(n, 0.f);
    for (int index = 0; index < state.panel_count; ++index) {
        const auto& panel = state.panels[index];
        const auto hw = panel.Width / 2.f - 2.f * s;
        const auto hh = panel.Height / 2.f - 2.f * s;
        const auto radius = std::min({state.panel_radius, hw, hh});
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
    box_blur(mask, width, height, std::max(1, static_cast<int>(std::lround(shadow_blur_px * s))));
    for (auto& value : mask)
        value *= shadow_alpha;
    state.shadow = std::move(mask);
    state.shadow_px.resize(n);
    for (size_t i = 0; i < n; ++i)
        state.shadow_px[i] = static_cast<uint32_t>(state.shadow[i] * 255.f + 0.5f) << 24;
    state.full_redraw = true;

    // 玻璃：邊緣往內取樣（放大），反光照邊緣朝向（光從左上來）
    const auto pw = static_cast<int>(state.panels[0].Width);
    const auto ph = static_cast<int>(state.panels[0].Height);
    state.panel_w = pw;
    state.panel_h = ph;
    const auto pn = static_cast<size_t>(pw) * ph;
    state.dx.assign(pn, 0.f);
    state.dy.assign(pn, 0.f);
    state.coverage.assign(pn, 0.f);
    state.specular.assign(pn, 0.f);
    const auto hw = pw / 2.f;
    const auto hh = ph / 2.f;
    const auto radius = std::min({state.panel_radius, hw, hh});
    const auto bezel = state.bezel;
    const auto refraction = state.refraction;
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

// 讀整個展開頁的截圖（BGRA）
bool read_stage_pixels(std::vector<uint32_t>& pixels, int& stage_w, int& stage_h, RECT& stage_screen)
{
    HWND owner{};
    const auto bitmap = panels::splitter::FlatSplitterPanel::s_capture_now_playing_stage(stage_screen, owner);
    if (!bitmap)
        return false;

    BITMAP info{};
    GetObject(bitmap.get(), sizeof(info), &info);
    stage_w = static_cast<int>(info.bmWidth);
    stage_h = static_cast<int>(info.bmHeight);
    if (stage_w <= 0 || stage_h <= 0)
        return false;

    pixels.resize(static_cast<size_t>(stage_w) * stage_h);
    BITMAPINFO header{};
    header.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    header.bmiHeader.biWidth = stage_w;
    header.bmiHeader.biHeight = -stage_h;
    header.bmiHeader.biPlanes = 1;
    header.bmiHeader.biBitCount = 32;
    header.bmiHeader.biCompression = BI_RGB;
    const auto screen_dc = wil::GetDC(nullptr);
    return GetDIBits(screen_dc.get(), bitmap.get(), 0, stage_h, pixels.data(), &header, DIB_RGB_COLORS) != 0;
}

// 截圖一定會連這個視窗自己（還有截圖當下剛好還在的歌詞）一起截進去（PrintWindow 主視窗或 ESLyric 視窗都一樣，2026-11-11 實測），
// 所以不直接用視窗裡面那塊：只取視窗外圍一圈（往外 3～10 像素）的背景色，再用 Coons 曲面從四邊往內補滿。
// 封面虛化背景很平滑，補出來的跟真的幾乎一樣，而且不用把自己藏起來再截（換歌時不會閃）
// (ox, oy)＝視窗左上角在截圖裡的位置
void fill_backdrop(State& state, const std::vector<uint32_t>& pixels, int stage_w, int stage_h, int ox, int oy)
{
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
    state.full_redraw = true;
}

// 取錶框後面的虛化背景
void capture_backdrop(HWND wnd, State& state)
{
    std::vector<uint32_t> pixels;
    int stage_w{};
    int stage_h{};
    RECT stage_screen{};
    if (!read_stage_pixels(pixels, stage_w, stage_h, stage_screen))
        return;

    RECT screen = state.window_rect;
    MapWindowPoints(GetParent(wnd), HWND_DESKTOP, reinterpret_cast<POINT*>(&screen), 2);
    fill_backdrop(state, pixels, stage_w, stage_h, static_cast<int>(screen.left - stage_screen.left),
        static_cast<int>(screen.top - stage_screen.top));
}

// ---- 錶面（不動的部分）----

void draw_text(Gdiplus::Graphics& g, const wchar_t* text, const Gdiplus::Font& font, const Gdiplus::Color& colour,
    Gdiplus::PointF at, Gdiplus::StringAlignment align, float s)
{
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
}

const Gdiplus::Color ink(214, 244, 240, 230);
const Gdiplus::Color ink_soft(107, 244, 240, 230);
const Gdiplus::Color scale_red(242, 240, 98, 74);

// 刻度弧、刻度線、數字、兩端正負號
void draw_scale(Gdiplus::Graphics& g, const Dial& m, float s, const Gdiplus::FontFamily* family)
{
    const auto arc = [&](const Gdiplus::Pen& pen, double from_pos, double to_pos, float r) {
        const auto a1 = angle_of_pos(m, from_pos);
        const auto a2 = angle_of_pos(m, to_pos);
        g.DrawArc(&pen, m.cx - r, m.cy - r, 2 * r, 2 * r, static_cast<float>(-90.0 + a1 * 180.0 / pi),
            static_cast<float>((a2 - a1) * 180.0 / pi));
    };

    Gdiplus::Pen main_pen(ink, 1.5f * s);
    arc(main_pen, pos_of_db(-20), pos_of_db(0), m.radius);
    Gdiplus::Pen red_pen(Gdiplus::Color(224, 240, 98, 74), 4.5f * s);
    arc(red_pen, pos_of_db(0), pos_of_db(3), m.radius + 2.6f * s);

    const auto tick = [&](double db, float length, const Gdiplus::Color& colour, float width) {
        const auto a = angle_of_pos(m, pos_of_db(db));
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
        tick(db, (big ? 9.f : 6.f) * s, db > 0 ? scale_red : ink, 1.2f * s);
    }

    if (!family)
        return;

    const auto style = regular_style(*family);
    const Gdiplus::Font number_font(family, 14.f * s, style, Gdiplus::UnitPixel);
    for (const auto db : marks) {
        const auto a = angle_of_pos(m, pos_of_db(db));
        const auto at = point_at(m, a, m.radius + 18.f * s);
        const auto text = std::to_wstring(static_cast<int>(std::abs(db)));
        draw_text(g, text.c_str(), number_font, db > 0 ? scale_red : ink, at, Gdiplus::StringAlignmentCenter, s);
    }

    // 刻度兩端的正負號（紅區數字不寫 +）；角度差換算成固定弧長，弧半徑不同時離數字一樣遠
    const auto sign_offset = 0.10 * (0.53 * 397.0 / 1.25) * s / m.radius;
    const Gdiplus::Font sign_font(family, 15.f * s, bold_style(*family), Gdiplus::UnitPixel);
    draw_text(g, L"−", sign_font, Gdiplus::Color(178, 244, 240, 230),
        point_at(m, angle_of_pos(m, pos_of_db(-20)) - sign_offset, m.radius + 4.f * s), Gdiplus::StringAlignmentCenter,
        s);
    draw_text(g, L"+", sign_font, scale_red,
        point_at(m, angle_of_pos(m, pos_of_db(3)) + sign_offset * 0.9, m.radius + 4.f * s),
        Gdiplus::StringAlignmentCenter, s);
}

// 峰值燈的位置：每個錶框有幾顆、各是哪個聲道
struct Lamp {
    Gdiplus::PointF at;
    int channel;
};

int lamps_of(const State& state, int index, Lamp* out)
{
    const auto& panel = state.panels[index];
    const auto s = state.scale;
    switch (state.style) {
    case Style::round: {
        const auto rc = panel.Width / 2.f;
        out[0] = {{panel.X + rc + 0.40f * rc, panel.Y + rc + 0.30f * rc}, index};
        return 1;
    }
    case Style::dual: {
        const auto u = panel.Width / 600.f;
        out[0] = {{panel.X + panel.Width - 46.f * u, panel.Y + 26.f * u}, 0};
        out[1] = {{panel.X + panel.Width - 26.f * u, panel.Y + 26.f * u}, 1};
        return 2;
    }
    default:
        out[0] = {{panel.X + panel.Width - 22.f * s, panel.Y + 22.f * s}, index};
        return 1;
    }
}

float lamp_radius(const State& state)
{
    return (state.style == Style::box ? 3.6f : 4.2f) * state.scale;
}

// 背光、刻度以外的字：VU 標誌、聲道、校準字、PEAK
void draw_face_text(Gdiplus::Graphics& g, const State& state, int index, const Gdiplus::FontFamily& family)
{
    const auto& panel = state.panels[index];
    const auto s = state.scale;
    const auto style = regular_style(family);
    const auto bold = bold_style(family);
    wchar_t reference[64]{};
    const auto& tuning = needle_tunings[static_cast<int>(cached_needle_preset())];
    if (tuning.automatic)
        swprintf_s(reference, L"AUTO"); // 2026-11-12 使用者說只寫 AUTO 就好
    else
        swprintf_s(reference, L"0 VU = −%d dBFS", static_cast<int>(std::lround(-tuning.reference_dbfs)));
    const Gdiplus::Color vu_colour(204, 244, 240, 230);
    const Gdiplus::Color channel_colour(158, 244, 240, 230);
    const Gdiplus::Color faint(122, 244, 240, 230);
    const Gdiplus::Font vu_font(&family, 24.f * s, bold, Gdiplus::UnitPixel);
    const Gdiplus::Font channel_font(&family, 13.f * s, bold, Gdiplus::UnitPixel);
    const Gdiplus::Font tiny_font(&family, 11.f * s, style, Gdiplus::UnitPixel);
    const Gdiplus::Font lamp_font(&family, 9.5f * s, style, Gdiplus::UnitPixel);
    const auto* channel = index == 0 ? L"L" : L"R";

    switch (state.style) {
    case Style::round: {
        // 圓形：VU 在指針軸上方；聲道與峰值燈左右對稱擺在指針軸兩側；校準字在最下面
        const auto rc = panel.Width / 2.f;
        const auto cx = panel.X + rc;
        const auto cy = panel.Y + rc;
        draw_text(g, L"VU", vu_font, vu_colour, {cx, cy + 0.04f * rc}, Gdiplus::StringAlignmentCenter, s);
        const Gdiplus::Font round_channel_font(&family, 15.f * s, bold, Gdiplus::UnitPixel);
        draw_text(g, channel, round_channel_font, channel_colour, {cx - 0.40f * rc, cy + 0.30f * rc},
            Gdiplus::StringAlignmentCenter, s);
        draw_text(g, L"PEAK", lamp_font, faint, {cx + 0.40f * rc, cy + 0.30f * rc + 13.f * s},
            Gdiplus::StringAlignmentCenter, s);
        draw_text(g, reference, tiny_font, faint, {cx, cy + 0.70f * rc}, Gdiplus::StringAlignmentCenter, s);
        break;
    }
    case Style::dual: {
        // 雙指針：左下角是聲道圖例（白點 L、琥珀點 R），右下角校準字，右上角 PEAK 和兩顆燈
        const auto u = panel.Width / 600.f;
        const auto bottom = panel.Y + panel.Height - 22.f * u;
        const Gdiplus::Font dual_vu_font(&family, 27.f * s, bold, Gdiplus::UnitPixel);
        draw_text(g, L"VU", dual_vu_font, vu_colour, {panel.X + panel.Width / 2.f, panel.Y + panel.Height * 0.62f},
            Gdiplus::StringAlignmentCenter, s);
        const auto dot_r = 3.6f * s;
        const Gdiplus::SolidBrush white(rgba(250, 247, 240, 0.95));
        const Gdiplus::SolidBrush amber(rgba(240, 176, 80, 0.95));
        auto x = panel.X + 26.f * u;
        g.FillEllipse(&white, x - dot_r, bottom - dot_r, 2 * dot_r, 2 * dot_r);
        draw_text(g, L"L", channel_font, channel_colour, {x + 9.f * s, bottom}, Gdiplus::StringAlignmentNear, s);
        x += 30.f * s;
        g.FillEllipse(&amber, x - dot_r, bottom - dot_r, 2 * dot_r, 2 * dot_r);
        draw_text(g, L"R", channel_font, channel_colour, {x + 9.f * s, bottom}, Gdiplus::StringAlignmentNear, s);
        draw_text(g, reference, tiny_font, faint, {panel.X + panel.Width - 20.f * u, bottom},
            Gdiplus::StringAlignmentFar, s);
        draw_text(g, L"PEAK", lamp_font, faint, {panel.X + panel.Width - 58.f * u, panel.Y + 26.f * u},
            Gdiplus::StringAlignmentFar, s);
        break;
    }
    default:
        draw_text(g, L"VU", vu_font, vu_colour, {panel.X + panel.Width / 2.f, panel.Y + panel.Height * 0.64f},
            Gdiplus::StringAlignmentCenter, s);
        draw_text(g, channel, channel_font, channel_colour, {panel.X + 18.f * s, panel.Y + panel.Height - 18.f * s},
            Gdiplus::StringAlignmentNear, s);
        draw_text(g, reference, tiny_font, faint, {panel.X + panel.Width - 16.f * s, panel.Y + panel.Height - 18.f * s},
            Gdiplus::StringAlignmentFar, s);
        draw_text(g, L"PEAK", lamp_font, faint, {panel.X + panel.Width - 30.f * s, panel.Y + 22.f * s},
            Gdiplus::StringAlignmentFar, s);
        break;
    }
}

// 暖色背光：從錶框下方往上散開
void draw_backlight(Gdiplus::Graphics& g, const State& state, const Gdiplus::RectF& panel)
{
    const auto s = state.scale;
    auto cx = panel.X + panel.Width / 2.f;
    auto cy = panel.Y + panel.Height + 8.f * s;
    auto r = panel.Width * 0.62f;
    if (state.style == Style::round) {
        const auto rc = panel.Width / 2.f;
        cy = panel.Y + rc + (round_pivot_drop + 0.2f) * rc;
        r = 1.1f * rc;
    } else if (state.style == Style::dual) {
        cy = panel.Y + panel.Height + 10.f * panel.Width / 600.f;
        r = panel.Width * 0.5f;
    }
    Gdiplus::GraphicsPath glow_path;
    glow_path.AddEllipse(cx - r, cy - r, 2 * r, 2 * r);
    Gdiplus::PathGradientBrush glow(&glow_path);
    glow.SetCenterPoint(Gdiplus::PointF(cx, cy));
    Gdiplus::Color colours[] = {rgba(255, 190, 120, 0), rgba(255, 190, 120, 0.07), rgba(255, 190, 120, 0.20)};
    Gdiplus::REAL positions[] = {0.f, 0.5f, 1.f};
    glow.SetInterpolationColors(colours, positions, 3);
    g.FillRectangle(&glow, panel);
}

void rebuild_static(State& state)
{
    const auto w = state.width;
    const auto h = state.height;
    state.base = state.backdrop;

    // 錶框裡：背景調飽和、依亮度壓暗
    for (int index = 0; index < state.panel_count; ++index) {
        const auto& panel = state.panels[index];
        const auto x0 = std::max(0, static_cast<int>(panel.X));
        const auto y0 = std::max(0, static_cast<int>(panel.Y));
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

    for (int index = 0; index < state.panel_count; ++index) {
        const auto& panel = state.panels[index];
        Gdiplus::GraphicsPath clip;
        add_panel_path(clip, panel, state.panel_radius);
        g.SetClip(&clip);

        if (is_backlight_enabled())
            draw_backlight(g, state, panel);

        draw_scale(g, dial_of(state, panel), s, family.get());
        if (family)
            draw_face_text(g, state, index, *family);

        // 峰值燈（熄著的樣子）
        Lamp lamps[2];
        const auto lamp_count = lamps_of(state, index, lamps);
        const auto lr = lamp_radius(state);
        const Gdiplus::SolidBrush off(rgba(90, 20, 16, 0.85));
        for (int k = 0; k < lamp_count; ++k)
            g.FillEllipse(&off, lamps[k].at.X - lr, lamps[k].at.Y - lr, 2 * lr, 2 * lr);
        g.ResetClip();
    }
    state.static_dirty = false;
}

// ---- 每格 ----

// 指針：上粗下細，浮在錶面上方，影子往右下偏
void draw_needle(Gdiplus::Graphics& g, const Dial& m, double pos, float base_r, float tip_r, const Gdiplus::Color& colour,
    float s)
{
    const auto a = angle_of_pos(m, pos);
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
    const Gdiplus::SolidBrush needle_brush(colour);
    g.FillPolygon(&needle_brush, points, 4);
}

// 霧面半圓軸蓋（方框、雙指針：指針軸在錶框下緣外面，只露出上半）
void draw_half_cap(Gdiplus::Graphics& g, float cx, float cy, float rx, float ry, float s)
{
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
}

// 亮面軸心蓋（圓形錶頭：指針軸在錶面裡，整顆露出來；光從左上來）
void draw_hub(Gdiplus::Graphics& g, float cx, float cy, float r, float s)
{
    Gdiplus::GraphicsPath hub;
    hub.AddEllipse(cx - r, cy - r, 2 * r, 2 * r);
    Gdiplus::PathGradientBrush brush(&hub);
    brush.SetCenterPoint(Gdiplus::PointF(cx - 0.35f * r, cy - 0.4f * r));
    Gdiplus::Color colours[] = {rgba(28, 29, 32, 1), rgba(60, 62, 68, 1), rgba(150, 152, 160, 1)};
    Gdiplus::REAL positions[] = {0.f, 0.45f, 1.f};
    brush.SetInterpolationColors(colours, positions, 3);
    const Gdiplus::SolidBrush drop(rgba(0, 0, 0, 0.22));
    g.FillEllipse(&drop, cx - r + 1.5f * s, cy - r + 2.f * s, 2 * r, 2 * r);
    g.FillPath(&brush, &hub);
    Gdiplus::Pen rim(rgba(255, 255, 255, 0.25), 1.f * s);
    g.DrawEllipse(&rim, cx - r, cy - r, 2 * r, 2 * r);
}

void draw_lamp_lit(Gdiplus::Graphics& g, Gdiplus::PointF at, double v, float lr, float s)
{
    if (v > 0.001) {
        const auto glow_r = 13.f * s;
        Gdiplus::GraphicsPath glow_path;
        glow_path.AddEllipse(at.X - glow_r, at.Y - glow_r, 2 * glow_r, 2 * glow_r);
        Gdiplus::PathGradientBrush glow(&glow_path);
        glow.SetCenterPoint(at);
        glow.SetCenterColor(rgba(255, 70, 50, 0.55 * v));
        Gdiplus::Color surround = rgba(255, 70, 50, 0);
        int surround_count = 1;
        glow.SetSurroundColors(&surround, &surround_count);
        g.FillEllipse(&glow, at.X - glow_r, at.Y - glow_r, 2 * glow_r, 2 * glow_r);
        const Gdiplus::SolidBrush lit(
            rgba(255, static_cast<int>(60 + 80 * v), static_cast<int>(40 + 60 * v), 0.35 + 0.65 * v));
        g.FillEllipse(&lit, at.X - lr, at.Y - lr, 2 * lr, 2 * lr);
    }
    const Gdiplus::SolidBrush spark(rgba(255, 255, 255, 0.35));
    const auto spark_r = lr * 0.3f;
    g.FillEllipse(&spark, at.X - lr * 0.28f - spark_r, at.Y - lr * 0.33f - spark_r, 2 * spark_r, 2 * spark_r);
}

// 指針軸端與尖端離指針軸多遠（各樣式不同）
void needle_span(const State& state, const Gdiplus::RectF& panel, const Dial& m, float& base_r, float& tip_r)
{
    const auto s = state.scale;
    switch (state.style) {
    case Style::round:
        base_r = 0.f;
        tip_r = m.radius + 13.f * s;
        break;
    case Style::dual:
        base_r = m.cy - (panel.Y + panel.Height) + 2.f * s;
        tip_r = m.radius + 14.f * s;
        break;
    default:
        base_r = m.cy - (panel.Y + panel.Height) + 2.f * s;
        tip_r = m.radius + 13.f * s;
        break;
    }
}

RECT union_rect(const RECT& a, const RECT& b)
{
    if (IsRectEmpty(&a))
        return b;
    if (IsRectEmpty(&b))
        return a;
    RECT out{};
    UnionRect(&out, &a, &b);
    return out;
}

RECT panel_bounds(const Gdiplus::RectF& panel)
{
    return RECT{static_cast<LONG>(std::floor(panel.X)), static_cast<LONG>(std::floor(panel.Y)),
        static_cast<LONG>(std::ceil(panel.X + panel.Width)), static_cast<LONG>(std::ceil(panel.Y + panel.Height))};
}

// 這個錶框的指針（含影子、反鋸齒邊）現在涵蓋的範圍，限在錶框裡；雙指針是兩支合起來
RECT needle_rect(const State& state, int index)
{
    const auto& panel = state.panels[index];
    const auto m = dial_of(state, panel);
    float base_r{};
    float tip_r{};
    needle_span(state, panel, m, base_r, tip_r);
    const auto s = state.scale;
    RECT out{};
    const auto add = [&](double pos) {
        const auto a = angle_of_pos(m, pos);
        const auto b = point_at(m, a, base_r);
        const auto t = point_at(m, a, tip_r);
        const auto pad = 2.1f * s + 2.f;
        const RECT r{static_cast<LONG>(std::floor(std::min(b.X, t.X) - pad)),
            static_cast<LONG>(std::floor(std::min(b.Y, t.Y) - pad)),
            static_cast<LONG>(std::ceil(std::max(b.X, t.X) + pad + 3.84f * s)),
            static_cast<LONG>(std::ceil(std::max(b.Y, t.Y) + pad + 5.04f * s))};
        out = union_rect(out, r);
    };
    if (state.style == Style::dual) {
        add(state.needles[0].pos);
        add(state.needles[1].pos);
    } else {
        add(state.needles[index].pos);
    }
    const auto bounds = panel_bounds(panel);
    RECT clipped{};
    IntersectRect(&clipped, &out, &bounds);
    return clipped;
}

// 畫會動的部分（指針、峰值燈、軸蓋）到 content。region＝只重畫這一塊（先把那塊還原成不動的那層），空的＝整張
void draw_dynamic(State& state, const RECT* region = nullptr)
{
    const auto w = state.width;
    const auto h = state.height;
    if (!region) {
        state.content = state.base;
    } else {
        for (auto y = std::max(0L, region->top); y < std::min(static_cast<LONG>(h), region->bottom); ++y) {
            const auto left = std::max(0L, region->left);
            const auto right = std::min(static_cast<LONG>(w), region->right);
            if (right > left)
                memcpy(&state.content[static_cast<size_t>(y) * w + left], &state.base[static_cast<size_t>(y) * w + left],
                    static_cast<size_t>(right - left) * 4);
        }
    }
    Gdiplus::Bitmap canvas(w, h, w * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(state.content.data()));
    Gdiplus::Graphics g(&canvas);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    const auto s = state.scale;
    const auto white = rgba(250, 247, 240, 0.97);
    const auto amber = rgba(240, 176, 80, 0.97);

    for (int index = 0; index < state.panel_count; ++index) {
        const auto& panel = state.panels[index];
        if (region) {
            const auto bounds = panel_bounds(panel);
            RECT overlap{};
            if (!IntersectRect(&overlap, &bounds, region))
                continue;
        }
        const auto m = dial_of(state, panel);
        Gdiplus::GraphicsPath clip;
        add_panel_path(clip, panel, state.panel_radius);
        g.SetClip(&clip);
        if (region)
            g.SetClip(Gdiplus::Rect(region->left, region->top, region->right - region->left, region->bottom - region->top),
                Gdiplus::CombineModeIntersect);

        Lamp lamps[2];
        const auto lamp_count = lamps_of(state, index, lamps);
        const auto lr = lamp_radius(state);
        for (int k = 0; k < lamp_count; ++k)
            draw_lamp_lit(g, lamps[k].at, state.needles[lamps[k].channel].lamp, lr, s);

        float base_r{};
        float tip_r{};
        needle_span(state, panel, m, base_r, tip_r);
        switch (state.style) {
        case Style::round:
            draw_needle(g, m, state.needles[index].pos, base_r, tip_r, white, s);
            draw_hub(g, m.cx, m.cy, 13.f * s, s);
            break;
        case Style::dual: {
            // 右聲道（琥珀）先畫，左聲道（白）疊在上面
            const auto u = panel.Width / 600.f;
            draw_needle(g, m, state.needles[1].pos, base_r, tip_r, amber, s);
            draw_needle(g, m, state.needles[0].pos, base_r, tip_r, white, s);
            draw_half_cap(g, m.cx, panel.Y + panel.Height, 40.f * u, 24.f * u, s);
            break;
        }
        default:
            draw_needle(g, m, state.needles[index].pos, base_r, tip_r, white, s);
            draw_half_cap(g, m.cx, panel.Y + panel.Height, 30.f * s, 19.f * s, s);
            break;
        }
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

// 合成一塊（預乘 BGRA，視窗座標）：陰影＋玻璃錶框
void compose_region(const State& state, uint32_t* out, const RECT& region)
{
    const auto w = state.width;
    const auto h = state.height;
    const auto x_begin = std::max(0, static_cast<int>(region.left));
    const auto x_end = std::min(w, static_cast<int>(region.right));
    const auto y_begin = std::max(0, static_cast<int>(region.top));
    const auto y_end = std::min(h, static_cast<int>(region.bottom));
    if (x_end <= x_begin || y_end <= y_begin)
        return;

    // 陰影（黑色，預乘）
    for (int y = y_begin; y < y_end; ++y)
        memcpy(&out[static_cast<size_t>(y) * w + x_begin], &state.shadow_px[static_cast<size_t>(y) * w + x_begin],
            static_cast<size_t>(x_end - x_begin) * 4);

    // 玻璃
    for (int index = 0; index < state.panel_count; ++index) {
        const auto& panel = state.panels[index];
        const auto px0 = static_cast<int>(panel.X);
        const auto py0 = static_cast<int>(panel.Y);
        const auto ys = std::max(y_begin, py0);
        const auto ye = std::min(y_end, py0 + state.panel_h);
        const auto xs = std::max(x_begin, px0);
        const auto xe = std::min(x_end, px0 + state.panel_w);
        for (int wy = ys; wy < ye; ++wy) {
            const auto y = wy - py0;
            for (int wx = xs; wx < xe; ++wx) {
                const auto x = wx - px0;
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
}

// 整張合成一格（縮圖、測試輸出、需要整張重畫時用）
void compose(State& state, uint32_t* out)
{
    if (state.static_dirty)
        rebuild_static(state);
    draw_dynamic(state);
    compose_region(state, out, RECT{0, 0, state.width, state.height});
}

bool ensure_dib(State& state)
{
    if (state.dib && state.dib_w == state.width && state.dib_h == state.height)
        return true;
    state.dib.reset();
    state.memory_dc.reset();
    state.dib_bits = nullptr;
    state.full_redraw = true;

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

// 重畫有變的地方，傳回要更新的範圍。content_changed＝指針或燈動了（只是淡入的話不用重新合成）。
// 平常只重畫「上一格與這一格指針涵蓋的範圍」與峰值燈，玻璃折射會往外取樣，所以合成範圍再往外擴折射寬度
RECT redraw(State& state, uint32_t* out, bool content_changed)
{
    RECT dirty{};
    if (state.full_redraw || state.static_dirty) {
        compose(state, out);
        for (int index = 0; index < state.panel_count; ++index)
            state.needle_rects[index] = needle_rect(state, index);
        state.full_redraw = false;
        dirty = {0, 0, state.width, state.height};
    } else if (content_changed) {
        RECT regions[2 + 2 * 2]{};
        int count = 0;
        const auto glow = static_cast<LONG>(std::ceil(13.f * state.scale)) + 2;
        for (int index = 0; index < state.panel_count; ++index) {
            const auto now = needle_rect(state, index);
            regions[count++] = union_rect(now, state.needle_rects[index]);
            state.needle_rects[index] = now;
            Lamp lamps[2];
            const auto lamp_count = lamps_of(state, index, lamps);
            for (int k = 0; k < lamp_count; ++k) {
                const auto x = static_cast<LONG>(std::lround(lamps[k].at.X));
                const auto y = static_cast<LONG>(std::lround(lamps[k].at.Y));
                regions[count++] = RECT{x - glow, y - glow, x + glow, y + glow};
            }
        }
        for (int k = 0; k < count; ++k)
            draw_dynamic(state, &regions[k]);
        const auto spread = static_cast<LONG>(std::ceil(state.refraction)) + 2;
        const RECT window{0, 0, state.width, state.height};
        for (int k = 0; k < count; ++k) {
            RECT grown = regions[k];
            InflateRect(&grown, spread, spread);
            IntersectRect(&grown, &grown, &window);
            compose_region(state, out, grown);
            dirty = union_rect(dirty, grown);
        }
    }
    return dirty;
}

void present(HWND wnd, State& state, bool content_changed = true)
{
    if (state.width <= 0 || state.height <= 0 || !ensure_dib(state))
        return;
    auto dirty = redraw(state, state.dib_bits, content_changed);

    BYTE opacity = 255;
    if (state.fading) {
        const auto elapsed = GetTickCount64() - state.shown_at;
        if (elapsed >= fade_in_ms)
            state.fading = false;
        else
            opacity = static_cast<BYTE>(255 * elapsed / fade_in_ms);
        // 透明度變了，整張都要更新
        dirty = {0, 0, state.width, state.height};
    }

    const auto screen_dc = wil::GetDC(nullptr);
    SIZE size{state.width, state.height};
    POINT source{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, opacity, AC_SRC_ALPHA};
    UPDATELAYEREDWINDOWINFO info{};
    info.cbSize = sizeof(info);
    info.hdcDst = screen_dc.get();
    info.psize = &size;
    info.hdcSrc = state.memory_dc.get();
    info.pptSrc = &source;
    info.pblend = &blend;
    info.dwFlags = ULW_ALPHA;
    info.prcDirty = IsRectEmpty(&dirty) ? nullptr : &dirty;
    UpdateLayeredWindowIndirect(wnd, &info);
    state.needs_present = false;
}

// ---- 音量 ----

// 第幾首（換歌時加一；開發版的指針紀錄用來分段）
int track_serial{};

#if defined(NOCTURNE_BUILD_DEV)
// 開發版才有的指針動態紀錄（2026-11-12 使用者要觀察播歌時指針跳動是否符合預期）：
// 環境變數 NOCTURNE_VU_LOG＝CSV 檔路徑（當前綴用）時，從有訊號起每段記 60 秒每一格的原始音量、校準、目標值、指針位置、
// 峰值與峰值燈，存成「<路徑去掉 .csv>_t<第幾首>_p<指針設定>.csv」；換歌或換指針設定就結束這段、另開一段
// （2026-11-12 用三十幾首歌調指針與峰值燈時改成每首一段）。
// 只在段落結束時寫檔（中途不碰磁碟）；沒設環境變數就什麼都不做
struct NeedleLog {
    bool checked{};
    bool disabled{};
    std::wstring prefix;
    LARGE_INTEGER frequency{};
    LARGE_INTEGER start{};
    int preset{-1};
    int track{-1};
    bool finished{}; // 這個指針設定已經記完一段，等換設定才再記
    std::string rows;
};

void flush_needle_log(NeedleLog& log)
{
    if (log.rows.empty())
        return;
    wchar_t path[MAX_PATH]{};
    swprintf_s(path, L"%s_t%d_p%d.csv", log.prefix.c_str(), log.track, log.preset);
    FILE* file{};
    if (_wfopen_s(&file, path, L"w") == 0 && file) {
        fwrite(log.rows.data(), 1, log.rows.size(), file);
        fclose(file);
    }
    log.rows.clear();
}

void log_needles(const State& state, double dt, const double* rms, double reference, const double* targets,
    const double* peaks, bool live)
{
    static NeedleLog log;
    if (log.disabled)
        return;
    if (!log.checked) {
        log.checked = true;
        wchar_t path[MAX_PATH]{};
        if (!GetEnvironmentVariable(L"NOCTURNE_VU_LOG", path, MAX_PATH)) {
            log.disabled = true;
            return;
        }
        log.prefix = path;
        if (log.prefix.size() > 4 && _wcsicmp(log.prefix.c_str() + log.prefix.size() - 4, L".csv") == 0)
            log.prefix.resize(log.prefix.size() - 4);
        QueryPerformanceFrequency(&log.frequency);
    }
    const auto preset = static_cast<int>(get_needle_preset());
    if (preset != log.preset || track_serial != log.track) {
        flush_needle_log(log);
        log.preset = preset;
        log.track = track_serial;
        log.start.QuadPart = 0;
        log.finished = false;
    }
    if (log.finished || (!live && log.start.QuadPart == 0))
        return;
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (log.start.QuadPart == 0) {
        log.start = now;
        log.rows = "t_ms,dt_ms,live,style,preset,ref_db,rms_l,rms_r,target_l,pos_l,target_r,pos_r,peak_l,peak_r,lamp_l,lamp_r\n";
    }
    const auto t = (now.QuadPart - log.start.QuadPart) * 1000.0 / log.frequency.QuadPart;
    char line[320]{};
    sprintf_s(line, "%.1f,%.1f,%d,%d,%d,%.2f,%.5f,%.5f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.2f,%.2f\n", t, dt * 1000.0,
        live ? 1 : 0, static_cast<int>(state.style), preset, reference, rms[0], rms[1], targets[0], state.needles[0].pos,
        targets[1], state.needles[1].pos, peaks[0], peaks[1], state.needles[0].lamp, state.needles[1].lamp);
    log.rows += line;
    if (t < 60000.0)
        return;
    flush_needle_log(log);
    log.finished = true;
}
#endif

// 「自動」校準：把長時間平均音量（功率平均，低於 −45 dBFS 的安靜片段不算）對到 −2 VU，
// 0 VU 的位置限制在 −20～−4 dBFS。剛開始或換歌後前 3 秒用 1.5 秒時間常數快速找到位置，之後變大聲用 3 秒、變小聲用 8 秒跟，
// 所以一首歌裡的大小聲起伏還是看得出來。2026-11-12 用三首實錄離線模擬調的：
// 原本上下都 8 秒、對到 −3 VU 時，安靜前奏轉爆發的歌（Fighting My Way）轉折後好幾秒都偏高、峰值燈亮 19% 的時間；
// 改成變大聲 3 秒、對到 −2 VU 後三首中位都在 −2.6～−3.3 VU，那首降到 16.5%，壓得很滿的 nazonazo 每分鐘閃約 2 次
void update_auto_reference(State& state, double dt, const double* rms)
{
    const auto power = (rms[0] * rms[0] + rms[1] * rms[1]) / 2.0;
    if (power <= std::pow(10.0, auto_gate_dbfs / 10.0))
        return;
    const auto tau = state.auto_warm < auto_warm_seconds
        ? auto_fast_seconds
        : (state.auto_power > 0 && power > state.auto_power ? auto_rise_seconds : auto_slow_seconds);
    state.auto_power = state.auto_power <= 0 ? power : state.auto_power + (power - state.auto_power) * std::min(1.0, dt / tau);
    state.auto_warm += dt;
    state.auto_reference
        = std::clamp(10.0 * std::log10(state.auto_power) + auto_average_vu_offset, auto_reference_min, auto_reference_max);
}

// 讀這一格的音量，更新兩支指針；有動到要重畫就傳回 true
bool update_needles(State& state, double dt)
{
    double rms[2]{};
    double targets[2]{};
    double peaks[2]{};
    const auto& tuning = needle_tunings[static_cast<int>(get_needle_preset())];
    const auto playback = play_control::get();
    const auto live = playback->is_playing() && !playback->is_paused();
    if (live) {
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
                for (int side = 0; side < 2; ++side) {
                    const auto channel = std::min(side, channels - 1);
                    double sum = 0;
                    double peak = 0;
                    for (size_t n = 0; n < samples; ++n) {
                        const double value = data[n * channels + channel];
                        sum += value * value;
                        peak = std::max(peak, std::abs(value));
                    }
                    rms[side] = std::sqrt(sum / samples);
                    peaks[side] = peak;
                }
                if (tuning.automatic)
                    update_auto_reference(state, dt, rms);
            }
        }
    }

    const auto reference = tuning.automatic ? state.auto_reference : tuning.reference_dbfs;
    const auto gain = std::pow(10.0, -reference / 20.0) / std::pow(10.0, scale_top_vu / 20.0);
    // 這一格的功率放進環狀紀錄，指針用最近 needle_frames 格、峰值燈用最近 auto_lamp_frames 格的平均
    state.power_index = (state.power_index + 1) % power_history;
    double lamp_rms[2]{};
    for (int side = 0; side < 2; ++side) {
        state.power_history_[side][state.power_index] = rms[side] * rms[side];
        const auto mean_of = [&](int frames) {
            double sum = 0;
            for (int k = 0; k < frames; ++k)
                sum += state.power_history_[side][(state.power_index - k + power_history) % power_history];
            return std::sqrt(sum / frames);
        };
        targets[side] = mean_of(needle_frames) * gain;
        lamp_rms[side] = mean_of(auto_lamp_frames);
    }

    const auto lamp_threshold = std::pow(10.0, peak_lamp_dbfs / 20.0);
    // 暫停／停止時沒有訊號：改用比較慢、阻尼小一點的回落（約 1.1 秒），像錶頭的彈簧把指針慢慢拉回去
    // （2026-11-11 使用者說暫停時指針「沒緩衝地」直接回 0，不夠真實）
    const auto omega = live ? tuning.natural_frequency : rest_frequency;
    const auto zeta = live ? tuning.damping_ratio : rest_damping_ratio;
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
                = omega * omega * (target - needle.pos) - 2.0 * zeta * omega * needle.velocity;
            needle.velocity += acceleration * h;
            needle.pos += needle.velocity * h;
            // 碰到兩端的擋針會小彈一下（反彈三成），速度很小就停住
            if (needle.pos < stop_low) {
                needle.pos = stop_low;
                needle.velocity = needle.velocity < -0.05 ? -needle.velocity * stop_bounce : std::max(0.0, needle.velocity);
            } else if (needle.pos > stop_high) {
                needle.pos = stop_high;
                needle.velocity = needle.velocity > 0.05 ? -needle.velocity * stop_bounce : std::min(0.0, needle.velocity);
            }
        }
        const auto lamp_on = tuning.automatic
            ? lamp_rms[side] > 0 && 20.0 * std::log10(lamp_rms[side]) - reference >= auto_lamp_vu
            : peaks[side] >= lamp_threshold;
        if (lamp_on) {
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
#if defined(NOCTURNE_BUILD_DEV)
    log_needles(state, dt, rms, reference, targets, peaks, live);
#endif
    return changed;
}

#if defined(NOCTURNE_BUILD_DEV)
// ---- 開發版才有的測試輸出 ----
// 環境變數 NOCTURNE_VU_DUMP＝一個資料夾時，第一次重截背景後把三種樣式各用目前展開頁大小的 1、1.5、2 倍
// （1080p 的展開頁放大成約 1440p／2K、4K）畫在放大後的背景上存成 PNG，並在主控台記每格合成要幾毫秒。
// 用來確認高解析度下的清晰度與效能（2026-11-12 使用者要求確認 2K、4K）；沒設環境變數就什麼都不做
bool save_png(const std::vector<uint32_t>& pixels, int w, int h, const std::wstring& path)
{
    UINT count{};
    UINT bytes{};
    if (Gdiplus::GetImageEncodersSize(&count, &bytes) != Gdiplus::Ok || bytes == 0)
        return false;
    std::vector<BYTE> buffer(bytes);
    auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
    if (Gdiplus::GetImageEncoders(count, bytes, encoders) != Gdiplus::Ok)
        return false;
    for (UINT i = 0; i < count; ++i) {
        if (wcscmp(encoders[i].MimeType, L"image/png") != 0)
            continue;
        auto copy = pixels;
        Gdiplus::Bitmap bitmap(w, h, w * 4, PixelFormat32bppRGB, reinterpret_cast<BYTE*>(copy.data()));
        return bitmap.Save(path.c_str(), &encoders[i].Clsid, nullptr) == Gdiplus::Ok;
    }
    return false;
}

void debug_dump(HWND wnd)
{
    static bool done = false;
    wchar_t folder[MAX_PATH]{};
    if (done || !GetEnvironmentVariable(L"NOCTURNE_VU_DUMP", folder, MAX_PATH))
        return;
    done = true;

    // 截背景時先把自己藏起來，不然別的樣式取背景那一圈會截到現在這個錶
    ShowWindow(wnd, SW_HIDE);
    std::vector<uint32_t> stage;
    int stage_w{};
    int stage_h{};
    RECT stage_screen{};
    const auto ok = read_stage_pixels(stage, stage_w, stage_h, stage_screen);
    ShowWindow(wnd, SW_SHOWNA);
    // 藏起來時 WM_SHOWWINDOW 會把指針的計時器關掉，要開回來（2026-11-12 第一次測時指針因此停住）
    SetTimer(wnd, frame_timer_id, frame_interval_ms, nullptr);
    if (!ok)
        return;

    for (const auto factor : {1.0, 1.5, 2.0}) {
        // 展開頁整張放大（雙線性；封面虛化背景本來就很平滑）
        const auto big_w = static_cast<int>(std::lround(stage_w * factor));
        const auto big_h = static_cast<int>(std::lround(stage_h * factor));
        std::vector<uint32_t> big(static_cast<size_t>(big_w) * big_h);
        for (int y = 0; y < big_h; ++y)
            for (int x = 0; x < big_w; ++x)
                big[static_cast<size_t>(y) * big_w + x] = sample_bilinear(stage, stage_w, stage_h,
                    static_cast<float>((x + 0.5) / factor - 0.5), static_cast<float>((y + 0.5) / factor - 0.5));

        // 版面標記裡的固定像素（封面間距、右邊界）也要跟著放大，所以用「假的 DPI」不方便；
        // 這裡直接把 1 倍算好的位置放大，等同整個展開頁（含 DPI）放大 factor 倍
        for (int style_index = 0; style_index < style_count; ++style_index) {
            const auto style = static_cast<Style>(style_index);
            const auto plan = window_plan_in_stage(style, stage_w, stage_h);
            if (!plan)
                continue;
            const RECT rect{static_cast<LONG>(std::lround(plan->rect.left * factor)),
                static_cast<LONG>(std::lround(plan->rect.top * factor)),
                static_cast<LONG>(std::lround(plan->rect.right * factor)),
                static_cast<LONG>(std::lround(plan->rect.bottom * factor))};
            State test;
            test.style = style;
            rebuild_geometry(test, rect.right - rect.left, rect.bottom - rect.top, plan->scale * static_cast<float>(factor));
            fill_backdrop(test, big, big_w, big_h, static_cast<int>(rect.left), static_cast<int>(rect.top));
            test.needles[0].pos = 0.62;
            test.needles[1].pos = 0.48;
            test.needles[1].lamp = 1.0;

            std::vector<uint32_t> frame(static_cast<size_t>(test.width) * test.height);
            compose(test, frame.data());
            LARGE_INTEGER frequency{};
            LARGE_INTEGER start{};
            LARGE_INTEGER end{};
            QueryPerformanceFrequency(&frequency);
            constexpr int runs = 30;
            QueryPerformanceCounter(&start);
            for (int run = 0; run < runs; ++run)
                compose(test, frame.data());
            QueryPerformanceCounter(&end);
            const auto ms = (end.QuadPart - start.QuadPart) * 1000.0 / frequency.QuadPart / runs;

            // 平常每格只重畫指針動到的地方：模擬指針一格一格掃過去（左右不同步、峰值燈閃）量平均時間
            test.full_redraw = true;
            redraw(test, frame.data(), true);
            constexpr int frame_runs = 120;
            QueryPerformanceCounter(&start);
            for (int run = 0; run < frame_runs; ++run) {
                test.needles[0].pos = 0.35 + 0.5 * std::abs(std::sin(run * 0.11));
                test.needles[1].pos = 0.30 + 0.5 * std::abs(std::sin(run * 0.07 + 1.0));
                test.needles[run % 2].lamp = (run / 10) % 2 ? 1.0 : 0.0;
                redraw(test, frame.data(), true);
            }
            QueryPerformanceCounter(&end);
            const auto frame_ms = (end.QuadPart - start.QuadPart) * 1000.0 / frequency.QuadPart / frame_runs;
            // 檢查：只重畫局部的結果要跟整張重畫一模一樣（容許 1 的四捨五入差）
            std::vector<uint32_t> reference(frame.size());
            test.full_redraw = true;
            redraw(test, reference.data(), true);
            int max_diff = 0;
            for (size_t i = 0; i < frame.size(); ++i)
                for (int shift = 0; shift <= 24; shift += 8)
                    max_diff = std::max(max_diff,
                        std::abs(static_cast<int>((frame[i] >> shift) & 0xFF) - static_cast<int>((reference[i] >> shift) & 0xFF)));

            // 疊回放大後的展開頁那一塊再存
            std::vector<uint32_t> shot(frame.size());
            for (int y = 0; y < test.height; ++y)
                for (int x = 0; x < test.width; ++x) {
                    const auto src = frame[static_cast<size_t>(y) * test.width + x];
                    const auto under = big[static_cast<size_t>(std::clamp(static_cast<int>(rect.top) + y, 0, big_h - 1)) * big_w
                        + std::clamp(static_cast<int>(rect.left) + x, 0, big_w - 1)];
                    const auto a = (src >> 24) / 255.f;
                    uint32_t out = 0xFF000000;
                    for (int shift = 0; shift <= 16; shift += 8) {
                        const auto c = ((src >> shift) & 0xFF) + ((under >> shift) & 0xFF) * (1.f - a);
                        out |= static_cast<uint32_t>(std::clamp(c, 0.f, 255.f) + 0.5f) << shift;
                    }
                    shot[static_cast<size_t>(y) * test.width + x] = out;
                }
            wchar_t name[MAX_PATH]{};
            swprintf_s(name, L"%s\\vu_style%d_x%.1f.png", folder, style_index, factor);
            const auto saved = save_png(shot, test.width, test.height, name);
            console::formatter log;
            log << "VU dump: style " << style_index << " x" << pfc::format_float(factor, 0, 1) << " stage " << big_w << "x"
                                 << big_h << " window " << test.width << "x" << test.height << " compose "
                                 << pfc::format_float(ms, 0, 2) << " ms, per frame (partial) "
                                 << pfc::format_float(frame_ms, 0, 2) << " ms, partial vs full max diff " << max_diff
                                 << (saved ? "" : " (PNG not saved)");
            // 同一行也記到資料夾裡的 vu_timing.txt（主控台要開設定才看得到）
            wchar_t timing_path[MAX_PATH]{};
            swprintf_s(timing_path, L"%s\\vu_timing.txt", folder);
            FILE* timing{};
            if (_wfopen_s(&timing, timing_path, L"a") == 0 && timing) {
                fprintf(timing, "%s\n", log.get_ptr());
                fclose(timing);
            }
        }
    }
}
#endif

// ---- 右鍵樣式挑選卡（2026-11-12）----
// 使用者說不要放設定頁，要在 VU 表上按右鍵、跳出「我們自己的視覺化調整」（不是 ESLyric 的右鍵選單）：
// 深色卡片，上排三格縮圖（拿目前的虛化背景、用同一套程式實際畫出每種樣式），下排暖色背光開關；
// 點縮圖馬上換、卡片留著可以比較；點卡片外面或按 Esc 關
namespace picker {

constexpr wchar_t class_name[] = L"columns_ui_vu_style_picker";
constexpr float shadow_dip = 18.f;
constexpr float pad_dip = 16.f;
constexpr float title_h_dip = 34.f;
constexpr float tile_w_dip = 168.f;
constexpr float tile_h_dip = 96.f;
constexpr float tile_gap_dip = 12.f;
constexpr float label_h_dip = 30.f;
constexpr float row_h_dip = 40.f;
constexpr float radius_dip = 14.f;
constexpr int backlight_hit = 50;
constexpr int needle_hit_base = 100; // 指針設定那排：100＋第幾個
constexpr float section_h_dip = 26.f;
constexpr float chip_h_dip = 46.f;
constexpr float chip_gap_dip = 8.f;
// 縮圖由左到右的順序（2026-11-12 使用者要單錶雙指針跟圓形互換位置）；存的設定值仍是 Style 的數字
constexpr Style display_order[style_count] = {Style::box, Style::dual, Style::round};

struct Picker {
    HWND wnd{};
    HWND meter{};
    float k{1.f}; // DPI 倍率
    int width{};
    int height{};
    Gdiplus::RectF card;
    Gdiplus::RectF tiles[style_count];
    float divider_y{};
    Gdiplus::RectF chips[needle_preset_count];
    float divider2_y{};
    Gdiplus::RectF backlight_row;
    int thumb_w{};
    int thumb_h{};
    std::vector<uint32_t> thumbs[style_count];
    std::vector<float> shadow;
    int hover{-1};
    bool tracking{};
};

std::unique_ptr<Picker>& current()
{
    static std::unique_ptr<Picker> picker;
    return picker;
}

// 一種樣式的縮圖（不透明 BGRA）：底是目前錶後面的虛化背景（拉成縮圖大小），錶照展開頁的比例縮小置中
std::vector<uint32_t> render_thumbnail(Style style, int tw, int th, const State* live)
{
    std::vector<uint32_t> out(static_cast<size_t>(tw) * th, 0xFF1A1C20);
    if (!live || live->width <= 0 || live->height <= 0 || live->backdrop.empty())
        return out;
    for (int y = 0; y < th; ++y)
        for (int x = 0; x < tw; ++x)
            out[static_cast<size_t>(y) * tw + x] = sample_bilinear(live->backdrop, live->width, live->height,
                (x + 0.5f) * live->width / tw - 0.5f, (y + 0.5f) * live->height / th - 0.5f);

    const auto plan = window_plan_in_stage(style, live->bar_rect.right - live->bar_rect.left, live->bar_rect.top);
    if (!plan)
        return out;
    const auto w = static_cast<float>(plan->rect.right - plan->rect.left);
    const auto h = static_cast<float>(plan->rect.bottom - plan->rect.top);
    if (w <= 0 || h <= 0)
        return out;
    const auto f = std::min(tw / w, th / h);
    State test;
    test.style = style;
    rebuild_geometry(test, std::max(1, static_cast<int>(std::lround(w * f))),
        std::max(1, static_cast<int>(std::lround(h * f))), plan->scale * f);
    const auto ox = (tw - test.width) / 2;
    const auto oy = (th - test.height) / 2;
    for (int y = 0; y < test.height; ++y)
        for (int x = 0; x < test.width; ++x)
            test.backdrop[static_cast<size_t>(y) * test.width + x]
                = out[static_cast<size_t>(std::clamp(oy + y, 0, th - 1)) * tw + std::clamp(ox + x, 0, tw - 1)];
    test.has_backdrop = true;
    test.static_dirty = true;
    test.needles[0].pos = 0.66;
    test.needles[1].pos = 0.54;

    std::vector<uint32_t> frame(static_cast<size_t>(test.width) * test.height);
    compose(test, frame.data());
    for (int y = 0; y < test.height; ++y)
        for (int x = 0; x < test.width; ++x) {
            const auto tx = ox + x;
            const auto ty = oy + y;
            if (tx < 0 || ty < 0 || tx >= tw || ty >= th)
                continue;
            const auto src = frame[static_cast<size_t>(y) * test.width + x];
            auto& dst = out[static_cast<size_t>(ty) * tw + tx];
            const auto a = (src >> 24) / 255.f;
            uint32_t mixed = 0xFF000000;
            for (int shift = 0; shift <= 16; shift += 8) {
                const auto c = ((src >> shift) & 0xFF) + ((dst >> shift) & 0xFF) * (1.f - a);
                mixed |= static_cast<uint32_t>(std::clamp(c, 0.f, 255.f) + 0.5f) << shift;
            }
            dst = mixed;
        }
    return out;
}

void build_thumbnails(Picker& p)
{
    const auto* live = state_of(p.meter);
    for (int i = 0; i < style_count; ++i)
        p.thumbs[i] = render_thumbnail(display_order[i], p.thumb_w, p.thumb_h, live);
}

void build_layout(Picker& p)
{
    const auto k = p.k;
    const auto px = [k](float dip) { return std::round(dip * k); };
    const auto sh = px(shadow_dip);
    const auto pad = px(pad_dip);
    p.thumb_w = static_cast<int>(px(tile_w_dip));
    p.thumb_h = static_cast<int>(px(tile_h_dip));
    const auto gap = px(tile_gap_dip);
    const auto card_w = 2 * pad + style_count * p.thumb_w + (style_count - 1) * gap;
    const auto card_h = px(title_h_dip) + p.thumb_h + px(label_h_dip) + px(4) + px(section_h_dip) + px(chip_h_dip)
        + px(14) + px(row_h_dip) + px(10);
    p.card = {sh, sh - px(4), card_w, card_h};
    p.width = static_cast<int>(card_w + 2 * sh);
    p.height = static_cast<int>(card_h + 2 * sh);
    for (int i = 0; i < style_count; ++i)
        p.tiles[i] = {p.card.X + pad + i * (p.thumb_w + gap), p.card.Y + px(title_h_dip),
            static_cast<float>(p.thumb_w), static_cast<float>(p.thumb_h)};
    p.divider_y = p.tiles[0].Y + p.thumb_h + px(label_h_dip) + px(4);
    // 指針設定：小標題＋一排四個按鈕（跟縮圖同寬）
    const auto chips_y = p.divider_y + px(section_h_dip);
    const auto chip_gap = px(chip_gap_dip);
    const auto inner_w = card_w - 2 * pad;
    const auto chip_w = std::floor((inner_w - (needle_preset_count - 1) * chip_gap) / needle_preset_count);
    for (int i = 0; i < needle_preset_count; ++i)
        p.chips[i] = {p.card.X + pad + i * (chip_w + chip_gap), chips_y, chip_w, px(chip_h_dip)};
    p.divider2_y = chips_y + px(chip_h_dip) + px(10);
    p.backlight_row = {p.card.X + px(8), p.divider2_y + px(5), card_w - px(16), px(row_h_dip) - px(6)};

    // 陰影：卡片形狀往下 6、模糊
    std::vector<float> mask(static_cast<size_t>(p.width) * p.height, 0.f);
    const auto hw = card_w / 2.f;
    const auto hh = card_h / 2.f;
    const auto cx = p.card.X + hw;
    const auto cy = p.card.Y + hh + px(6);
    for (int y = 0; y < p.height; ++y)
        for (int x = 0; x < p.width; ++x)
            mask[static_cast<size_t>(y) * p.width + x]
                = std::clamp(0.5f - rounded_rect_sdf(x + 0.5f - cx, y + 0.5f - cy, hw, hh, px(radius_dip)), 0.f, 1.f);
    box_blur(mask, p.width, p.height, std::max(1, static_cast<int>(px(7))));
    for (auto& value : mask)
        value *= 0.55f;
    p.shadow = std::move(mask);
}

int hit_test(const Picker& p, POINT pt)
{
    const auto fx = static_cast<float>(pt.x);
    const auto fy = static_cast<float>(pt.y);
    for (int i = 0; i < style_count; ++i) {
        const auto& t = p.tiles[i];
        const auto half_gap = tile_gap_dip * p.k / 2.f;
        if (fx >= t.X - half_gap && fx < t.X + t.Width + half_gap && fy >= t.Y - 4.f * p.k
            && fy < t.Y + t.Height + label_h_dip * p.k)
            return i;
    }
    for (int i = 0; i < needle_preset_count; ++i) {
        const auto& c = p.chips[i];
        if (fx >= c.X && fx < c.X + c.Width && fy >= c.Y && fy < c.Y + c.Height)
            return needle_hit_base + i;
    }
    const auto& r = p.backlight_row;
    if (fx >= r.X && fx < r.X + r.Width && fy >= r.Y && fy < r.Y + r.Height)
        return backlight_hit;
    return -1;
}

void render(Picker& p)
{
    const auto w = p.width;
    const auto h = p.height;
    const auto k = p.k;
    std::vector<uint32_t> pixels(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < pixels.size(); ++i)
        pixels[i] = static_cast<uint32_t>(p.shadow[i] * 255.f + 0.5f) << 24;

    {
        Gdiplus::Bitmap canvas(w, h, w * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(pixels.data()));
        Gdiplus::Graphics g(&canvas);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

        // 卡片：深色、上緣一點光澤、細白邊
        Gdiplus::GraphicsPath card_path;
        add_panel_path(card_path, p.card, radius_dip * k);
        const Gdiplus::SolidBrush card_fill(rgba(24, 25, 29, 0.97));
        g.FillPath(&card_fill, &card_path);
        {
            Gdiplus::LinearGradientBrush sheen(Gdiplus::PointF(0, p.card.Y), Gdiplus::PointF(0, p.card.Y + 60.f * k),
                rgba(255, 255, 255, 0.05), rgba(255, 255, 255, 0));
            g.SetClip(&card_path);
            g.FillRectangle(&sheen, p.card.X, p.card.Y, p.card.Width, 60.f * k);
            g.ResetClip();
        }
        const Gdiplus::Pen card_border(rgba(255, 255, 255, 0.10), 1.f * k);
        g.DrawPath(&card_border, &card_path);

        std::unique_ptr<Gdiplus::FontFamily> family = theme_family();
        if (!family)
            family.reset(Gdiplus::FontFamily::GenericSansSerif()->Clone());
        const Gdiplus::Font title_font(family.get(), 13.f * k, regular_style(*family), Gdiplus::UnitPixel);
        const Gdiplus::Font label_font(family.get(), 14.f * k, regular_style(*family), Gdiplus::UnitPixel);
        const Gdiplus::Font label_bold(family.get(), 14.f * k, bold_style(*family), Gdiplus::UnitPixel);

        draw_text(g, nocturne_text::vu_picker_title, title_font, rgba(244, 240, 230, 0.55),
            {p.card.X + pad_dip * k, p.card.Y + title_h_dip * k / 2.f}, Gdiplus::StringAlignmentNear, k);

        int selected = 0;
        for (int i = 0; i < style_count; ++i)
            if (display_order[i] == get_style())
                selected = i;
        for (int i = 0; i < style_count; ++i) {
            const auto& t = p.tiles[i];
            Gdiplus::GraphicsPath tile_path;
            add_panel_path(tile_path, t, 9.f * k);
            g.SetClip(&tile_path);
            if (!p.thumbs[i].empty()) {
                Gdiplus::Bitmap thumb(p.thumb_w, p.thumb_h, p.thumb_w * 4, PixelFormat32bppPARGB,
                    reinterpret_cast<BYTE*>(p.thumbs[i].data()));
                g.DrawImage(&thumb, t.X, t.Y, t.Width, t.Height);
            }
            // 沒選的縮圖壓暗一點，選中的那張最亮
            if (i != selected) {
                const Gdiplus::SolidBrush dim(rgba(0, 0, 0, i == p.hover ? 0.10 : 0.28));
                g.FillRectangle(&dim, t);
            }
            g.ResetClip();

            if (i == selected) {
                Gdiplus::GraphicsPath ring;
                const auto o = 2.f * k;
                add_panel_path(ring, {t.X - o, t.Y - o, t.Width + 2 * o, t.Height + 2 * o}, 9.f * k + o);
                const Gdiplus::Pen pen(rgba(240, 190, 120, 0.95), 2.f * k);
                g.DrawPath(&pen, &ring);
            } else {
                const Gdiplus::Pen pen(rgba(255, 255, 255, i == p.hover ? 0.40 : 0.12), 1.f * k);
                g.DrawPath(&pen, &tile_path);
            }
            draw_text(g, nocturne_text::vu_style_names[static_cast<int>(display_order[i])], i == selected ? label_bold : label_font,
                i == selected ? rgba(244, 240, 230, 0.96) : rgba(244, 240, 230, i == p.hover ? 0.85 : 0.58),
                {t.X + t.Width / 2.f, t.Y + t.Height + label_h_dip * k / 2.f}, Gdiplus::StringAlignmentCenter, k);
        }

        const Gdiplus::Pen divider(rgba(255, 255, 255, 0.08), 1.f * k);
        g.DrawLine(&divider, p.card.X + pad_dip * k, p.divider_y, p.card.X + p.card.Width - pad_dip * k, p.divider_y);

        // 指針設定：小標題＋四個按鈕（名稱＋一行小字說明），選中的暖色框
        draw_text(g, nocturne_text::vu_needle_title, title_font, rgba(244, 240, 230, 0.55),
            {p.card.X + pad_dip * k, p.divider_y + section_h_dip * k / 2.f + 1.f * k}, Gdiplus::StringAlignmentNear, k);
        const Gdiplus::Font detail_font(family.get(), 11.f * k, regular_style(*family), Gdiplus::UnitPixel);
        const auto chosen = static_cast<int>(get_needle_preset());
        for (int i = 0; i < needle_preset_count; ++i) {
            const auto& c = p.chips[i];
            const auto hovered = p.hover == needle_hit_base + i;
            Gdiplus::GraphicsPath chip_path;
            add_panel_path(chip_path, c, 10.f * k);
            const Gdiplus::SolidBrush chip_fill(
                i == chosen ? rgba(240, 190, 120, 0.14) : rgba(255, 255, 255, hovered ? 0.07 : 0.035));
            g.FillPath(&chip_fill, &chip_path);
            const Gdiplus::Pen chip_pen(i == chosen ? rgba(240, 190, 120, 0.95) : rgba(255, 255, 255, hovered ? 0.30 : 0.10),
                (i == chosen ? 1.5f : 1.f) * k);
            g.DrawPath(&chip_pen, &chip_path);
            draw_text(g, nocturne_text::vu_needle_names[i], i == chosen ? label_bold : label_font,
                i == chosen ? rgba(244, 240, 230, 0.96) : rgba(244, 240, 230, hovered ? 0.85 : 0.65),
                {c.X + c.Width / 2.f, c.Y + 16.f * k}, Gdiplus::StringAlignmentCenter, k);
            draw_text(g, nocturne_text::vu_needle_details[i], detail_font,
                rgba(244, 240, 230, i == chosen ? 0.70 : 0.42), {c.X + c.Width / 2.f, c.Y + 33.f * k},
                Gdiplus::StringAlignmentCenter, k);
        }
        g.DrawLine(&divider, p.card.X + pad_dip * k, p.divider2_y, p.card.X + p.card.Width - pad_dip * k, p.divider2_y);

        // 暖色背光：開關＋文字，整行都可以點
        const auto& row = p.backlight_row;
        if (p.hover == backlight_hit) {
            Gdiplus::GraphicsPath row_path;
            add_panel_path(row_path, row, 8.f * k);
            const Gdiplus::SolidBrush row_fill(rgba(255, 255, 255, 0.05));
            g.FillPath(&row_fill, &row_path);
        }
        const auto on = is_backlight_enabled();
        const auto cy = row.Y + row.Height / 2.f;
        const Gdiplus::RectF pill{row.X + (pad_dip - 8.f) * k, cy - 9.f * k, 34.f * k, 18.f * k};
        Gdiplus::GraphicsPath pill_path;
        add_panel_path(pill_path, pill, 9.f * k);
        const Gdiplus::SolidBrush pill_fill(on ? rgba(232, 170, 96, 0.95) : rgba(255, 255, 255, 0.16));
        g.FillPath(&pill_fill, &pill_path);
        const auto knob_r = 7.f * k;
        const auto knob_x = on ? pill.X + pill.Width - 2.f * k - knob_r : pill.X + 2.f * k + knob_r;
        const Gdiplus::SolidBrush knob(rgba(250, 248, 244, 0.97));
        g.FillEllipse(&knob, knob_x - knob_r, cy - knob_r, 2 * knob_r, 2 * knob_r);
        draw_text(g, nocturne_text::vu_backlight, label_font, rgba(244, 240, 230, 0.85),
            {pill.X + pill.Width + 10.f * k, cy}, Gdiplus::StringAlignmentNear, k);
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = -h;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    const auto screen_dc = wil::GetDC(nullptr);
    void* bits{};
    wil::unique_hbitmap dib(CreateDIBSection(screen_dc.get(), &info, DIB_RGB_COLORS, &bits, nullptr, 0));
    if (!dib || !bits)
        return;
    memcpy(bits, pixels.data(), pixels.size() * 4);
    wil::unique_hdc memory_dc(CreateCompatibleDC(screen_dc.get()));
    const auto old = SelectObject(memory_dc.get(), dib.get());
    SIZE size{w, h};
    POINT source{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(p.wnd, screen_dc.get(), nullptr, &size, memory_dc.get(), &source, 0, &blend, ULW_ALPHA);
    SelectObject(memory_dc.get(), old);
}

LRESULT CALLBACK window_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
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
        if (hit >= 0 && hit < style_count) {
            if (display_order[hit] != get_style())
                set_style(display_order[hit]);
            render(p);
        } else if (hit >= needle_hit_base && hit < needle_hit_base + needle_preset_count) {
            set_needle_preset(static_cast<NeedlePreset>(hit - needle_hit_base));
            render(p);
        } else if (hit == backlight_hit) {
            set_backlight_enabled(!is_backlight_enabled());
            build_thumbnails(p);
            render(p);
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE)
            PostMessage(wnd, WM_CLOSE, 0, 0);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE)
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
void open(HWND meter, POINT at)
{
    close();
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
        return;

    auto p = std::make_unique<Picker>();
    p->meter = meter;
    p->k = static_cast<float>(uih::scale_dpi_value(1000)) / 1000.f;
    build_layout(*p);
    build_thumbnails(*p);

    const auto sh = static_cast<int>(std::round(shadow_dip * p->k));
    auto x = at.x - sh;
    auto y = at.y - sh;
    MONITORINFO monitor{sizeof(monitor)};
    if (GetMonitorInfo(MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST), &monitor)) {
        const auto& work = monitor.rcWork;
        x = std::clamp(x, work.left - sh, std::max(work.left - sh, work.right + sh - p->width));
        y = std::clamp(y, work.top - sh, std::max(work.top - sh, work.bottom + sh - p->height));
    }

    const auto owner = GetAncestor(meter, GA_ROOT);
    current() = std::move(p);
    const auto wnd = CreateWindowEx(WS_EX_LAYERED | WS_EX_TOOLWINDOW, class_name, L"", WS_POPUP, x, y,
        current()->width, current()->height, owner, nullptr, core_api::get_my_instance(), nullptr);
    if (!wnd) {
        current().reset();
        return;
    }
    current()->wnd = wnd;
    render(*current());
    ShowWindow(wnd, SW_SHOW);
    SetForegroundWindow(wnd);
    SetFocus(wnd);
}

} // namespace picker

// 點在哪個錶框裡（視窗用戶區座標）：只有錶框本身接滑鼠，陰影與空白照常落到底下的 ESLyric
bool hit_panel(const State& state, POINT pt)
{
    for (int index = 0; index < state.panel_count; ++index) {
        const auto& panel = state.panels[index];
        const auto hw = panel.Width / 2.f;
        const auto hh = panel.Height / 2.f;
        const auto d = rounded_rect_sdf(pt.x + 0.5f - (panel.X + hw), pt.y + 0.5f - (panel.Y + hh), hw, hh,
            std::min({state.panel_radius, hw, hh}));
        if (d <= 0.f)
            return true;
    }
    return false;
}

// 錶框上的滾輪轉給底下的 ESLyric（跟沒有錶時一樣）
void forward_wheel(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    struct Search {
        HWND self;
        POINT point;
        HWND found;
    } search{wnd, {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, nullptr};
    EnumChildWindows(
        GetParent(wnd),
        [](HWND child, LPARAM data) -> BOOL {
            auto& s = *reinterpret_cast<Search*>(data);
            if (child == s.self || !IsWindowVisible(child))
                return TRUE;
            char name[64]{};
            GetClassNameA(child, name, static_cast<int>(std::size(name)));
            RECT rect{};
            GetWindowRect(child, &rect);
            if (strcmp(name, "uie_eslyric_wnd_class") != 0 || !PtInRect(&rect, s.point))
                return TRUE;
            s.found = child;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&search));
    if (search.found)
        SendMessage(search.found, msg, wp, lp);
}

// ---- 視窗 ----

void schedule_recapture(HWND wnd);

void apply_layout(HWND wnd)
{
    auto* state = state_of(wnd);
    if (!state)
        return;

    const auto style = cached_style();
    const auto target = compute_window_rect(state->bar_rect, style);
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
    const auto restyled = state->style != style;
    const auto moved = !EqualRect(&state->window_rect, &target) || restyled;

    if (!visible || moved) {
        // 先藏起來再截背景，免得截到自己
        if (visible)
            ShowWindow(wnd, SW_HIDE);
        state->window_rect = target;
        state->style = style;
        if (width != state->width || height != state->height || moved) {
            const auto plan = window_plan_in_stage(
                style, state->bar_rect.right - state->bar_rect.left, state->bar_rect.top);
            rebuild_geometry(*state, width, height, plan ? plan->scale : 1.f);
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
        if (const auto& open_picker = picker::current(); open_picker && open_picker->meter == wnd)
            picker::close();
        states().erase(wnd);
        break;
    }
    case WM_NCHITTEST: {
        // 錶框本身接滑鼠（右鍵開樣式挑選卡，2026-11-12），錶框外的陰影與空白不擋、照常落到底下的 ESLyric
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(wnd, &pt);
        if (const auto* state = state_of(wnd); state && hit_panel(*state, pt))
            return HTCLIENT;
        return HTTRANSPARENT;
    }
    case WM_RBUTTONUP: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ClientToScreen(wnd, &pt);
        picker::open(wnd, pt);
        return 0;
    }
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_CONTEXTMENU:
        // 錶框上的左鍵不做事（VU 模式底下沒有歌詞可點），右鍵只開我們的卡片、不出 ESLyric 的選單
        return 0;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        forward_wheel(wnd, msg, wp, lp);
        return 0;
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
                present(wnd, *state, changed || state->needs_present);
            return 0;
        }
        if (wp == recapture_timer_id || wp == recapture_late_timer_id) {
            KillTimer(wnd, wp);
            recapture(wnd);
#if defined(NOCTURNE_BUILD_DEV)
            if (wp == recapture_late_timer_id)
                debug_dump(wnd);
#endif
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
        // 換歌：「自動」校準重新快速找位置（保留上一首的平均當起點，同一張專輯不會跳）
        for (const auto& [wnd, state] : states())
            state->auto_warm = 0;
        ++track_serial;
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

NeedlePreset get_needle_preset()
{
    return cached_needle_preset();
}

void set_needle_preset(NeedlePreset preset)
{
    if (static_cast<int>(preset) < 0 || static_cast<int>(preset) >= needle_preset_count)
        preset = needle_preset_default;
    cached_needle_preset() = preset;
    fb2k::configStore::get()->setConfigInt(needle_preset_config_key, static_cast<int>(preset));
    // 錶面的校準字（0 VU = …）要重畫
    for (const auto& [wnd, state] : states())
        state->static_dirty = true;
    refresh_layouts();
}

Style get_style()
{
    return cached_style();
}

void set_style(Style style)
{
    if (static_cast<int>(style) < 0 || static_cast<int>(style) >= style_count)
        style = style_default;
    cached_style() = style;
    fb2k::configStore::get()->setConfigInt(style_config_key, static_cast<int>(style));
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
    const auto target = compute_window_rect(bar_rect, cached_style());

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
