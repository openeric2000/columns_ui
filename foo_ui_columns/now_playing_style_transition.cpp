#include "pch.h"

#if __has_include("nocturne_build_info.h")
#include "nocturne_build_info.h"
#endif

#include "now_playing_style_transition.h"
#include "now_playing_cover_zoom.h"
#include "splitter.h"

// 展開頁封面⇄資訊卡的切換動畫，說明見 now_playing_style_transition.h

namespace cui::style_transition {

namespace {

constexpr wchar_t window_class_name[] = L"columns_ui_style_transition";
constexpr UINT_PTR frame_timer_id = 1;
constexpr UINT frame_interval_ms = 10;

// 封面外圍多截一圈（封面有陰影）；封面下緣到歌名只有 24 DIP，不能超過，不然歌名的跑馬燈會被蓋住的那一小條凍住
constexpr int pad_dip = 20;
// 預覽頁的「淡出＋微縮放」：基準 250 ms × 1.15
constexpr double duration_ms = 290;
constexpr float old_end_scale = 0.96f;
constexpr float new_start_scale = 1.04f;

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
constexpr double stage_stable_average = 1.0;

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

using Signature = std::vector<int>;

struct Transition {
    HWND wnd{};
    // 蓋層視窗在螢幕上的位置（封面方塊加外圍一圈）、封面在蓋層裡的位置
    RECT screen{};
    RECT cover{};
    std::unique_ptr<Gdiplus::Bitmap> before;
    std::unique_ptr<Gdiplus::Bitmap> after;
    // 舊的一面縮小時露出來的邊：用外圍一圈的背景色內插出來的底（虛化背景很平滑，看不出接縫）
    std::unique_ptr<Gdiplus::Bitmap> base;
    Signature before_signature;
    Signature last_signature;
    std::unique_ptr<Gdiplus::Bitmap> last_capture;
    ULONGLONG applied_at{};
    // ESLyric 畫好切換後畫面的時間（新的一面從這時開始淡入）；畫好之前舊的一面已經開始動（2026-11-12）
    ULONGLONG ready_at{};
    bool ready{};
    // 整個展開頁淡出（切換展開頁顯示內容，2026-11-11）：蓋層放舊畫面，ESLyric 畫好後整個蓋層淡出、透出底下真的新畫面，不縮放
    // （2026-11-12 起不再截新畫面每格合成兩張全頁圖：一格 15 ms、偶爾卡 50 ms）
    bool whole_stage{};
};

std::unique_ptr<Transition> g_transition;

#if defined(NOCTURNE_BUILD_DEV)
// 開發版才有的動畫紀錄（2026-11-12 使用者說封面⇄資訊卡的淡出＋微縮放不順，先量再改）：
// 環境變數 NOCTURNE_ANIM_LOG＝文字檔路徑時，每次切換記一段：每一格的時間、跟上一格的間隔、合成花多久、截圖判斷花多久，
// 結束時記總結（格數、最長間隔、超過 25／50 毫秒的次數）。動畫跑完才一次寫檔；沒設環境變數就什麼都不做
struct AnimLog {
    bool checked{};
    std::wstring path;
    LARGE_INTEGER frequency{};
    LARGE_INTEGER start{};
    double last_frame{-1};
    std::vector<double> gaps;
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
    void begin(const char* kind)
    {
        if (!enabled())
            return;
        QueryPerformanceCounter(&start);
        last_frame = -1;
        gaps.clear();
        text = fmt::format("== {}\n", kind);
    }
    void line(const std::string& value)
    {
        if (!path.empty())
            text += value + "\n";
    }
    void frame(double present_ms, double poll_ms, bool ready)
    {
        if (path.empty())
            return;
        const auto t = now();
        const auto gap = last_frame < 0 ? 0.0 : t - last_frame;
        if (last_frame >= 0)
            gaps.push_back(gap);
        last_frame = t;
        text += fmt::format("frame t={:.1f} gap={:.1f} present={:.1f} poll={:.1f} ready={}\n", t, gap, present_ms, poll_ms, ready ? 1 : 0);
    }
    void end()
    {
        if (path.empty() || text.empty())
            return;
        double max_gap{};
        int over25{}, over50{};
        for (const auto gap : gaps) {
            max_gap = std::max(max_gap, gap);
            over25 += gap > 25 ? 1 : 0;
            over50 += gap > 50 ? 1 : 0;
        }
        text += fmt::format("end t={:.1f} frames={} max_gap={:.1f} over25={} over50={}\n", now(), gaps.size() + 1, max_gap, over25, over50);
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
#define ANIM_LOG_FRAME(present, poll, ready) g_anim_log.frame(present, poll, ready)
#define ANIM_LOG_END() g_anim_log.end()
#else
#define ANIM_LOG_BEGIN(kind) ((void)0)
#define ANIM_LOG_LINE(value) ((void)0)
#define ANIM_LOG_NOW() 0.0
#define ANIM_LOG_FRAME(present, poll, ready) ((void)0)
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

// 以封面中心為準縮放、帶透明度畫上去
void draw_scaled(Gdiplus::Graphics& graphics, Gdiplus::Bitmap& image, const RECT& cover, float scale, float alpha)
{
    if (alpha <= 0.002f)
        return;

    const auto width = static_cast<float>(image.GetWidth());
    const auto height = static_cast<float>(image.GetHeight());
    const auto cx = static_cast<float>(cover.left + cover.right) / 2.f;
    const auto cy = static_cast<float>(cover.top + cover.bottom) / 2.f;

    Gdiplus::ColorMatrix matrix = {{{1, 0, 0, 0, 0}, {0, 1, 0, 0, 0}, {0, 0, 1, 0, 0}, {0, 0, 0, alpha, 0}, {0, 0, 0, 0, 1}}};
    Gdiplus::ImageAttributes attributes;
    attributes.SetColorMatrix(&matrix);

    const Gdiplus::RectF destination(cx - cx * scale, cy - cy * scale, width * scale, height * scale);
    graphics.DrawImage(&image, destination, 0.f, 0.f, width, height, Gdiplus::UnitPixel, &attributes);
}

// 畫一格：old_progress＝舊的一面縮小淡出到哪（0～1）、new_progress＝新的一面放大淡入到哪（0～1）。
// 整個展開頁只在一開始畫一次舊畫面（之後用整個蓋層的透明度淡出，見 on_frame）
void present(Transition& transition, double old_progress, double new_progress)
{
    const auto width = wil::rect_width(transition.screen);
    const auto height = wil::rect_height(transition.screen);

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
        Gdiplus::Graphics graphics(&canvas);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeBilinear);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        if (transition.base)
            graphics.DrawImage(transition.base.get(), 0, 0, width, height);
        else
            graphics.Clear(Gdiplus::Color(255, 24, 24, 24));
        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);

        const auto o = static_cast<float>(old_progress);
        const auto n = static_cast<float>(new_progress);
        if (transition.whole_stage) {
            if (transition.before)
                draw_scaled(graphics, *transition.before, transition.cover, 1.f, 1.f);
        } else {
            if (transition.before)
                draw_scaled(graphics, *transition.before, transition.cover, 1.f + (old_end_scale - 1.f) * o, 1.f - o);
            if (transition.after)
                draw_scaled(
                    graphics, *transition.after, transition.cover, new_start_scale + (1.f - new_start_scale) * n, n);
        }
    }

    POINT origin{transition.screen.left, transition.screen.top};
    SIZE size{width, height};
    POINT source{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(transition.wnd, screen_dc.get(), &origin, &size, memory_dc.get(), &source, 0, &blend, ULW_ALPHA);
}

void finish()
{
    if (!g_transition)
        return;

    auto transition = std::move(g_transition);
    ANIM_LOG_END();
    if (transition->wnd) {
        KillTimer(transition->wnd, frame_timer_id);
        DestroyWindow(transition->wnd);
    }
}

// 整個蓋層的不透明度（畫面內容不變，只改透明度，幾乎不花時間）
void set_overlay_alpha(HWND wnd, double alpha)
{
    BLENDFUNCTION blend{AC_SRC_OVER, 0, static_cast<BYTE>(std::lround(std::clamp(alpha, 0.0, 1.0) * 255)), AC_SRC_ALPHA};
    UpdateLayeredWindow(wnd, nullptr, nullptr, nullptr, nullptr, nullptr, 0, &blend, ULW_ALPHA);
}

double eased(ULONGLONG now, ULONGLONG start)
{
    const auto linear = std::clamp(static_cast<double>(now - start) / duration_ms, 0.0, 1.0);
    return cubic_bezier(0.4, 0.0, 0.2, 1.0, linear);
}

// ESLyric 畫好切換後的畫面了沒（2026-11-12 改）：
// - 封面方塊：跟切換前不一樣、而且跟上一張差不多（封面是靜態圖，兩三張就穩定）
// - 整個展開頁：門檻另外訂（見下面），原本用封面的門檻永遠判斷不到，每次都等滿上限 0.45 秒（2026-11-12 實測）
// 實測 169：點下去後約 0.14 秒內 ESLyric 重排版面佔住主執行緒，一格都畫不了；新畫面約 0.19～0.23 秒定下來
void poll_ready(Transition& transition, ULONGLONG now)
{
    const auto elapsed = now - transition.applied_at;
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
            const bool stage_stable = difference(signature, transition.last_signature) < stage_stable_average;            ready = stage_changed && stage_stable;
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
    // 整個展開頁不用切換後的截圖：舊畫面淡出時直接透出底下真的展開頁（VU 指針、頻譜淡入時也照樣在動）
    if (!transition.whole_stage)
        transition.after = std::move(transition.last_capture);
    transition.last_capture.reset();
}

void on_frame()
{
    if (!g_transition)
        return;

    auto& transition = *g_transition;
    const auto now = GetTickCount64();

    [[maybe_unused]] const auto poll_start = ANIM_LOG_NOW();
    const bool was_ready = transition.ready;
    if (!transition.ready)
        poll_ready(transition, now);
    [[maybe_unused]] const auto poll_ms = was_ready ? 0.0 : ANIM_LOG_NOW() - poll_start;
    if (!was_ready && transition.ready)
        ANIM_LOG_LINE(fmt::format("ready t={:.1f} (since apply {} ms)", ANIM_LOG_NOW(), transition.ready_at - transition.applied_at));

    if (transition.whole_stage) {
        // 畫好之前舊畫面整張蓋著；畫好之後整張淡出
        if (!transition.ready) {
            ANIM_LOG_FRAME(0.0, poll_ms, false);
            return;
        }
        [[maybe_unused]] const auto present_start = ANIM_LOG_NOW();
        const auto progress = eased(now, transition.ready_at);
        set_overlay_alpha(transition.wnd, 1.0 - progress);
        ANIM_LOG_FRAME(ANIM_LOG_NOW() - present_start, poll_ms, true);
        if (progress >= 1.0)
            finish();
        return;
    }

    // 封面方塊：舊的一面一按就開始縮小淡出，新的一面等 ESLyric 畫好才放大淡入
    const auto old_progress = eased(now, transition.applied_at);
    const auto new_progress = transition.ready ? eased(now, transition.ready_at) : 0.0;
    [[maybe_unused]] const auto present_start = ANIM_LOG_NOW();
    present(transition, old_progress, new_progress);
    ANIM_LOG_FRAME(ANIM_LOG_NOW() - present_start, poll_ms, transition.ready);

    if (transition.ready && new_progress >= 1.0)
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
        if (wp == frame_timer_id) {
            on_frame();
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

    const auto wnd = CreateWindowEx(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        window_class_name, L"", WS_POPUP, screen.left, screen.top, wil::rect_width(screen), wil::rect_height(screen),
        owner, nullptr, core_api::get_my_instance(), nullptr);

    if (wnd) {
        // 關掉 Windows 開視窗時的淡入動畫，一出現就要跟原本的畫面一模一樣
        BOOL disable_transitions = TRUE;
        DwmSetWindowAttribute(wnd, DWMWA_TRANSITIONS_FORCEDISABLED, &disable_transitions, sizeof(disable_transitions));
    }
    return wnd;
}

} // namespace

void run(std::function<void()> apply)
{
    // 上一次的動畫還沒跑完又切換：結束它，這次直接切換、不做動畫
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

    const auto cover_in_stage = cover_zoom::cover_rect_in_stage(wil::rect_width(stage), wil::rect_height(stage));
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
    transition->before = capture(screen);
    ANIM_LOG_LINE(fmt::format("captured t={:.1f} size={}x{}", ANIM_LOG_NOW(), wil::rect_width(screen), wil::rect_height(screen)));

    if (!transition->before) {
        apply();
        return;
    }

    transition->before_signature = signature_of(*transition->before, transition->cover);
    transition->base = make_base(*transition->before);
    transition->wnd = create_overlay(owner, screen);

    if (!transition->wnd) {
        apply();
        return;
    }

    // 先把切換前的畫面蓋上去、等它真的合成到螢幕上，才在底下切換（不然會先閃一格切換後的畫面）
    present(*transition, 0.0, 0.0);
    ShowWindow(transition->wnd, SW_SHOWNOACTIVATE);
    [[maybe_unused]] const auto hr = DwmFlush();

    ANIM_LOG_LINE(fmt::format("shown t={:.1f}", ANIM_LOG_NOW()));
    g_transition = std::move(transition);
    apply();
    ANIM_LOG_LINE(fmt::format("applied t={:.1f}", ANIM_LOG_NOW()));
    g_transition->applied_at = GetTickCount64();
    SetTimer(g_transition->wnd, frame_timer_id, frame_interval_ms, nullptr);
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

    auto transition = std::make_unique<Transition>();
    ANIM_LOG_BEGIN("stage");
    transition->whole_stage = true;
    transition->screen = stage;
    transition->cover = RECT{0, 0, wil::rect_width(stage), wil::rect_height(stage)};
    transition->before = capture(stage);
    ANIM_LOG_LINE(fmt::format("captured t={:.1f} size={}x{}", ANIM_LOG_NOW(), wil::rect_width(stage), wil::rect_height(stage)));

    if (!transition->before) {
        apply();
        return;
    }

    transition->before_signature = signature_of(*transition->before, transition->cover);
    transition->wnd = create_overlay(owner, stage);

    if (!transition->wnd) {
        apply();
        return;
    }

    present(*transition, 0.0, 0.0);
    ShowWindow(transition->wnd, SW_SHOWNOACTIVATE);
    [[maybe_unused]] const auto hr = DwmFlush();

    ANIM_LOG_LINE(fmt::format("shown t={:.1f}", ANIM_LOG_NOW()));
    g_transition = std::move(transition);
    apply();
    ANIM_LOG_LINE(fmt::format("applied t={:.1f}", ANIM_LOG_NOW()));
    g_transition->applied_at = GetTickCount64();
    SetTimer(g_transition->wnd, frame_timer_id, frame_interval_ms, nullptr);
}

} // namespace cui::style_transition
