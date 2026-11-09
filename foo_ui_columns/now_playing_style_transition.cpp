#include "pch.h"

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
    ULONGLONG animation_start{};
    bool animating{};
};

std::unique_ptr<Transition> g_transition;

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

// 畫一格：progress 0＝完全是切換前、1＝完全是切換後
void present(Transition& transition, double progress)
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

        const auto p = static_cast<float>(progress);
        if (transition.before)
            draw_scaled(graphics, *transition.before, transition.cover, 1.f + (old_end_scale - 1.f) * p, 1.f - p);
        if (transition.after)
            draw_scaled(graphics, *transition.after, transition.cover, new_start_scale + (1.f - new_start_scale) * p, p);
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
    if (transition->wnd) {
        KillTimer(transition->wnd, frame_timer_id);
        DestroyWindow(transition->wnd);
    }
}

void on_frame()
{
    if (!g_transition)
        return;

    auto& transition = *g_transition;
    const auto now = GetTickCount64();

    if (!transition.animating) {
        const auto elapsed = now - transition.applied_at;
        if (elapsed < settle_min_ms)
            return;

        // ESLyric 畫好了沒：跟切換前不一樣，而且跟上一張差不多
        bool ready = false;
        if (auto shot = capture(transition.screen)) {
            auto signature = signature_of(*shot, transition.cover);
            const bool changed = difference(signature, transition.before_signature) > changed_threshold;
            const bool stable = difference(signature, transition.last_signature) < stable_threshold;
            transition.last_signature = std::move(signature);
            transition.last_capture = std::move(shot);
            ready = changed && stable;
        }

        if (!ready && elapsed < settle_limit_ms)
            return;

        if (!transition.last_capture) {
            finish();
            return;
        }

        transition.after = std::move(transition.last_capture);
        transition.animating = true;
        transition.animation_start = now;
    }

    const auto linear = std::clamp(static_cast<double>(now - transition.animation_start) / duration_ms, 0.0, 1.0);
    present(transition, cubic_bezier(0.4, 0.0, 0.2, 1.0, linear));

    if (linear >= 1.0)
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

    auto transition = std::make_unique<Transition>();
    transition->screen = screen;
    transition->cover = cover_screen;
    OffsetRect(&transition->cover, -screen.left, -screen.top);
    transition->before = capture(screen);

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
    present(*transition, 0.0);
    ShowWindow(transition->wnd, SW_SHOWNOACTIVATE);
    [[maybe_unused]] const auto hr = DwmFlush();

    g_transition = std::move(transition);
    apply();
    g_transition->applied_at = GetTickCount64();
    SetTimer(g_transition->wnd, frame_timer_id, frame_interval_ms, nullptr);
}

} // namespace cui::style_transition
