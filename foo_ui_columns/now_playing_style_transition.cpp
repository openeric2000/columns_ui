#include "pch.h"

#if __has_include("nocturne_build_info.h")
#include "nocturne_build_info.h"
#endif

#include "nocturne_dcomp.h"
#include "now_playing_style_transition.h"
#include "now_playing_cover_zoom.h"
#include "menu_items.h"
#include "splitter.h"

// 展開頁封面⇄資訊卡、切換顯示內容的過渡動畫，說明見 now_playing_style_transition.h

namespace cui::style_transition {

namespace {

constexpr wchar_t window_class_name[] = L"columns_ui_style_transition";
// 等 ESLyric 畫好的檢查計時器（動畫本身由 DWM 跑，不靠這個）
constexpr UINT_PTR poll_timer_id = 1;
constexpr UINT poll_interval_ms = 15;

// 封面外圍多截一圈（封面有陰影）；封面下緣到歌名只有 24 DIP，不能超過，不然歌名的跑馬燈會被蓋住的那一小條凍住
constexpr int pad_dip = 20;
// 預覽頁的「淡出＋微縮放」：基準 250 ms × 1.15
constexpr double duration_ms = 290;
constexpr float old_end_scale = 0.96f;
constexpr float new_start_scale = 1.04f;
// 動畫跑完後再多等一點才拿掉蓋層（DWM 最後一格要先合成上去）
constexpr ULONGLONG finish_margin_ms = 40;

// 等 ESLyric 畫好切換後的畫面：至少等一下再截，最多等這麼久（到了還沒穩定就用最後一張）
constexpr ULONGLONG settle_min_ms = 30;
constexpr ULONGLONG settle_limit_ms = 450;
// 封面方塊裡取樣點的平均色差（0～255）：跟切換前差超過這個才算換過去了；連續兩張差小於這個才算畫好了
constexpr double changed_threshold = 6.0;
constexpr double stable_threshold = 1.5;
// 整個展開頁（換模式）用的門檻，見 poll_ready
constexpr int stage_cell_threshold = 72;
constexpr int stage_changed_cells = 2;
constexpr double stage_changed_average = 0.5;

// 換歌交叉淡化（2026-11-14）：自然換歌在結束前這麼多秒先截圖蓋上（剩 pre_capture_window_s 秒內才排計時器）；
// 蓋上後最多等這麼久換歌，沒等到就照常淡出
// 2026-11-14 219 實測：結束前 0.25 秒排的計時器，實際在換歌前 344 ms 蓋上（計時器誤差＋截圖約 30 ms），改 0.18
constexpr double pre_capture_lead_s = 0.18;
constexpr double pre_capture_window_s = 2.5;
constexpr ULONGLONG track_wait_limit_ms = 1500;
// 換歌後不等畫面「穩定」：ESLyric 的封面自己溶 0.26 秒、歌詞會捲動，整個展開頁一直在動，照換模式的規則每次都等滿 0.45 秒
// （實測換歌後停住 469～484 ms 才淡出）。改成至少等 track_settle_min_ms、看到畫面換了就淡出（底下還在溶沒關係），最多等 track_settle_limit_ms
constexpr ULONGLONG track_settle_min_ms = 100;
constexpr ULONGLONG track_settle_limit_ms = 300;

// 跟 CSS 的 cubic-bezier(x1, y1, x2, y2) 相同（同 splitter_window.cpp），給時間比例 x 求進度
double cubic_bezier(double x1, double y1, double x2, double y2, double x)
{
    const auto sample = [](double p1, double p2, double t) {
        return ((1 + 3 * p1 - 3 * p2) * t + (3 * p2 - 6 * p1)) * t * t + 3 * p1 * t;
    };

    double low = 0;
    double high = 1;
    double t = x;
    for (int iteration = 0; iteration < 32; ++iteration) {
        const auto value = sample(x1, x2, t);
        if (std::abs(value - x) < 1e-6)
            break;
        if (value < x)
            low = t;
        else
            high = t;
        t = (low + high) / 2;
    }

    return sample(y1, y2, t);
}

double eased(double x)
{
    return cubic_bezier(0.4, 0.0, 0.2, 1.0, std::clamp(x, 0.0, 1.0));
}

using Signature = std::vector<int>;

// ---- DirectComposition（2026-11-12 起）----
// 原本每格用 GDI+ 把兩張封面大小的圖縮放、調透明度再 UpdateLayeredWindow：219 實測一格 16～33 ms，
// 加上等新畫面時每次截圖約 30 ms，整段只有約 14 格、格間隔 34～50 ms（使用者說不順）。
// 改成把舊、新兩張圖各上傳一次，縮放與透明度交給 DWM 的動畫（IDCompositionAnimation）：每格不花 CPU、照螢幕更新跑，
// 主執行緒忙（ESLyric 重排版面、截圖判斷）也不會停住
// 2026-11-22：裝置、上傳圖片、分段曲線搬到 nocturne_dcomp.h，跟輸出裝置顯示的滑動動畫共用同一個裝置
using nocturne_dcomp::Graphics;
using nocturne_dcomp::graphics;
using nocturne_dcomp::make_surface;

// 一層：圖＋以封面中心為準的縮放＋透明度
struct Layer {
    wil::com_ptr<IDCompositionVisual> visual;
    wil::com_ptr<IDCompositionScaleTransform> scale;
    wil::com_ptr<IDCompositionEffectGroup> effect;
};

std::optional<Layer> make_layer(Graphics& g, Gdiplus::Bitmap& bitmap, const RECT& cover)
{
    const auto surface = make_surface(g, bitmap);
    if (!surface)
        return {};

    Layer layer;
    if (FAILED(g.dcomp->CreateVisual(layer.visual.put())) || FAILED(g.dcomp->CreateScaleTransform(layer.scale.put()))
        || FAILED(g.dcomp->CreateEffectGroup(layer.effect.put())))
        return {};

    layer.visual->SetContent(surface.get());
    layer.visual->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);
    layer.scale->SetCenterX(static_cast<float>(cover.left + cover.right) / 2.f);
    layer.scale->SetCenterY(static_cast<float>(cover.top + cover.bottom) / 2.f);
    layer.visual->SetTransform(layer.scale.get());
    layer.visual->SetEffect(layer.effect.get());
    return layer;
}

// from → to、照 cubic-bezier(0.4, 0, 0.2, 1) 走 length_ms 的動畫：DirectComposition 只吃分段三次多項式，
// 所以把曲線切 12 段，每段用兩端的值與斜率做 Hermite 內插（跟真曲線差不到 0.1%）
wil::com_ptr<IDCompositionAnimation> make_animation(Graphics& g, float from, float to, double length_ms = duration_ms)
{
    // 這條 bezier 沒有現成的導數公式，用數值微分（跟搬出去之前一樣）
    const auto slope = [](double x) {
        constexpr double h = 1e-4;
        const auto a = std::max(0.0, x - h);
        const auto b = std::min(1.0, x + h);
        return (eased(b) - eased(a)) / (b - a);
    };
    return nocturne_dcomp::make_animation(g, from, to, length_ms, eased, slope);
}

// 讓一層照動畫縮放、淡入淡出（scale_from == scale_to 就不縮放）
void animate(Graphics& g, Layer& layer, float scale_from, float scale_to, float opacity_from, float opacity_to)
{
    if (scale_from != scale_to) {
        if (const auto scale = make_animation(g, scale_from, scale_to)) {
            layer.scale->SetScaleX(scale.get());
            layer.scale->SetScaleY(scale.get());
        }
    }
    if (const auto opacity = make_animation(g, opacity_from, opacity_to))
        layer.effect->SetOpacity(opacity.get());
}

// 毫秒時鐘（GetTickCount64 只有約 16 ms 精度，算動畫走到哪不夠準）
double now_ms()
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// 一層目前這段動畫的起點、終點與時間：連點倒回去時要知道它現在走到哪（DWM 那邊的值讀不回來，照同一條曲線自己算）
struct Track {
    float opacity_from{1.f};
    float opacity_to{1.f};
    float scale_from{1.f};
    float scale_to{1.f};
    double start{};
    double length{};

    double progress(double now) const { return length > 0 ? eased((now - start) / length) : 1.0; }
    float opacity(double now) const
    {
        return static_cast<float>(opacity_from + (opacity_to - opacity_from) * progress(now));
    }
    float scale(double now) const { return static_cast<float>(scale_from + (scale_to - scale_from) * progress(now)); }
    double end() const { return start + length; }
};

// 照 track 設定一層的動畫（縮放沒變就直接設定值）
void animate(Graphics& g, Layer& layer, const Track& track)
{
    if (track.scale_from != track.scale_to) {
        if (const auto scale = make_animation(g, track.scale_from, track.scale_to, track.length)) {
            layer.scale->SetScaleX(scale.get());
            layer.scale->SetScaleY(scale.get());
        }
    } else {
        layer.scale->SetScaleX(track.scale_to);
        layer.scale->SetScaleY(track.scale_to);
    }
    if (const auto opacity = make_animation(g, track.opacity_from, track.opacity_to, track.length))
        layer.effect->SetOpacity(opacity.get());
}

struct Transition {
    HWND wnd{};
    // 蓋層視窗在螢幕上的位置（封面方塊加外圍一圈）、封面在蓋層裡的位置
    RECT screen{};
    RECT cover{};
    Signature before_signature;
    Signature last_signature;
    std::unique_ptr<Gdiplus::Bitmap> last_capture;
    wil::com_ptr<IDCompositionTarget> target;
    wil::com_ptr<IDCompositionVisual> root;
    // 舊的一面縮小時露出來的邊：用外圍一圈的背景色內插出來的底（虛化背景很平滑，看不出接縫）
    wil::com_ptr<IDCompositionVisual> base;
    std::optional<Layer> old_layer;
    std::optional<Layer> new_layer;
    ULONGLONG applied_at{};
    // ESLyric 畫好切換後畫面的時間（新的一面從這時開始淡入）；畫好之前舊的一面已經開始動（2026-11-12）
    ULONGLONG ready_at{};
    ULONGLONG finish_at{};
    bool ready{};
    // 整個展開頁淡出（切換展開頁顯示內容，2026-11-11）：蓋層放舊畫面，ESLyric 畫好後整個蓋層淡出、透出底下真的新畫面，不縮放
    bool whole_stage{};
    // 換歌交叉淡化（2026-11-14）：舊畫面先蓋上、還沒換歌；換歌（on_playback_new_track）後才開始等 ESLyric 畫好
    bool track{};
    bool waiting_track{};
    ULONGLONG shown_at{};
    // 封面方塊連點倒回去（2026-11-16）：兩層各自目前的動畫、現在要往哪一面（true＝切換後那面）、
    // 切換後那面的取樣（倒回去再點回來時用來確認底下畫好了）、拿掉蓋層前底下要長得像哪一面
    Track old_track;
    Track new_track;
    bool target_new{true};
    Signature new_signature;
    std::optional<Signature> confirm_signature;
    ULONGLONG confirm_deadline{};
};

std::unique_ptr<Transition> g_transition;

#if defined(NOCTURNE_BUILD_DEV)
// 開發版才有的動畫紀錄（2026-11-12 使用者說封面⇄資訊卡的淡出＋微縮放不順，先量再改）：
// 環境變數 NOCTURNE_ANIM_LOG＝文字檔路徑時，每次切換記一段：截圖、蓋上、切換、畫好、拿掉蓋層的時間，
// 以及這段期間 DWM 合成了幾格、掉了幾格（DirectComposition 的動畫在 DWM 跑，掉格只會發生在那邊）。動畫跑完才一次寫檔
struct AnimLog {
    bool checked{};
    std::wstring path;
    LARGE_INTEGER frequency{};
    LARGE_INTEGER start{};
    DWM_TIMING_INFO dwm_start{};
    std::string text;

    bool enabled()
    {
        if (!checked) {
            checked = true;
            wchar_t buffer[MAX_PATH]{};
            if (GetEnvironmentVariable(L"NOCTURNE_ANIM_LOG", buffer, MAX_PATH))
                path = buffer;
            QueryPerformanceFrequency(&frequency);
        }
        return !path.empty();
    }
    double now() const
    {
        LARGE_INTEGER value{};
        QueryPerformanceCounter(&value);
        return static_cast<double>(value.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
    }
    static DWM_TIMING_INFO timing()
    {
        DWM_TIMING_INFO info{};
        info.cbSize = sizeof(info);
        DwmGetCompositionTimingInfo(nullptr, &info);
        return info;
    }
    void begin(const char* kind)
    {
        if (!enabled())
            return;
        QueryPerformanceCounter(&start);
        dwm_start = timing();
        text = fmt::format("== {}\n", kind);
    }
    void line(const std::string& value)
    {
        if (!path.empty() && !text.empty())
            text += value + "\n";
    }
    void end()
    {
        if (path.empty() || text.empty())
            return;
        const auto dwm = timing();
        text += fmt::format("end t={:.1f} dwm_frames={} dropped={} missed={}\n", now(), dwm.cFrame - dwm_start.cFrame,
            dwm.cFramesDropped - dwm_start.cFramesDropped, dwm.cFramesMissed - dwm_start.cFramesMissed);
        FILE* file{};
        if (_wfopen_s(&file, path.c_str(), L"ab") == 0 && file) {
            fwrite(text.data(), 1, text.size(), file);
            fclose(file);
        }
        text.clear();
    }
};
AnimLog g_anim_log;
#define ANIM_LOG_BEGIN(kind) g_anim_log.begin(kind)
#define ANIM_LOG_LINE(value) g_anim_log.line(value)
#define ANIM_LOG_NOW() g_anim_log.now()
#define ANIM_LOG_END() g_anim_log.end()
#else
#define ANIM_LOG_BEGIN(kind) ((void)0)
#define ANIM_LOG_LINE(value) ((void)0)
#define ANIM_LOG_NOW() 0.0
#define ANIM_LOG_END() ((void)0)
#endif

// 截下螢幕上 screen 那一塊（用展開頁截圖：PrintWindow 整個主視窗，不含蓋層這種另外的視窗）
std::unique_ptr<Gdiplus::Bitmap> capture(const RECT& screen)
{
    RECT stage{};
    HWND owner{};
    const auto bitmap = panels::splitter::FlatSplitterPanel::s_capture_now_playing_stage(stage, owner);
    if (!bitmap)
        return {};

    const auto width = wil::rect_width(screen);
    const auto height = wil::rect_height(screen);
    Gdiplus::Bitmap whole(bitmap.get(), nullptr);
    if (whole.GetLastStatus() != Gdiplus::Ok || width <= 0 || height <= 0)
        return {};

    auto result = std::make_unique<Gdiplus::Bitmap>(width, height, PixelFormat32bppPARGB);
    {
        Gdiplus::Graphics graphics(result.get());
        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        graphics.DrawImage(&whole, Gdiplus::Rect(0, 0, width, height), screen.left - stage.left, screen.top - stage.top,
            width, height, Gdiplus::UnitPixel);
    }
    return result;
}

// 封面方塊裡 16×16 個取樣點的顏色，用來判斷畫面換了沒、穩定了沒
Signature signature_of(Gdiplus::Bitmap& bitmap, const RECT& area)
{
    constexpr int grid = 16;
    Signature result;
    result.reserve(grid * grid * 3);
    const auto width = area.right - area.left;
    const auto height = area.bottom - area.top;

    for (int gy = 0; gy < grid; ++gy) {
        for (int gx = 0; gx < grid; ++gx) {
            Gdiplus::Color colour;
            bitmap.GetPixel(area.left + (2 * gx + 1) * width / (2 * grid), area.top + (2 * gy + 1) * height / (2 * grid),
                &colour);
            result.push_back(colour.GetR());
            result.push_back(colour.GetG());
            result.push_back(colour.GetB());
        }
    }
    return result;
}

double difference(const Signature& a, const Signature& b)
{
    if (a.size() != b.size() || a.empty())
        return 255.0;

    double sum{};
    for (size_t i = 0; i < a.size(); ++i)
        sum += std::abs(a[i] - b[i]);
    return sum / static_cast<double>(a.size());
}

// 用蓋層最外圈（封面陰影以外，只有背景）的顏色內插出整塊的底（Coons 曲面：四邊各自線性混合，再扣掉四角）
std::unique_ptr<Gdiplus::Bitmap> make_base(Gdiplus::Bitmap& source)
{
    const auto width = static_cast<int>(source.GetWidth());
    const auto height = static_cast<int>(source.GetHeight());
    if (width < 2 || height < 2)
        return {};

    Gdiplus::BitmapData in{};
    const Gdiplus::Rect all(0, 0, width, height);
    if (source.LockBits(&all, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &in) != Gdiplus::Ok)
        return {};

    const auto pixel = [&in](int x, int y) {
        return reinterpret_cast<const uint32_t*>(static_cast<const BYTE*>(in.Scan0) + static_cast<ptrdiff_t>(y) * in.Stride)[x];
    };
    std::vector<uint32_t> top(width), bottom(width), left(height), right(height);
    for (int x = 0; x < width; ++x) {
        top[x] = pixel(x, 0);
        bottom[x] = pixel(x, height - 1);
    }
    for (int y = 0; y < height; ++y) {
        left[y] = pixel(0, y);
        right[y] = pixel(width - 1, y);
    }
    source.UnlockBits(&in);

    auto result = std::make_unique<Gdiplus::Bitmap>(width, height, PixelFormat32bppARGB);
    Gdiplus::BitmapData out{};
    if (result->LockBits(&all, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &out) != Gdiplus::Ok)
        return {};

    const auto channel = [](uint32_t value, int shift) { return static_cast<double>((value >> shift) & 0xff); };
    const auto c00 = top[0], c10 = top[width - 1], c01 = bottom[0], c11 = bottom[width - 1];

    for (int y = 0; y < height; ++y) {
        auto* row = reinterpret_cast<uint32_t*>(static_cast<BYTE*>(out.Scan0) + static_cast<ptrdiff_t>(y) * out.Stride);
        const double v = static_cast<double>(y) / (height - 1);
        for (int x = 0; x < width; ++x) {
            const double u = static_cast<double>(x) / (width - 1);
            uint32_t colour = 0xff000000;
            for (const int shift : {0, 8, 16}) {
                const double value = (1 - u) * channel(left[y], shift) + u * channel(right[y], shift)
                    + (1 - v) * channel(top[x], shift) + v * channel(bottom[x], shift)
                    - ((1 - u) * (1 - v) * channel(c00, shift) + u * (1 - v) * channel(c10, shift)
                        + (1 - u) * v * channel(c01, shift) + u * v * channel(c11, shift));
                colour |= static_cast<uint32_t>(std::clamp(std::lround(value), 0L, 255L)) << shift;
            }
            row[x] = colour;
        }
    }
    result->UnlockBits(&out);
    return result;
}

void finish()
{
    if (!g_transition)
        return;

    auto transition = std::move(g_transition);
    ANIM_LOG_END();
    if (transition->wnd)
        KillTimer(transition->wnd, poll_timer_id);
    // 先拆掉 DirectComposition 的內容再關視窗
    transition->old_layer.reset();
    transition->new_layer.reset();
    transition->base.reset();
    transition->root.reset();
    transition->target.reset();
    if (transition->wnd)
        DestroyWindow(transition->wnd);
}

// ESLyric 畫好切換後的畫面了沒（2026-11-12 改）：
// - 封面方塊：跟切換前不一樣、而且跟上一張差不多（封面是靜態圖，兩三張就穩定）
// - 整個展開頁：門檻另外訂（見下面），原本用封面的門檻永遠判斷不到，每次都等滿上限 0.45 秒（2026-11-12 實測）
// 截圖一次約 30 ms（219），會佔住主執行緒，但動畫在 DWM 跑，不受影響
void poll_ready(Transition& transition, ULONGLONG now)
{
    const auto elapsed = now - transition.applied_at;

    // 換歌：畫面跟蓋上時不一樣了（新的字畫出來了）就淡出，不等穩定
    if (transition.track) {
        if (elapsed < track_settle_min_ms)
            return;
        bool changed = false;
        if (auto shot = capture(transition.screen)) {
            const auto signature = signature_of(*shot, transition.cover);
            int cells = 0;
            for (size_t i = 0; i + 2 < signature.size() && i + 2 < transition.before_signature.size(); i += 3) {
                const auto d = std::abs(signature[i] - transition.before_signature[i])
                    + std::abs(signature[i + 1] - transition.before_signature[i + 1])
                    + std::abs(signature[i + 2] - transition.before_signature[i + 2]);
                if (d > stage_cell_threshold)
                    cells++;
            }
            changed = cells >= stage_changed_cells || difference(signature, transition.before_signature) > stage_changed_average;
        }
        if (!changed && elapsed < track_settle_limit_ms)
            return;
        transition.ready = true;
        transition.ready_at = now;
        return;
    }

    if (elapsed < settle_min_ms)
        return;

    bool ready = false;
    if (auto shot = capture(transition.screen)) {
        auto signature = signature_of(*shot, transition.cover);
        const bool changed = difference(signature, transition.before_signature) > changed_threshold;
        if (transition.whole_stage) {
            // 整個展開頁換模式通常只換右邊那一塊，16×16 取樣點平均色差只有 1～2（實測），用封面的門檻 6 永遠判斷不到。
            // 改成：有幾個取樣點明顯不同（三色合計差 72 以上）或平均差超過 0.5，而且跟上一張幾乎一樣（平均差 1 以下）
            int cells = 0;
            for (size_t i = 0; i + 2 < signature.size() && i + 2 < transition.before_signature.size(); i += 3) {
                const auto d = std::abs(signature[i] - transition.before_signature[i])
                    + std::abs(signature[i + 1] - transition.before_signature[i + 1])
                    + std::abs(signature[i + 2] - transition.before_signature[i + 2]);
                if (d > stage_cell_threshold)
                    cells++;
            }
            const auto from_before = difference(signature, transition.before_signature);
            const bool stage_changed = cells >= stage_changed_cells || from_before > stage_changed_average;
            // 2026-11-17：原本還要「跟上一張差不多」才算畫好，每次截圖 33 ms＋輪詢 15 ms，至少要多等一輪（219 動畫紀錄：切換後 90～110 ms 才淡出）。
            // VU 表在切換當下就顯示、ESLyric 也是一次重畫，看到畫面換了就淡出（跟換歌同一個規則）；
            // 就算 ESLyric 還在補畫，淡出剛開始時蓋層幾乎不透明，看不出來
            ready = stage_changed;
        } else {
            const bool stable = difference(signature, transition.last_signature) < stable_threshold;
            ready = changed && stable;
        }
        transition.last_signature = std::move(signature);
        transition.last_capture = std::move(shot);
    }

    if (!ready && elapsed < settle_limit_ms)
        return;

    transition.ready = true;
    transition.ready_at = now;
}

// ESLyric 畫好了：封面方塊時新的一面從 104% 縮回、淡入（疊在舊的一面上面）；整個展開頁時舊畫面整張淡出
void start_second_half(Transition& transition)
{
    auto* g = graphics();
    if (g) {
        if (transition.whole_stage) {
            if (transition.old_layer)
                animate(*g, *transition.old_layer, 1.f, 1.f, 1.f, 0.f);
        } else if (transition.last_capture) {
            transition.new_layer = make_layer(*g, *transition.last_capture, transition.cover);
            if (transition.new_layer) {
                transition.new_layer->effect->SetOpacity(0.f);
                transition.new_layer->scale->SetScaleX(new_start_scale);
                transition.new_layer->scale->SetScaleY(new_start_scale);
                transition.root->AddVisual(transition.new_layer->visual.get(), TRUE,
                    transition.old_layer ? transition.old_layer->visual.get() : nullptr);
                transition.new_track = Track{0.f, 1.f, new_start_scale, 1.f, now_ms(), duration_ms};
                animate(*g, *transition.new_layer, transition.new_track);
                transition.new_signature = transition.last_signature;
            }
        }
        g->dcomp->Commit();
    }
    transition.last_capture.reset();

    if (!transition.whole_stage) {
        // 封面方塊：兩層各自的動畫都跑完才拿掉蓋層（舊的一面可能是連點倒回去後重新開始的）
        const auto end = std::max(transition.old_track.end(), transition.new_layer ? transition.new_track.end() : 0.0);
        transition.finish_at = GetTickCount64() + static_cast<ULONGLONG>(std::max(0.0, end - now_ms())) + finish_margin_ms;
        return;
    }

    // 舊的一面在切換時就開始動；兩段都跑完才拿掉蓋層
    const auto end = std::max(transition.applied_at, transition.ready_at) + static_cast<ULONGLONG>(duration_ms);
    transition.finish_at = std::max(end, transition.applied_at + static_cast<ULONGLONG>(duration_ms)) + finish_margin_ms;
}

// 拿掉蓋層之前，底下真的畫面是不是已經是要停的那一面（連點倒回去時 ESLyric 要重畫，還沒畫好就拿掉會閃一下舊的）
bool underneath_ready(Transition& transition, ULONGLONG now)
{
    if (!transition.confirm_signature)
        return true;
    if (now >= transition.confirm_deadline) {
        ANIM_LOG_LINE(fmt::format("confirm timed out t={:.1f}", ANIM_LOG_NOW()));
        return true;
    }
    const auto shot = capture(transition.screen);
    if (!shot)
        return true;
    return difference(signature_of(*shot, transition.cover), *transition.confirm_signature) < changed_threshold;
}

// 封面方塊動畫還沒跑完又點了一次：從兩層現在的透明度與縮放倒回去（to_new＝往切換後那一面）。
// 蓋層最底下有一層不透明的底，底下的 ESLyric 在蓋層拿掉前看不到，所以可以先切換再慢慢動
bool retarget(Transition& transition, bool to_new)
{
    auto* g = graphics();
    if (!g || !transition.old_layer)
        return false;

    const auto now = now_ms();
    const auto tick = GetTickCount64();
    // 剩下的距離越短走越快，最少 min_reverse_ms
    static constexpr double min_reverse_ms = 120;
    const auto length_for = [](float from, float to) {
        return std::max(min_reverse_ms, duration_ms * std::abs(static_cast<double>(to) - from));
    };

    const auto old_opacity = transition.old_track.opacity(now);
    const auto old_scale = transition.old_track.scale(now);
    const auto old_to_opacity = to_new ? 0.f : 1.f;
    const auto old_to_scale = to_new ? old_end_scale : 1.f;
    transition.old_track
        = Track{old_opacity, old_to_opacity, old_scale, old_to_scale, now, length_for(old_opacity, old_to_opacity)};
    animate(*g, *transition.old_layer, transition.old_track);
    auto end = transition.old_track.end();

    if (transition.new_layer) {
        const auto new_opacity = transition.new_track.opacity(now);
        const auto new_scale = transition.new_track.scale(now);
        const auto new_to_opacity = to_new ? 1.f : 0.f;
        const auto new_to_scale = to_new ? 1.f : new_start_scale;
        transition.new_track
            = Track{new_opacity, new_to_opacity, new_scale, new_to_scale, now, length_for(new_opacity, new_to_opacity)};
        animate(*g, *transition.new_layer, transition.new_track);
        end = std::max(end, transition.new_track.end());
    }
    g->dcomp->Commit();

    transition.target_new = to_new;
    transition.last_capture.reset();
    transition.finish_at = tick + static_cast<ULONGLONG>(std::max(0.0, end - now)) + finish_margin_ms;
    transition.confirm_deadline = transition.finish_at + settle_limit_ms;

    if (!to_new) {
        // 倒回原本那一面：圖已經有了，不用再等 ESLyric；拿掉蓋層前確認底下畫回原本的樣子
        transition.ready = true;
        transition.confirm_signature = transition.before_signature;
    } else if (transition.new_layer) {
        transition.ready = true;
        transition.confirm_signature = transition.new_signature;
    } else {
        // 切換後那面還沒截到（第一次切換時 ESLyric 還沒畫好就倒回去了）：照第一次的流程重新等它畫好
        transition.ready = false;
        transition.applied_at = tick;
        transition.last_signature.clear();
        transition.confirm_signature.reset();
    }
    ANIM_LOG_LINE(fmt::format("retarget to {} t={:.1f} old opacity {:.2f} new layer {}", to_new ? "new" : "old",
        ANIM_LOG_NOW(), old_opacity, transition.new_layer ? "yes" : "no"));
    return true;
}

void on_timer()
{
    if (!g_transition)
        return;

    auto& transition = *g_transition;
    const auto now = GetTickCount64();

    // 換歌交叉淡化：舊畫面蓋著等換歌；等太久（算錯結束時間、播完清單沒有下一首）就當作已經換了，照常淡出
    if (transition.waiting_track) {
        if (now - transition.shown_at < track_wait_limit_ms)
            return;
        ANIM_LOG_LINE(fmt::format("no new track after {} ms", now - transition.shown_at));
        transition.waiting_track = false;
        transition.applied_at = now;
    }

    if (!transition.ready) {
        poll_ready(transition, now);
        if (transition.ready) {
            ANIM_LOG_LINE(fmt::format("ready t={:.1f} (since apply {} ms)", ANIM_LOG_NOW(), transition.ready_at - transition.applied_at));
            start_second_half(transition);
            ANIM_LOG_LINE(fmt::format("second half committed t={:.1f}", ANIM_LOG_NOW()));
        }
        return;
    }

    if (now >= transition.finish_at && underneath_ready(transition, now))
        finish();
}

LRESULT CALLBACK window_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_NCHITTEST:
        // 不擋滑鼠：點到的是底下的展開頁
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_TIMER:
        if (wp == poll_timer_id) {
            on_timer();
            return 0;
        }
        break;
    }
    return DefWindowProc(wnd, msg, wp, lp);
}

HWND create_overlay(HWND owner, const RECT& screen)
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

    // 沒有重新導向點陣圖：畫面完全由 DirectComposition 提供，沒放內容的地方是透明的
    const auto wnd = CreateWindowEx(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, window_class_name, L"",
        WS_POPUP, screen.left, screen.top, wil::rect_width(screen), wil::rect_height(screen), owner, nullptr,
        core_api::get_my_instance(), nullptr);

    if (wnd) {
        // 關掉 Windows 開視窗時的淡入動畫，一出現就要跟原本的畫面一模一樣
        BOOL disable_transitions = TRUE;
        DwmSetWindowAttribute(wnd, DWMWA_TRANSITIONS_FORCEDISABLED, &disable_transitions, sizeof(disable_transitions));
    }
    return wnd;
}

// 蓋層在前後順序裡要插在誰的下面，才會緊貼在主視窗正上方（2026-11-22）。
// 新顯示的視窗 Windows 一律排到整個桌面最上面，不管擁有者在哪：fb2k 被瀏覽器蓋住時，
// 自然換歌的蓋層（結束前先截圖蓋上）會閃到瀏覽器前面，約半秒後淡出。
// 主視窗上面是「永遠在最上層」的視窗（或就是蓋層自己）時用 HWND_TOP：插在最上層視窗後面會讓蓋層也變成最上層
HWND overlay_insert_after(HWND owner, HWND overlay)
{
    const auto above = owner ? GetWindow(owner, GW_HWNDPREV) : nullptr;
    if (!above || above == overlay || (GetWindowLongPtr(above, GWL_EXSTYLE) & WS_EX_TOPMOST))
        return HWND_TOP;
    return above;
}

// 蓋層：截下來的舊畫面（封面方塊時底下再墊一層內插的底），蓋上去、等它真的合成到螢幕上才回傳
bool show_overlay(Graphics& g, Transition& transition, HWND owner, Gdiplus::Bitmap& before, Gdiplus::Bitmap* base)
{
    transition.wnd = create_overlay(owner, transition.screen);
    if (!transition.wnd)
        return false;

    if (FAILED(g.dcomp->CreateTargetForHwnd(transition.wnd, TRUE, transition.target.put()))
        || FAILED(g.dcomp->CreateVisual(transition.root.put())) || FAILED(transition.target->SetRoot(transition.root.get())))
        return false;

    if (base) {
        const auto surface = make_surface(g, *base);
        if (surface && SUCCEEDED(g.dcomp->CreateVisual(transition.base.put()))) {
            transition.base->SetContent(surface.get());
            transition.root->AddVisual(transition.base.get(), FALSE, nullptr);
        }
    }

    transition.old_layer = make_layer(g, before, transition.cover);
    if (!transition.old_layer)
        return false;
    transition.root->AddVisual(
        transition.old_layer->visual.get(), TRUE, transition.base ? transition.base.get() : nullptr);

    if (FAILED(g.dcomp->Commit()))
        return false;
    // 不用 ShowWindow：它會把蓋層排到整個桌面最上面（見 overlay_insert_after）
    SetWindowPos(transition.wnd, overlay_insert_after(owner, transition.wnd), 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    g.dcomp->WaitForCommitCompletion();
    [[maybe_unused]] const auto hr = DwmFlush();
    return true;
}

// 共用：截圖、蓋上、切換、舊的一面開始動、開始等 ESLyric 畫好
void start(std::unique_ptr<Transition> transition, HWND owner, std::function<void()>& apply)
{
    auto* g = graphics();
    auto before = g ? capture(transition->screen) : nullptr;
    if (!before) {
        apply();
        return;
    }
    ANIM_LOG_LINE(fmt::format("captured t={:.1f} size={}x{}", ANIM_LOG_NOW(), wil::rect_width(transition->screen),
        wil::rect_height(transition->screen)));

    transition->before_signature = signature_of(*before, transition->cover);
    const auto base = transition->whole_stage ? nullptr : make_base(*before);

    if (!show_overlay(*g, *transition, owner, *before, base.get())) {
        ANIM_LOG_LINE("overlay failed");
        const auto wnd = transition->wnd;
        transition.reset();
        if (wnd)
            DestroyWindow(wnd);
        apply();
        return;
    }
    ANIM_LOG_LINE(fmt::format("shown t={:.1f}", ANIM_LOG_NOW()));

    g_transition = std::move(transition);
    apply();
    g_transition->applied_at = GetTickCount64();
    ANIM_LOG_LINE(fmt::format("applied t={:.1f}", ANIM_LOG_NOW()));

    // 封面方塊：舊的一面一切換就開始縮小淡出（DWM 照螢幕更新跑，主執行緒忙也不停）
    if (!g_transition->whole_stage && g_transition->old_layer) {
        g_transition->old_track = Track{1.f, 0.f, 1.f, old_end_scale, now_ms(), duration_ms};
        animate(*g, *g_transition->old_layer, g_transition->old_track);
        g->dcomp->Commit();
    }
    SetTimer(g_transition->wnd, poll_timer_id, poll_interval_ms, nullptr);
}

} // namespace

void run(std::function<void()> apply)
{
    // 封面⇄資訊卡的動畫還沒跑完又點了一次（2026-11-16 使用者：原本會直接跳過去、沒有動畫）：
    // 底下照樣切換，蓋層從兩層目前的樣子倒回去（或再轉回來）
    if (g_transition && !g_transition->whole_stage && !g_transition->track) {
        const auto to_new = !g_transition->target_new;
        apply();
        if (retarget(*g_transition, to_new))
            return;
        finish();
        return;
    }

    // 上一次的動畫（換顯示內容、換歌）還沒跑完又切換：結束它，這次直接切換、不做動畫
    if (g_transition) {
        finish();
        apply();
        return;
    }

    RECT stage{};
    HWND owner{};
    if (!panels::splitter::FlatSplitterPanel::s_get_now_playing_stage(stage, owner)) {
        apply();
        return;
    }

    // 純封面模式（2026-11-22 起也能點封面切資訊卡）：動畫範圍是置中的大封面
    const auto solo = main_menu::get_now_playing_mode() == main_menu::NowPlayingMode::cover;
    const auto cover_in_stage = solo
        ? cover_zoom::solo_cover_rect_in_stage(wil::rect_width(stage), wil::rect_height(stage))
        : cover_zoom::cover_rect_in_stage(wil::rect_width(stage), wil::rect_height(stage));
    if (!cover_in_stage) {
        apply();
        return;
    }

    RECT cover_screen = *cover_in_stage;
    OffsetRect(&cover_screen, stage.left, stage.top);
    RECT screen = cover_screen;
    const auto pad = uih::scale_dpi_value(pad_dip);
    InflateRect(&screen, pad, pad);
    IntersectRect(&screen, &screen, &stage);

    ANIM_LOG_BEGIN("cover");
    auto transition = std::make_unique<Transition>();
    transition->screen = screen;
    transition->cover = cover_screen;
    OffsetRect(&transition->cover, -screen.left, -screen.top);
    start(std::move(transition), owner, apply);
}

void run_stage(std::function<void()> apply)
{
    if (g_transition) {
        finish();
        apply();
        return;
    }

    RECT stage{};
    HWND owner{};
    if (!panels::splitter::FlatSplitterPanel::s_get_now_playing_stage(stage, owner)) {
        apply();
        return;
    }

    ANIM_LOG_BEGIN("stage");
    auto transition = std::make_unique<Transition>();
    transition->whole_stage = true;
    transition->screen = stage;
    transition->cover = RECT{0, 0, wil::rect_width(stage), wil::rect_height(stage)};
    start(std::move(transition), owner, apply);
}

// ---- 換歌交叉淡化（2026-11-14，v1.4 待做清單第 11 個）----
// ESLyric 換歌時封面會自己溶約 0.26 秒，但標題、資訊列、歌詞、背景是當下直接換掉。做法跟切換顯示內容一樣：
// 舊畫面截下來蓋在整個展開頁上，換歌後等 ESLyric 畫好，整個蓋層由 DWM 淡出。
// 舊畫面一定要在 ESLyric 換字之前截、而且蓋上去（換歌後才蓋，會先閃一下新畫面再跳回舊的）：
// - 手動換歌（下一首、雙擊別首）：on_playback_starting，這時還沒開新歌
// - 自然換歌沒有 starting：照播放位置排計時器，結束前 pre_capture_lead_s 秒截圖蓋上（最後這一下畫面停住，看不太出來）
// 拖進度、暫停、停止時拿掉還在等換歌的蓋層
namespace {

UINT_PTR g_pre_capture_timer{};

void cancel_pre_capture()
{
    if (g_pre_capture_timer) {
        KillTimer(nullptr, g_pre_capture_timer);
        g_pre_capture_timer = 0;
    }
}

// 截下目前的展開頁、蓋上去，等換歌；展開頁沒完全展開、正在跑別的動畫時什麼都不做
void cover_before_track_change(const char* reason)
{
    if (g_transition)
        return;

    RECT stage{};
    HWND owner{};
    if (!panels::splitter::FlatSplitterPanel::s_get_now_playing_stage(stage, owner))
        return;

    auto* g = graphics();
    auto before = g ? capture(stage) : nullptr;
    if (!before)
        return;

    ANIM_LOG_BEGIN(reason);
    auto transition = std::make_unique<Transition>();
    transition->whole_stage = true;
    transition->track = true;
    transition->waiting_track = true;
    transition->screen = stage;
    transition->cover = RECT{0, 0, wil::rect_width(stage), wil::rect_height(stage)};
    transition->before_signature = signature_of(*before, transition->cover);

    if (!show_overlay(*g, *transition, owner, *before, nullptr)) {
        ANIM_LOG_LINE("overlay failed");
        const auto wnd = transition->wnd;
        transition.reset();
        if (wnd)
            DestroyWindow(wnd);
        return;
    }
    ANIM_LOG_LINE(fmt::format("shown t={:.1f}", ANIM_LOG_NOW()));

    g_transition = std::move(transition);
    g_transition->shown_at = GetTickCount64();
    SetTimer(g_transition->wnd, poll_timer_id, poll_interval_ms, nullptr);
}

void CALLBACK on_pre_capture_timer(HWND, UINT, UINT_PTR, DWORD)
{
    cancel_pre_capture();
    // 計時器排好之後又被拖進度、暫停的話不截
    const auto playback = play_control::get();
    if (!playback->is_playing() || playback->is_paused())
        return;
    cover_before_track_change("track (natural)");
}

// 快播完了：排一個計時器在結束前 pre_capture_lead_s 秒截圖（on_playback_time 一秒一次，不夠準）
void schedule_pre_capture()
{
    if (g_pre_capture_timer || g_transition)
        return;

    const auto playback = play_control::get();
    metadb_handle_ptr track;
    if (playback->is_paused() || !playback->get_now_playing(track) || !track.is_valid())
        return;

    const auto length = track->get_length();
    const auto remaining = length - playback->playback_get_position();
    if (length <= 0 || remaining <= pre_capture_lead_s || remaining > pre_capture_window_s)
        return;

    const auto delay = static_cast<UINT>((remaining - pre_capture_lead_s) * 1000.0);
    g_pre_capture_timer = SetTimer(nullptr, 0, std::max(delay, 1u), on_pre_capture_timer);
}

// 還在等換歌的蓋層拿掉（拖進度、暫停、停止）；已經換歌、正在淡出的不動
void drop_waiting_cover()
{
    cancel_pre_capture();
    if (g_transition && g_transition->waiting_track) {
        ANIM_LOG_LINE("dropped before new track");
        finish();
    }
}

class TrackTransitionCallback : public play_callback_static {
public:
    unsigned get_flags() override
    {
        return flag_on_playback_starting | flag_on_playback_new_track | flag_on_playback_stop | flag_on_playback_seek
            | flag_on_playback_pause | flag_on_playback_time;
    }

    void on_playback_starting(play_control::t_track_command command, bool paused) override
    {
        cancel_pre_capture();
        if (!paused)
            cover_before_track_change("track (manual)");
    }

    void on_playback_new_track(metadb_handle_ptr track) override
    {
        cancel_pre_capture();
        if (g_transition && g_transition->waiting_track) {
            g_transition->waiting_track = false;
            g_transition->applied_at = GetTickCount64();
            ANIM_LOG_LINE(fmt::format("new track t={:.1f} (covered {} ms before)", ANIM_LOG_NOW(),
                g_transition->applied_at - g_transition->shown_at));
        }
    }

    void on_playback_stop(play_control::t_stop_reason reason) override
    {
        if (reason != play_control::stop_reason_starting_another)
            drop_waiting_cover();
    }

    void on_playback_seek(double time) override { drop_waiting_cover(); }
    void on_playback_pause(bool state) override { drop_waiting_cover(); }
    void on_playback_time(double time) override { schedule_pre_capture(); }

    void on_playback_edited(metadb_handle_ptr track) override {}
    void on_playback_dynamic_info(const file_info& info) override {}
    void on_playback_dynamic_info_track(const file_info& info) override {}
    void on_volume_change(float new_value) override {}
};

play_callback_static_factory_t<TrackTransitionCallback> g_track_transition_callback;

} // namespace

} // namespace cui::style_transition
