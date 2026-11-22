#include "pch.h"
#include "cover_line.h"

#include "menu_items.h"
#include "next_track_toast.h"
#include "nocturne_segment.h"
#include "nocturne_surface.h"
#include "nocturne_text.h"
#include "now_playing_cover_zoom.h"
#include "popup_dismiss.h"
#include "splitter.h"

namespace cui::cover_line {

namespace {

constexpr wchar_t hotspot_class_name[] = L"columns_ui_cover_line_hotspot";
constexpr wchar_t picker_class_name[] = L"columns_ui_cover_line_picker";
constexpr char eslyric_window_class[] = "uie_eslyric_wnd_class";
// 熱區比那一排的排版高度上下各多一點，比較好點到
constexpr int hotspot_pad_dip = 10;

uint32_t clamp_style(uint32_t value)
{
    return value >= 1 && value <= style_count ? value : style_default;
}

void on_style_changed();

} // namespace

fbh::ConfigUint32 style(GUID{0x4a520ac0, 0x3401, 0x4395, {0xb0, 0x48, 0xf9, 0x73, 0x1c, 0xf0, 0x27, 0x48}}, style_default,
    [](uint32_t, uint32_t, std::optional<uint32_t>) { fb2k::inMainThread([] { on_style_changed(); }); });

namespace {

// ---- 下一首：主執行緒算好存起來，欄位只讀存好的值（欄位可能在別的執行緒算）----
std::mutex next_mutex;
std::string next_title;
std::string next_artist;
bool next_valid{};
bool next_update_scheduled{};
// 框選中晚點再算（見 update_next）：重試的計時器
UINT_PTR next_marquee_retry_timer{};
constexpr UINT next_marquee_retry_ms = 100;
// 封面下那一排「下一首」的編號（style 1）
constexpr uint32_t style_next = 1;
// 挑選卡拖曳中的暫時預覽（1～5；0＝沒有預覽，照設定）：封面下那一排先換成經過的段，放開才存
std::atomic<uint32_t> preview_style{0};

std::string format_handle(const metadb_handle_ptr& handle, const char* script)
{
    titleformat_object::ptr object;
    titleformat_compiler::get()->compile_safe(object, script);
    pfc::string8 out;
    handle->format_title(nullptr, out, object, nullptr);
    return out.c_str();
}

void schedule_next_update();

void CALLBACK on_next_marquee_retry(HWND, UINT, UINT_PTR, DWORD)
{
    KillTimer(nullptr, next_marquee_retry_timer);
    next_marquee_retry_timer = 0;
    schedule_next_update();
}

// 下一首的規則跟右下角的下一首提示同一套（佇列第一首 → 播放跟隨游標 → 下一列；隨機、單曲循環、沒有下一列就沒有）
// 2026-11-22 框選卡頓：開著「播放跟隨游標」時下一首就是焦點那首，框選每掃到一列焦點就變、這裡就叫一次 refresh_layout_fields，
// 那是對正在播的歌送 dispatch_refresh，fb2k 會廣播給所有元件（Album list 走整棵樹重建、標頭清封面快取、ESLyric 整個版面重算）。
// xperf 量到框選 8 秒主執行緒忙 1.95 秒、其中 Album list 0.56 秒。所以：
// - 框選中先不算，每 0.1 秒看一次，放開後只算一次
// - 封面下那一排沒顯示「下一首」時照樣算好存著（欄位讀的是存好的值），但不廣播；換到「下一首」那一排時 on_style_changed 會重算版面
void update_next()
{
    if (uih::ListView::s_is_marquee_active()) {
        if (!next_marquee_retry_timer)
            next_marquee_retry_timer = SetTimer(nullptr, 0, next_marquee_retry_ms, on_next_marquee_retry);
        return;
    }

    std::string title;
    std::string artist;
    bool valid{};
    metadb_handle_ptr now_playing;
    if (play_control::get()->get_now_playing(now_playing)) {
        if (const auto next = next_track_toast::predict_next_track(now_playing); next.is_valid()) {
            title = format_handle(next, "%title%");
            artist = format_handle(next, "[%artist%]");
            valid = !title.empty();
        }
    }

    bool changed{};
    {
        std::scoped_lock lock(next_mutex);
        changed = valid != next_valid || title != next_title || artist != next_artist;
        next_valid = valid;
        next_title = std::move(title);
        next_artist = std::move(artist);
    }
    if (changed && style_for_fields() == style_next)
        main_menu::refresh_layout_fields();
}

// 事件常常一次來好幾個（框選時焦點一直變），併成一次
void schedule_next_update()
{
    if (next_update_scheduled)
        return;
    next_update_scheduled = true;
    fb2k::inMainThread([] {
        next_update_scheduled = false;
        update_next();
    });
}

class NextPlaylistCallback : public playlist_callback_impl_base {
public:
    NextPlaylistCallback()
        : playlist_callback_impl_base(flag_on_items_added | flag_on_items_reordered | flag_on_items_removed
              | flag_on_item_focus_change | flag_on_playback_order_changed | flag_on_playlist_activate)
    {
    }
    void on_items_added(size_t, size_t, metadb_handle_list_cref, const bit_array&) override { schedule_next_update(); }
    void on_items_reordered(size_t, const size_t*, size_t) override { schedule_next_update(); }
    void on_items_removed(size_t, const bit_array&, size_t, size_t) override { schedule_next_update(); }
    void on_item_focus_change(size_t, size_t, size_t) override { schedule_next_update(); }
    void on_playback_order_changed(size_t) override { schedule_next_update(); }
    void on_playlist_activate(size_t, size_t) override { schedule_next_update(); }
};

class NextPlayCallback : public play_callback_impl_base {
public:
    NextPlayCallback() : play_callback_impl_base(flag_on_playback_new_track | flag_on_playback_stop) {}
    void on_playback_new_track(metadb_handle_ptr) override { schedule_next_update(); }
    void on_playback_stop(play_control::t_stop_reason) override { schedule_next_update(); }
};

class NextQueueCallback : public playback_queue_callback {
public:
    void on_changed(t_change_origin) override { fb2k::inMainThread([] { schedule_next_update(); }); }
};

service_factory_single_t<NextQueueCallback> next_queue_callback;

std::unique_ptr<NextPlaylistCallback> g_playlist_callback;
std::unique_ptr<NextPlayCallback> g_play_callback;

class CoverLineInitQuit : public initquit {
public:
    void on_init() override
    {
        g_playlist_callback = std::make_unique<NextPlaylistCallback>();
        g_play_callback = std::make_unique<NextPlayCallback>();
        schedule_next_update();
    }
    void on_quit() override
    {
        g_playlist_callback.reset();
        g_play_callback.reset();
    }
};

initquit_factory_t<CoverLineInitQuit> cover_line_initquit;

// ---- 共用：分層視窗貼圖 ----
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
    if (!memory_dc || !bitmap || !bits)
        return;
    memset(bits, 0, static_cast<size_t>(width) * height * 4);
    {
        Gdiplus::Bitmap canvas(width, height, width * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(bits));
        Gdiplus::Graphics g(&canvas);
        draw(g);
    }
    const auto old = SelectObject(memory_dc.get(), bitmap.get());
    SIZE size{width, height};
    POINT source{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(wnd, screen_dc.get(), const_cast<POINT*>(origin), &size, memory_dc.get(), &source, 0, &blend,
        ULW_ALPHA);
    SelectObject(memory_dc.get(), old);
}

// ---- 挑選卡（2026-11-14 改成分段切換，共用 nocturne_segment.h：使用者在預覽頁選 D＋等級 2 動畫＋按住拖曳＋開關淡入淡出）----
// 一條五段（下一首／等級＋格式／播放統計／聽了多久／常聽排名，後兩段 2026-11-17 加）；點或拖都可以，拖曳經過時封面下那一排先換成那段。
// 2026-11-14 卡片改貼在那一排上方後，使用者說軌道下面那行「實際會出現的字」跟下面那排重複 → 拿掉，只留軌道
namespace picker {

namespace seg = nocturne_segment;

constexpr float shadow_dip = 18.f;
constexpr float pad_dip = 10.f;
// 2026-11-17 三段變五段：每段寬度維持跟原本差不多（約 130 DIP），「等級＋格式」放得下
constexpr float card_w_dip = 680.f;

struct Picker {
    HWND wnd{};
    HWND hotspot{};
    float k{1.f};
    int width{};
    int height{};
    int shadow{};
    Gdiplus::RectF card;
    seg::Row row;
    seg::Presenter presenter;
    seg::FrameClock clock;
    bool tracking{};
    // 開卡時建好、每格重複用：字型、卡片底板（陰影＋實心底，不會動的部分）
    std::unique_ptr<Gdiplus::FontFamily> family;
    std::unique_ptr<Gdiplus::Font> label_font;
    std::vector<uint32_t> background;
    std::vector<uint32_t> frame;
};

std::unique_ptr<Picker>& current()
{
    static std::unique_ptr<Picker> picker;
    return picker;
}

void build_layout(Picker& p)
{
    const auto k = p.k;
    const auto px = [k](float dip) { return std::round(dip * k); };
    p.shadow = static_cast<int>(px(shadow_dip));
    const auto pad = px(pad_dip);
    const auto card_w = px(card_w_dip);
    const auto row_h = px(seg::row_h_dip);
    const auto card_h = 2 * pad + row_h;
    const auto sh = static_cast<float>(p.shadow);
    p.card = {sh, sh - px(4), card_w, card_h};
    p.width = static_cast<int>(card_w + 2 * sh);
    p.height = static_cast<int>(card_h + 2 * sh);

    p.row.labels.clear();
    for (const auto* name : nocturne_text::cover_line_style_names)
        p.row.labels.emplace_back(name);
    p.row.selected = static_cast<int>(clamp_style(style.get())) - 1;
    seg::layout(p.row, {p.card.X + pad, p.card.Y + pad, card_w - 2 * pad, row_h}, k);
}

// 不會動的部分先畫好：字型、陰影＋實心底
void build_static(Picker& p)
{
    const auto k = p.k;
    p.family = seg::theme_family();
    const auto style_flags = seg::regular_style(*p.family);
    p.label_font = std::make_unique<Gdiplus::Font>(p.family.get(), seg::label_font_dip * k, style_flags, Gdiplus::UnitPixel);

    p.background.assign(static_cast<size_t>(p.width) * p.height, 0);
    Gdiplus::Bitmap canvas(p.width, p.height, p.width * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(p.background.data()));
    Gdiplus::Graphics g(&canvas);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    nocturne_surface::paint_panel(g, p.card, nocturne_surface::card_radius_dip * k, p.shadow);
}

void render(Picker& p)
{
    const auto k = p.k;
    p.frame = p.background;
    {
        Gdiplus::Bitmap canvas(p.width, p.height, p.width * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(p.frame.data()));
        Gdiplus::Graphics g(&canvas);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        seg::draw(g, p.row, *p.label_font, k);
    }
    p.presenter.present(p.wnd, p.frame.data(), p.width, p.height, {p.card.X, p.card.Y});
}

// 拖曳中的暫時預覽：封面下那一排先換成經過的段（0＝不預覽，照設定）
void set_preview(uint32_t value)
{
    if (preview_style.exchange(value) != value)
        main_menu::refresh_layout_fields();
}

void animate(Picker& p)
{
    p.clock.ensure(p.wnd);
}

// 時脈每一格（螢幕更新率）：更新動畫、畫一次；拖曳中也一直跑，滑鼠移動只改狀態、在這裡畫
void tick(Picker& p)
{
    const auto now = seg::Clock::now();
    auto running = seg::update(p.row, now);
    running = p.presenter.update(now) || running;
    running = running || p.row.pressed;
    render(p);
    p.clock.frame_done();
    if (p.presenter.closed()) {
        DestroyWindow(p.wnd);
        return;
    }
    if (!running)
        p.clock.stop();
}

// 放開或點擊選定：存設定（存的值一樣就不動），拖曳預覽收掉
void finish_pointer(Picker& p)
{
    seg::mouse_up(p.row, [](int index) {
        const auto value = static_cast<uint32_t>(index) + 1;
        if (clamp_style(style.get()) != value)
            style = value;
    });
    set_preview(0);
    animate(p);
}

void begin_close(Picker& p)
{
    if (p.row.pressed) {
        seg::cancel_drag(p.row);
        set_preview(0);
    }
    if (!seg::animations_enabled()) {
        DestroyWindow(p.wnd);
        return;
    }
    p.presenter.begin_close();
    animate(p);
}

LRESULT CALLBACK window_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    // 點外面、切走程式、失去啟用、熱區藏起來（收合、換成純封面、改回詳細）時關（見 popup_dismiss.h）
    if (popup_dismiss::handle_message(wnd, msg, wp, lp))
        return 0;

    auto& picker = current();
    if (!picker || picker->wnd != wnd)
        return DefWindowProc(wnd, msg, wp, lp);
    auto& p = *picker;
    if (msg == seg::FrameClock::message()) {
        tick(p);
        return 0;
    }
    const auto x = static_cast<float>(GET_X_LPARAM(lp));
    const auto y = static_cast<float>(GET_Y_LPARAM(lp));

    switch (msg) {
    case WM_MOUSEMOVE: {
        if (p.presenter.closing())
            return 0;
        if (!p.tracking) {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, wnd, 0};
            p.tracking = TrackMouseEvent(&track) != FALSE;
        }
        if (p.row.pressed) {
            seg::mouse_move(p.row, x, p.k, [](int index) { set_preview(static_cast<uint32_t>(index) + 1); });
            animate(p);
            return 0;
        }
        const auto hover = seg::cell_at(p.row, x, y);
        if (hover != p.row.hover) {
            p.row.hover = hover;
            render(p);
        }
        SetCursor(LoadCursor(nullptr, hover >= 0 ? IDC_HAND : IDC_ARROW));
        return 0;
    }
    case WM_SETCURSOR:
        SetCursor(LoadCursor(nullptr, p.row.hover >= 0 || p.row.pressed ? IDC_HAND : IDC_ARROW));
        return TRUE;
    case WM_MOUSELEAVE:
        p.tracking = false;
        if (p.row.hover != -1) {
            p.row.hover = -1;
            render(p);
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (!p.presenter.closing() && seg::mouse_down(p.row, x, y)) {
            SetCapture(wnd);
            animate(p);
        }
        return 0;
    case WM_LBUTTONUP:
        if (p.row.pressed) {
            finish_pointer(p);
            if (GetCapture() == wnd)
                ReleaseCapture();
        }
        return 0;
    case WM_CAPTURECHANGED:
        // 拖到一半被搶走捕捉：亮底回到原本的選擇
        if (p.row.pressed) {
            seg::cancel_drag(p.row);
            set_preview(0);
            animate(p);
        }
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE)
            PostMessage(wnd, WM_CLOSE, 0, 0);
        return 0;
    case WM_CLOSE:
        begin_close(p);
        return 0;
    case WM_DESTROY:
        p.clock.stop();
        if (p.row.pressed)
            set_preview(0);
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

// 開卡片：貼在封面下一排的正上方（位置規則見 nocturne_segment::place_card）
void open(HWND hotspot, POINT at)
{
    close();
    static const auto class_registered = [] {
        WNDCLASSEX window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = window_proc;
        window_class.hInstance = core_api::get_my_instance();
        window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
        window_class.lpszClassName = picker_class_name;
        return RegisterClassEx(&window_class) != 0;
    }();
    if (!class_registered)
        return;

    seg::refresh_settings();
    auto p = std::make_unique<Picker>();
    p->hotspot = hotspot;
    p->k = static_cast<float>(uih::scale_dpi_value(1000)) / 1000.f;
    build_layout(*p);
    build_static(*p);

    // 位置（2026-11-14 使用者要求）：貼在封面下一排的正上方、左右對齊那一排的中間，
    // 不再蓋住那一排（拖滑塊時的即時預覽才看得到），也不會壓到下面的進度條；範圍限制在 fb2k 視窗內
    POINT prefer = at;
    RECT line{};
    if (GetWindowRect(hotspot, &line)) {
        const auto gap = static_cast<LONG>(std::lround(8.f * p->k));
        prefer = {(line.left + line.right) / 2 - static_cast<LONG>(std::lround(p->card.Width / 2.f)),
            line.top - gap - static_cast<LONG>(std::lround(p->card.Height))};
    }
    const auto [x, y] = seg::place_card(hotspot, prefer, at.y, p->card);

    const auto owner = GetAncestor(hotspot, GA_ROOT);
    current() = std::move(p);
    const auto wnd = CreateWindowEx(WS_EX_LAYERED | WS_EX_TOOLWINDOW, picker_class_name, L"", WS_POPUP, x, y,
        current()->width, current()->height, owner, nullptr, core_api::get_my_instance(), nullptr);
    if (!wnd) {
        current().reset();
        return;
    }
    auto& picker = *current();
    picker.wnd = wnd;
    popup_dismiss::attach(wnd, [hotspot] { return IsWindow(hotspot) && IsWindowVisible(hotspot); });
    picker.presenter.begin_open();
    render(picker);
    ShowWindow(wnd, SW_SHOW);
    SetForegroundWindow(wnd);
    SetFocus(wnd);
    animate(picker);
}

// 樣式從別處換了（設定檔、另一個視窗）：卡片開著就把亮底滑過去
void refresh()
{
    auto& picker = current();
    if (!picker || !picker->wnd || picker->row.pressed)
        return;
    auto& p = *picker;
    const auto target = static_cast<int>(clamp_style(style.get())) - 1;
    if (target != p.row.selected) {
        seg::select(p.row, target, true);
        animate(p);
    }
    render(p);
}

} // namespace picker

// ---- 右鍵熱區 ----
std::vector<HWND>& live_windows()
{
    static std::vector<HWND> windows;
    return windows;
}

// 每個熱區記著上一次的播放列位置（分割面板座標），設定或模式改了用它重排
std::unordered_map<HWND, RECT>& bar_rects()
{
    static std::unordered_map<HWND, RECT> rects;
    return rects;
}

RECT compute_layout(const RECT& bar_rect)
{
    if (bar_rect.right <= bar_rect.left || !cover_zoom::compact_info.get())
        return {};

    // 只在展開頁完全展開、而且左邊有封面（純封面模式沒有這一排）時蓋
    RECT stage_screen{};
    HWND owner{};
    if (!panels::splitter::FlatSplitterPanel::s_get_now_playing_stage(stage_screen, owner))
        return {};
    if (main_menu::get_now_playing_mode() == main_menu::NowPlayingMode::cover)
        return {};

    const auto stage_width = bar_rect.right - bar_rect.left;
    const auto stage_height = bar_rect.top;
    const auto cover = cover_zoom::cover_rect_in_stage(stage_width, stage_height);
    const auto top = cover_zoom::info_top_in_stage(stage_width, stage_height);
    const auto bottom = cover_zoom::info_bottom_in_stage(stage_width, stage_height);
    if (!cover || !top || !bottom)
        return {};

    const auto pad = uih::scale_dpi_value(hotspot_pad_dip);
    return RECT{bar_rect.left + cover->left, std::max(0, *top - pad), bar_rect.left + cover->right,
        std::min(static_cast<int>(stage_height), *bottom + pad)};
}

// 熱區底下那個 ESLyric 視窗（右鍵以外的滑鼠訊息轉給它）
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

void forward_client_message(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    POINT screen{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    ClientToScreen(wnd, &screen);
    const auto target = find_eslyric_under(wnd, screen);
    if (!target)
        return;
    ScreenToClient(target, &screen);
    SendMessage(target, msg, wp, MAKELPARAM(screen.x, screen.y));
}

void render_hotspot(HWND wnd)
{
    RECT client{};
    GetClientRect(wnd, &client);
    if (client.right <= 0 || client.bottom <= 0)
        return;
    // 幾乎看不見的底（透明度 1/255）：整塊收得到滑鼠，看起來跟沒蓋一樣
    present_layered(wnd, nullptr, client.right, client.bottom, [&](Gdiplus::Graphics& g) {
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
    render_hotspot(wnd);
    if (!visible)
        SetWindowPos(wnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

LRESULT CALLBACK hotspot_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
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
    case WM_SETCURSOR:
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        return TRUE;
    // 右鍵：開挑選卡（放開時開，按下那一下吃掉，免得 ESLyric 也跳選單）
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
    case WM_CONTEXTMENU:
        return 0;
    case WM_RBUTTONUP: {
        // 用這一下右鍵的座標（實際點擊時跟游標位置一樣；測試送訊息時卡片才會開在 fb2k 視窗裡）
        POINT at{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ClientToScreen(wnd, &at);
        picker::open(wnd, at);
        return 0;
    }
    // 其他按鍵與滾輪照常交給底下的 ESLyric
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
        forward_client_message(wnd, msg, wp, lp);
        return 0;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        const POINT screen{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (const auto target = find_eslyric_under(wnd, screen))
            return SendMessage(target, msg, wp, lp);
        return 0;
    }
    }
    return DefWindowProc(wnd, msg, wp, lp);
}

void on_style_changed()
{
    main_menu::refresh_layout_fields();
    picker::refresh();
}

} // namespace

uint32_t style_for_fields()
{
    if (const auto preview = preview_style.load())
        return preview;
    return clamp_style(style.get());
}

bool next_text_for_fields(bool artist, std::string& out)
{
    std::scoped_lock lock(next_mutex);
    if (!next_valid)
        return false;
    out = artist ? next_artist : next_title;
    return !out.empty();
}

HWND create(HWND parent)
{
    static const auto class_registered = [] {
        WNDCLASSEX window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.style = CS_DBLCLKS;
        window_class.lpfnWndProc = hotspot_proc;
        window_class.hInstance = core_api::get_my_instance();
        window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
        window_class.lpszClassName = hotspot_class_name;
        return RegisterClassEx(&window_class) != 0;
    }();

    if (!class_registered)
        return nullptr;

    return CreateWindowEx(WS_EX_LAYERED, hotspot_class_name, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, parent,
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

} // namespace cui::cover_line
