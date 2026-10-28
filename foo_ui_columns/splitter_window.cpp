#include "pch.h"
#include "splitter.h"
#include "menu_items.h"

namespace cui::panels::splitter {

ui_extension::window_host_factory<FlatSplitterPanel::FlatSplitterPanelHost> g_splitter_host_vert;

unsigned FlatSplitterPanel::g_count = 0;
wil::unique_hfont FlatSplitterPanel::g_font_menu_horizontal;
wil::unique_hfont FlatSplitterPanel::g_font_menu_vertical;
ULONGLONG FlatSplitterPanel::s_last_now_playing_toggle_tick = 0;
std::atomic<bool> FlatSplitterPanel::s_now_playing_expanded_for_fields{false};

namespace {

constexpr UINT_PTR now_playing_bar_subclass_id = 1;

// 自編版新增（展開頁動畫）：跟 CSS 的 cubic-bezier(x1, y1, x2, y2) 相同，給時間比例 x 求進度
double cubic_bezier(double x1, double y1, double x2, double y2, double x)
{
    // 起點 (0,0)、終點 (1,1) 的三次貝茲曲線，在參數 t 的值
    const auto sample = [](double p1, double p2, double t) {
        return ((1 + 3 * p1 - 3 * p2) * t + (3 * p2 - 6 * p1)) * t * t + 3 * p1 * t;
    };

    // x 隨 t 單調遞增，用二分法找出對應的 t
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

} // namespace

void FlatSplitterPanel::insert_panel(size_t index, const uie::splitter_item_t* p_item)
{
    if (index > m_panels.size())
        return;

    auto temp = std::make_shared<Panel>();
    temp->set_from_splitter_item(p_item);
    m_panels.insert(m_panels.begin() + index, std::move(temp));

    if (get_wnd())
        refresh_children();
}

void FlatSplitterPanel::replace_panel(size_t index, const uie::splitter_item_t* p_item)
{
    if (index >= m_panels.size())
        return;

    if (get_wnd())
        m_panels[index]->destroy();

    auto temp = std::make_shared<Panel>();
    temp->set_from_splitter_item(p_item);

    m_panels[index] = std::move(temp);

    if (get_wnd())
        refresh_children();
}

void FlatSplitterPanel::reorder_panels(const size_t* order, size_t count)
{
    if (count != m_panels.size())
        return;

    m_panels = std::span(order, count) | ranges::views::transform([this](size_t index) { return m_panels[index]; })
        | ranges::to<std::vector>();

    if (get_wnd())
        on_size_changed();
}

void FlatSplitterPanel::destroy_children()
{
    const auto count = m_panels.size();
    for (size_t n = 0; n < count; n++) {
        std::shared_ptr<Panel> pal = m_panels[n];
        if (pal->m_child.is_valid()) {
            pal->m_child->destroy_window();
            pal->m_wnd_child = nullptr;
            DestroyWindow(pal->m_wnd);
            pal->m_wnd = nullptr;
            pal->m_child.release();
        }
    }
}

std::vector<FlatSplitterPanel::Panel::Ptr>::iterator FlatSplitterPanel::find_panel_by_container_wnd(HWND wnd)
{
    return std::ranges::find_if(m_panels, [wnd](const Panel::Ptr& panel) { return panel->m_wnd == wnd; });
}

std::vector<FlatSplitterPanel::Panel::Ptr>::iterator FlatSplitterPanel::find_panel_by_panel_wnd(HWND wnd)
{
    return std::ranges::find_if(m_panels, [wnd](const Panel::Ptr& panel) { return panel->m_wnd_child == wnd; });
}

void FlatSplitterPanel::refresh_children()
{
    auto _ = wil::scope_exit(
        [&, previous_value{m_refresh_children_in_progress}] { m_refresh_children_in_progress = previous_value; });

    m_refresh_children_in_progress = true;

    // Create a copy to protect against window_host::relinquish_ownership() calls
    // during iteration
    const auto panels = m_panels;
    std::vector<Panel::Ptr> created_panels;

    for (auto&& panel : panels) {
        if (panel->m_wnd)
            continue;

        uie::window_ptr p_ext = panel->m_child;

        bool b_new = false;

        if (!p_ext.is_valid()) {
            create_by_guid(panel->m_guid, p_ext);
            b_new = true;
        }

        if (p_ext.is_valid() && p_ext->is_available(m_window_host)) {
            pfc::string8 name;
            if (panel->m_use_custom_title) {
                name = panel->m_custom_title;
            } else {
                if (!p_ext->get_short_name(name))
                    p_ext->get_name(name);
            }

            HWND wnd_host = panel->m_container.create(m_wnd);
            panel->m_container.set_window_ptr(this);

            uSetWindowText(wnd_host, name);

            if (wnd_host) {
                if (b_new) {
                    try {
                        abort_callback_dummy p_abort;
                        p_ext->set_config_from_ptr(
                            panel->m_child_data.get_ptr(), panel->m_child_data.get_size(), p_abort);
                    } catch (const exception_io& e) {
                        console::formatter formatter;
                        formatter << "Error setting panel config: " << e.what();
                    }
                }

                HWND wnd_panel = p_ext->create_or_transfer_window(wnd_host, m_window_host);
                if (wnd_panel) {
                    SetWindowLongPtr(wnd_panel, GWL_STYLE, GetWindowLongPtr(wnd_panel, GWL_STYLE) | WS_CLIPSIBLINGS);
                    MINMAXINFO mmi{};
                    mmi.ptMaxTrackSize.x = MAXLONG;
                    mmi.ptMaxTrackSize.y = MAXLONG;
                    SendMessage(wnd_panel, WM_GETMINMAXINFO, 0, (LPARAM)&mmi);
                    helpers::clip_minmaxinfo(mmi);

                    panel->m_wnd = wnd_host;
                    panel->m_wnd_child = wnd_panel;
                    panel->m_child = p_ext;

                    // 自編版新增：Layout 按 Apply 會重建面板，所以在建立子視窗這裡掛，不在切換命令裡掛
                    if (panel->has_custom_title(now_playing_bar_title))
                        SetWindowSubclass(wnd_panel, s_now_playing_bar_subclass_proc, now_playing_bar_subclass_id, 0);

                    panel->m_size_limits.min_height = mmi.ptMinTrackSize.y;
                    panel->m_size_limits.min_width = mmi.ptMinTrackSize.x;
                    panel->m_size_limits.max_width = mmi.ptMaxTrackSize.x;
                    panel->m_size_limits.max_height = mmi.ptMaxTrackSize.y;
                } else {
                    panel->m_container.destroy();
                }
            }
        }

        created_panels.emplace_back(panel);
    }

    on_size_changed();

    if (IsWindowVisible(get_wnd())) {
        // 自編版新增：展開頁右側的容器只由 on_size_changed 決定顯示與否，這裡不強制顯示
        const auto now_playing_side = get_now_playing_side_panel();

        for (auto&& panel : created_panels) {
            if (!panel->m_wnd_child)
                continue;

            ShowWindow(panel->m_wnd_child, SW_SHOWNORMAL);

            if (panel != now_playing_side)
                ShowWindow(panel->m_wnd, SW_SHOWNORMAL);
        }

        get_host()->on_size_limit_change(get_wnd(), uie::size_limit_all);
        RedrawWindow(get_wnd(), nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN);
    }
}

void FlatSplitterPanel::on_size_changed(unsigned width, unsigned height)
{
    pfc::list_t<unsigned> sizes;
    get_panels_sizes(width, height, sizes);
    const auto count = m_panels.size();
    // 自編版新增：播放列的容器另外處理，見 s_toggle_now_playing；展開頁右側（VU 表）排完播放列後再疊上去
    const auto now_playing_bar = get_now_playing_bar_panel();
    const auto now_playing_side = get_now_playing_side_panel();
    RECT now_playing_bar_rect{};

    RedrawWindow(get_wnd(), nullptr, nullptr, RDW_INVALIDATE);

    HDWP dwp = BeginDeferWindowPos(gsl::narrow<int>(m_panels.size()));
    if (dwp) {
        unsigned size_cumulative = 0;
        for (size_t n = 0; n < count; n++) {
            if (m_panels[n]->m_child.is_valid() && m_panels[n]->m_wnd) {
                unsigned size = sizes[n];

                unsigned x = get_orientation() == horizontal ? size_cumulative : 0;
                unsigned y = get_orientation() == horizontal ? 0 : size_cumulative;
                unsigned cx = get_orientation() == horizontal ? size - get_panel_divider_size(n) : width;
                unsigned cy = get_orientation() == horizontal ? height : size - get_panel_divider_size(n);

                if (m_panels[n] == now_playing_bar) {
                    // 排版算給播放列的位置留著當收合時的視窗區域；容器本身撐滿整個分割面板、疊在最上層
                    now_playing_bar_rect = {static_cast<LONG>(x), static_cast<LONG>(y), static_cast<LONG>(x + cx),
                        static_cast<LONG>(y + cy)};
                    m_now_playing_bar_rect = now_playing_bar_rect;
                    dwp = DeferWindowPos(dwp, m_panels[n]->m_wnd, HWND_TOP, 0, 0, width, height, 0);
                } else if (m_panels[n] == now_playing_side) {
                    // 位置在下面另外設定
                } else {
                    dwp = DeferWindowPos(dwp, m_panels[n]->m_wnd, HWND_BOTTOM, x, y, cx, cy, 0);
                }

                size_cumulative += size;
            }
        }
        EndDeferWindowPos(dwp);
    }

    if (now_playing_bar && now_playing_bar->m_wnd) {
        // 收合：只露出排版給播放列的位置；展開：不設區域，整塊蓋住主內容（區域物件交給系統管理）
        // 自編版（展開頁動畫）：動畫進行中改用這一刻的區域，結束時由計時器再排一次
        if (m_now_playing_animating) {
            const auto animation_rect = get_now_playing_animation_rect(width, height);
            SetWindowRgn(now_playing_bar->m_wnd, CreateRectRgnIndirect(&animation_rect), TRUE);
        } else {
            SetWindowRgn(now_playing_bar->m_wnd,
                m_now_playing_expanded ? nullptr : CreateRectRgnIndirect(&now_playing_bar_rect), TRUE);
        }
        s_now_playing_expanded_for_fields = m_now_playing_expanded;
    }

    if (now_playing_side && now_playing_side->m_wnd) {
        // 展開且樣式 2 時疊在播放列（ESLyric）上，對齊 main.xml 樣式 2 的右側面板：
        // 寬 640、離右邊 100，在展開區（扣掉播放列高度）垂直置中；高度照 VU 表外觀 Modern antonyb 的比例 830×199
        // main.xml 的右側面板寬度、右留白改了，這裡要跟著改
        // 展開動畫進行中先不顯示，等展開頁整塊出來再疊上去
        if (m_now_playing_expanded && !m_now_playing_animating && main_menu::get_now_playing_style() == 2) {
            const int stage_height = gsl::narrow<int>(height) - (now_playing_bar_rect.bottom - now_playing_bar_rect.top);
            const int side_width = uih::scale_dpi_value(640);
            const int side_height = MulDiv(side_width, 199, 830);
            const int x = gsl::narrow<int>(width) - uih::scale_dpi_value(100) - side_width;
            const int y = std::max(0, (stage_height - side_height) / 2);
            SetWindowPos(
                now_playing_side->m_wnd, HWND_TOP, x, y, side_width, side_height, SWP_SHOWWINDOW | SWP_NOACTIVATE);
        } else {
            SetWindowPos(now_playing_side->m_wnd, nullptr, 0, 0, 0, 0,
                SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    RedrawWindow(get_wnd(), nullptr, nullptr, RDW_UPDATENOW);
}

void FlatSplitterPanel::on_size_changed()
{
    RECT rc;
    GetClientRect(m_wnd, &rc);
    on_size_changed(rc.right, rc.bottom);
}

bool FlatSplitterPanel::find_by_divider_pt(POINT& pt, size_t& p_out)
{
    for (const auto [n, p_item] : ranges::views::enumerate(m_panels)) {
        if (!p_item->m_wnd_child)
            continue;

        RECT rc_area;
        GetRelativeRect(p_item->m_wnd_child, m_wnd, &rc_area);

        if (PtInRect(&rc_area, pt))
            return false;

        bool in_divider = false;
        if (get_orientation() == vertical) {
            in_divider = (pt.y >= rc_area.bottom) && (pt.y < (rc_area.bottom + (LONG)get_panel_divider_size(n)));
        } else {
            in_divider = pt.x >= rc_area.right && pt.x < (rc_area.right + (LONG)get_panel_divider_size(n));
        }
        if (in_divider) {
            p_out = n;
            return true;
        }
    }
    return false;
}

bool FlatSplitterPanel::test_divider_pt(const POINT& pt, size_t index)
{
    size_t divider_index;
    POINT pt2 = pt;
    if (find_by_divider_pt(pt2, divider_index)) {
        return divider_index == index || (index && divider_index == index - 1);
    }
    return false;
}

void FlatSplitterPanel::save_sizes(unsigned width, unsigned height)
{
    pfc::list_t<unsigned> sizes;
    get_panels_sizes(width, height, sizes);
    const auto count = m_panels.size();

    for (size_t n = 0; n < count; n++) {
        if (!m_panels[n]->m_hidden)
            m_panels[n]->m_size = sizes[n] - get_panel_divider_size(n);
    }
}

void FlatSplitterPanel::save_sizes()
{
    RECT rc;
    GetClientRect(m_wnd, &rc);
    save_sizes(rc.right, rc.bottom);
}

void FlatSplitterPanel::get_panels_sizes(
    unsigned client_width, unsigned client_height, pfc::list_base_t<unsigned>& p_out)
{
    struct SizeInfo {
        int size{};
        bool is_sized{};
        int parts{};
    };

    if (m_panels.empty())
        return;

    const auto count = m_panels.size();
    std::vector<SizeInfo> size_infos(count);

    const auto caption_size = g_get_caption_size();

    int available_size = get_orientation() == horizontal ? client_width : client_height;
    int available_parts{};
    const auto now_playing_bar = get_now_playing_bar_panel();

    for (auto&& [index, panel, size_info] :
        ranges::views::zip(ranges::views::iota(size_t{}, count), m_panels, size_infos)) {
        const auto panel_divider_size = get_panel_divider_size(index);

        const auto size = [&] {
            if (panel->m_hidden)
                return 0;

            // 自編版新增：播放列高度固定，跟 ESLyric 佈局一致（見 now_playing_bar_height）
            if (panel == now_playing_bar && get_orientation() == vertical)
                return uih::scale_dpi_value(now_playing_bar_height);

            return std::min(max_panel_dimension, static_cast<int>(panel->m_size.get_scaled_value()));
        }();

        available_size -= size + panel_divider_size;
        size_info.size = size + panel_divider_size;
        size_info.parts = [&] {
            if (panel->m_locked || panel->m_hidden)
                return 0;

            if (settings::use_legacy_splitter_child_sizing)
                return 1;

            return size;
        }();

        available_parts += size_info.parts;
    }

    do {
        auto this_pass_available_parts = available_parts;
        auto this_pass_available_size = available_size;

        for (auto&& [index, panel, size_info] :
            ranges::views::zip(ranges::views::iota(size_t{}, count), m_panels, size_infos)) {
            if (size_info.is_sized)
                continue;

            const auto panel_divider_size = get_panel_divider_size(index);
            const auto panel_caption_size
                = (get_orientation() != panel->m_caption_orientation && panel->m_show_caption) ? caption_size : 0;

            int size = size_info.size;

            int adjustment = this_pass_available_parts
                ? MulDiv(this_pass_available_size, size_info.parts, this_pass_available_parts)
                : 0;
            this_pass_available_parts -= size_info.parts;
            this_pass_available_size -= adjustment;

            if (adjustment < 0 && (size > panel_divider_size ? size - panel_divider_size : 0) < adjustment * -1) {
                adjustment = (size > panel_divider_size ? size - panel_divider_size : 0) * -1;
                size_info.is_sized = true;
            }

            const int unadjusted = size;

            const auto hidden = panel->m_hidden;

            size += adjustment;

            auto min_size = [&] {
                if (hidden)
                    return 0;

                return get_orientation() == horizontal ? panel->m_size_limits.min_width
                                                       : panel->m_size_limits.min_height;
            }();

            min_size += panel_divider_size + panel_caption_size;

            auto max_size = [&] {
                if (hidden)
                    return 0;

                return get_orientation() == horizontal ? panel->m_size_limits.max_width
                                                       : panel->m_size_limits.max_height;
            }();

            max_size += panel_divider_size + panel_caption_size;

            if (get_orientation() == horizontal && panel->m_show_toggle_area && !panel->m_autohide) {
                ++max_size;
                ++min_size;
            }

            if (size < min_size) {
                size = min_size;
                size_info.is_sized = true;
            } else if (size > max_size) {
                size = max_size;
                size_info.is_sized = true;
            }

            if (panel->m_locked || hidden)
                size_info.is_sized = true;

            if (size_info.is_sized)
                available_parts -= size_info.parts;

            available_size -= size - unadjusted;
            size_info.size = size;
        }
    } while (available_parts > 0 && available_size != 0);

    for (const auto& size_info : size_infos)
        p_out.add_item(size_info.size);
}

bool FlatSplitterPanel::can_resize_divider(size_t index) const
{
    size_t count_left = 0;
    size_t count_right = 0;
    for (size_t i = 0, count = m_panels.size(); i < count; i++) {
        if (can_resize_panel(i)) {
            if (i <= index)
                count_left++;
            else
                count_right++;
        }
    }
    return count_left && count_right;
}

bool FlatSplitterPanel::can_resize_panel(size_t index) const
{
    const auto& panel = m_panels[index];

    if (!settings::allow_locked_panel_resizing && panel->m_locked)
        return false;

    if (panel->m_wnd_child) {
        RECT rc_window;
        GetWindowRect(panel->m_wnd_child, &rc_window);

        if (get_orientation() == vertical && wil::rect_height(rc_window) == panel->m_size_limits.max_height
            && wil::rect_height(rc_window) == panel->m_size_limits.min_height)
            return false;

        if (get_orientation() == horizontal && wil::rect_width(rc_window) == panel->m_size_limits.max_width
            && wil::rect_width(rc_window) == panel->m_size_limits.min_width)
            return false;
    }

    return true;
}

int FlatSplitterPanel::override_size(const size_t panel, const int delta)
{
    struct SizeInfo {
        int min_size{};
        int max_size{};
        int size{};
    };

    if (m_panels.empty())
        return 0;

    save_sizes();

    const auto count = m_panels.size();

    if (panel + 1 >= count)
        return 0;

    const auto default_caption_size = g_get_caption_size();
    std::vector<SizeInfo> size_infos(count);

    for (auto&& [panel, size_info] : ranges::views::zip(m_panels, size_infos)) {
        auto panel_caption_size
            = panel->m_show_caption && panel->m_caption_orientation != get_orientation() ? default_caption_size : 0;
        auto min_size = panel->m_hidden     ? 0
            : get_orientation() == vertical ? panel->m_size_limits.min_height
                                            : panel->m_size_limits.min_width;
        auto max_size = panel->m_hidden     ? 0
            : get_orientation() == vertical ? panel->m_size_limits.max_height
                                            : panel->m_size_limits.max_width;

        min_size += panel_caption_size;
        max_size += panel_caption_size;

        if (get_orientation() == horizontal && panel->m_show_toggle_area && !panel->m_autohide) {
            ++max_size;
            ++min_size;
            ++panel_caption_size;
        }

        size_info.min_size = min_size;
        size_info.max_size = max_size;
        size_info.size = panel->m_hidden ? panel_caption_size : panel->m_size.get_scaled_value();
    }

    const auto is_moving_up_or_left = delta < 0;
    const auto is_moving_down_or_right = delta > 0;

    if (is_moving_up_or_left) {
        int abs_diff{};
        const int available_diff = abs(delta);

        auto index = panel + 1;
        while (index < count && abs_diff < available_diff) {
            auto size = size_infos[index].size + (available_diff - abs_diff);

            const auto min_size = size_infos[index].min_size;
            const auto max_size = size_infos[index].max_size;

            if (size < min_size)
                size = min_size;
            else if (size > max_size)
                size = max_size;

            abs_diff += size - size_infos[index].size;
            ++index;
        }

        index = panel + 1;
        int obtained_above{};

        while (index > 0 && obtained_above < abs_diff) {
            --index;

            auto size = abs_diff - obtained_above > size_infos[index].size
                ? 0
                : size_infos[index].size - (abs_diff - obtained_above);
            const auto min_size = size_infos[index].min_size;
            const auto max_size = size_infos[index].max_size;

            if (size < min_size)
                size = min_size;
            else if (size > max_size)
                size = max_size;

            obtained_above += size_infos[index].size - size;
            size_infos[index].size = size;

            if (!m_panels[index]->m_hidden)
                m_panels[index]->m_size = size;
        }

        index = panel;
        auto to_reallocate = obtained_above;

        while (index < count - 1 && to_reallocate != 0) {
            ++index;
            auto size = size_infos[index].size;

            const auto min_size = size_infos[index].min_size;
            const auto max_size = size_infos[index].max_size;

            size += to_reallocate;

            if (size < min_size)
                size = min_size;
            else if (size > max_size)
                size = max_size;

            to_reallocate -= size - size_infos[index].size;
            size_infos[index].size = size;

            if (!m_panels[index]->m_hidden)
                m_panels[index]->m_size = size;
        }
        return abs(delta) - obtained_above;
    }

    if (is_moving_down_or_right) {
        int abs_diff{};
        const int available_diff = abs(delta);

        auto index = panel + 1;
        while (index > 0 && abs_diff < available_diff) {
            --index;

            auto size = size_infos[index].size + (available_diff - abs_diff);
            const auto min_size = size_infos[index].min_size;
            const auto max_size = size_infos[index].max_size;

            if (size < min_size)
                size = min_size;
            else if (size > max_size)
                size = max_size;

            abs_diff += size - size_infos[index].size;
        }
        index = panel;
        int obtained_below{};

        while (index < count - 1 && obtained_below < abs_diff) {
            ++index;

            auto size = abs_diff - obtained_below > size_infos[index].size
                ? 0
                : size_infos[index].size - (abs_diff - obtained_below);
            const auto min_size = size_infos[index].min_size;
            const auto max_size = size_infos[index].max_size;

            if (size < min_size)
                size = min_size;
            else if (size > max_size)
                size = max_size;

            obtained_below += size_infos[index].size - size;
            size_infos[index].size = size;

            if (!m_panels[index]->m_hidden)
                m_panels[index]->m_size = size;
        }

        index = panel + 1;
        auto to_allocate = obtained_below;

        while (index > 0 && to_allocate != 0) {
            --index;
            auto size = size_infos[index].size;
            const auto min_size = size_infos[index].min_size;
            const auto max_size = size_infos[index].max_size;

            size += to_allocate;

            if (size < min_size)
                size = min_size;
            else if (size > max_size)
                size = max_size;

            to_allocate -= size - size_infos[index].size;

            size_infos[index].size = size;

            if (!m_panels[index]->m_hidden)
                m_panels[index]->m_size = size;
        }

        return 0 - (abs(delta) - obtained_below);
    }

    return 0;
}

void FlatSplitterPanel::start_autohide_dehide(size_t p_panel, bool b_next_too)
{
    bool b_have_next = b_next_too && is_index_valid(p_panel + 1);
    auto& panel_before = m_panels[p_panel];
    const auto panel_after = b_have_next ? m_panels[p_panel + 1] : Panel::Ptr{};
    if ((panel_before->m_autohide && !panel_before->m_container.m_hook_active)
        || (b_have_next && panel_after->m_autohide && !panel_after->m_container.m_hook_active)) {
        bool a1 = false;
        bool a2 = false;
        if (panel_before->m_autohide && !panel_before->m_container.m_hook_active) {
            panel_before->m_hidden = false;
            a1 = true;
        }
        if (b_have_next && panel_after->m_autohide && !panel_after->m_container.m_hook_active) {
            panel_after->m_hidden = false;
            a2 = true;
        }
        if (a1 || a2) {
            get_host()->on_size_limit_change(get_wnd(), uie::size_limit_all);
            on_size_changed();
            if (a1)
                panel_before->m_container.enter_autohide_hook();
            if (a2)
                panel_after->m_container.enter_autohide_hook();
        }
    }
}

void FlatSplitterPanel::get_supported_panels(
    const pfc::list_base_const_t<window::ptr>& p_windows, bit_array_var& p_mask_unsupported)
{
    const uie::window_host::ptr host
        = m_window_host.is_valid() ? m_window_host : fb2k::service_new<FlatSplitterPanelHost>(this);

    size_t count = p_windows.get_count();
    for (size_t i = 0; i < count; i++)
        p_mask_unsupported.set(i, !p_windows[i]->is_available(host));
}

bool FlatSplitterPanel::is_point_ours(
    HWND wnd_point, const POINT& pt_screen, pfc::list_base_t<window::ptr>& p_hierarchy)
{
    if (wnd_point == get_wnd() || IsChild(get_wnd(), wnd_point)) {
        if (wnd_point == get_wnd()) {
            p_hierarchy.add_item(this);
            return true;
        }
        size_t count = m_panels.size();
        for (size_t i = 0; i < count; i++) {
            uie::splitter_window_v2_ptr sptr;
            if (m_panels[i]->m_child.is_valid()) {
                if (m_panels[i]->m_child->service_query_t(sptr)) {
                    pfc::list_t<window::ptr> temp;
                    temp.add_item(this);
                    if (sptr->is_point_ours(wnd_point, pt_screen, temp)) {
                        p_hierarchy.add_items(temp);
                        return true;
                    }
                } else if (wnd_point == m_panels[i]->m_wnd_child || IsChild(m_panels[i]->m_wnd_child, wnd_point)) {
                    p_hierarchy.add_item(this);
                    p_hierarchy.add_item(m_panels[i]->m_child);
                    return true;
                } else if (wnd_point == m_panels[i]->m_wnd) {
                    p_hierarchy.add_item(this);
                    return true;
                }
            }
        }
    }
    return false;
}

int FlatSplitterPanel::get_panel_divider_size(size_t index) const
{
    if (index + 1 >= m_panels.size())
        return 0;

    // 自編版新增：播放列上下不留分隔線。播放列高度鎖定、不用拖曳，分隔線只會在進度條上方、播放列下方各多一條灰帶
    const auto now_playing_bar = get_now_playing_bar_panel();
    if (now_playing_bar && (m_panels[index] == now_playing_bar || m_panels[index + 1] == now_playing_bar))
        return 0;

    return settings::custom_splitter_divider_width;
}

bool FlatSplitterPanel::set_config_item(
    size_t index, const GUID& p_type, stream_reader* p_source, abort_callback& p_abort)
{
    if (is_index_valid(index)) {
        if (p_type == bool_show_caption) {
            p_source->read_object_t(m_panels[index]->m_show_caption, p_abort);
            if (get_wnd()) {
                get_host()->on_size_limit_change(get_wnd(), uie::size_limit_all);
                on_size_changed();
                m_panels[index]->on_size();
            }
            return true;
        }
        if (p_type == bool_hidden) {
            if (!m_panels[index]->m_autohide)
                p_source->read_object_t(m_panels[index]->m_hidden, p_abort);
            return true;
        }
        if (p_type == bool_autohide) {
            p_source->read_object_t(m_panels[index]->m_autohide, p_abort);
            m_panels[index]->m_hidden = m_panels[index]->m_autohide;
            return true;
        }
        if (p_type == bool_locked) {
            if (get_wnd())
                save_sizes();
            p_source->read_object_t(m_panels[index]->m_locked, p_abort);
            return true;
        }
        if (p_type == uint32_orientation) {
            p_source->read_object_t(m_panels[index]->m_caption_orientation, p_abort);
            return true;
        }
        if (p_type == uint32_size) {
            uint32_t size;
            p_source->read_object_t(size, p_abort);
            m_panels[index]->m_size = size;
            return true;
        }
        if (p_type == size_and_dpi) {
            uie::size_and_dpi sad;
            p_source->read_object_t(sad.size, p_abort);
            p_source->read_object_t(sad.dpi, p_abort);
            m_panels[index]->m_size.value = sad.size;
            m_panels[index]->m_size.dpi = sad.dpi;
            return true;
        }
        if (p_type == bool_show_toggle_area && get_orientation() == horizontal) {
            p_source->read_object_t(m_panels[index]->m_show_toggle_area, p_abort);
            return true;
        }
        if (p_type == bool_use_custom_title) {
            p_source->read_object_t(m_panels[index]->m_use_custom_title, p_abort);
            return true;
        }
        if (p_type == string_custom_title) {
            p_source->read_string(m_panels[index]->m_custom_title, p_abort);
            return true;
        }
        return false;
    }
    return false;
}

bool FlatSplitterPanel::get_config_item(
    size_t index, const GUID& p_type, stream_writer* p_out, abort_callback& p_abort) const
{
    if (is_index_valid(index)) {
        if (p_type == bool_show_caption) {
            p_out->write_object_t(m_panels[index]->m_show_caption, p_abort);
            return true;
        }
        if (p_type == bool_hidden) {
            p_out->write_object_t(m_panels[index]->m_hidden, p_abort);
            return true;
        }
        if (p_type == bool_autohide) {
            p_out->write_object_t(m_panels[index]->m_autohide, p_abort);
            return true;
        }
        if (p_type == bool_locked) {
            p_out->write_object_t(m_panels[index]->m_locked, p_abort);
            return true;
        }
        if (p_type == uint32_orientation) {
            p_out->write_object_t(m_panels[index]->m_caption_orientation, p_abort);
            return true;
        }
        if (p_type == uint32_size) {
            p_out->write_object_t(m_panels[index]->m_size.get_scaled_value(), p_abort);
            return true;
        }
        if (p_type == size_and_dpi) {
            p_out->write_object_t(m_panels[index]->m_size.value, p_abort);
            p_out->write_object_t(m_panels[index]->m_size.dpi, p_abort);
            return true;
        }
        if (p_type == bool_show_toggle_area && get_orientation() == horizontal) {
            p_out->write_object_t(m_panels[index]->m_show_toggle_area, p_abort);
            return true;
        }
        if (p_type == bool_use_custom_title) {
            p_out->write_object_t(m_panels[index]->m_use_custom_title, p_abort);
            return true;
        }
        if (p_type == string_custom_title) {
            p_out->write_string(m_panels[index]->m_custom_title, p_abort);
            return true;
        }
        return false;
    }
    return false;
}

bool FlatSplitterPanel::get_config_item_supported(size_t index, const GUID& p_type) const
{
    if (is_index_valid(index)) {
        if (p_type == bool_show_caption || p_type == bool_locked || p_type == bool_hidden
            || p_type == uint32_orientation || p_type == bool_autohide
            || (p_type == bool_show_toggle_area && get_orientation() == horizontal) || p_type == uint32_size
            || p_type == size_and_dpi || p_type == bool_use_custom_title || p_type == string_custom_title)
            return true;
    }
    return false;
}

bool FlatSplitterPanel::is_index_valid(size_t index) const
{
    return index < m_panels.size();
}

void FlatSplitterPanel::get_config(stream_writer* out, abort_callback& p_abort) const
{
    write_config(out, false, p_abort);
}

void FlatSplitterPanel::export_config(stream_writer* p_writer, abort_callback& p_abort) const
{
    write_config(p_writer, true, p_abort);
}

void FlatSplitterPanel::write_config(stream_writer* p_writer, bool is_export, abort_callback& p_abort) const
{
    p_writer->write_lendian_t(static_cast<uint32_t>(stream_version_current), p_abort);
    const auto count = m_panels.size();
    p_writer->write_lendian_t(gsl::narrow<uint32_t>(count), p_abort);
    for (size_t i = 0; i < count; i++) {
        if (is_export)
            m_panels[i]->_export(p_writer, p_abort);
        else
            m_panels[i]->write(p_writer, p_abort);
    }
    // Extra data added in version 0.5.0
    for (size_t i = 0; i < count; i++) {
        stream_writer_memblock extraData;
        m_panels[i]->write_extra(&extraData, p_abort);
        p_writer->write_lendian_t(gsl::narrow<uint32_t>(extraData.m_data.get_size()), p_abort);
        p_writer->write(extraData.m_data.get_ptr(), extraData.m_data.get_size(), p_abort);
    }
}

void FlatSplitterPanel::read_config(stream_reader* config, size_t p_size, bool is_import, abort_callback& p_abort)
{
    if (p_size) {
        uint32_t version;
        config->read_lendian_t(version, p_abort);
        if (version <= stream_version_current) {
            std::vector<Panel::Ptr> panels;

            unsigned count;
            config->read_lendian_t(count, p_abort);

            unsigned i;
            for (i = 0; i < count; i++) {
                auto temp = std::make_shared<Panel>();
                if (is_import)
                    temp->import(config, p_abort);
                else
                    temp->read(config, p_abort);
                panels.emplace_back(std::move(temp));
            }

            // Extra data added in version 0.5.0
            for (i = 0; i < count; i++) {
                uint32_t extraDataSize;
                try {
                    config->read_lendian_t(extraDataSize, p_abort);
                } catch (const exception_io_data_truncation&) {
                    if (i == 0)
                        break;
                    throw;
                }
                pfc::array_staticsize_t<uint8_t> columnExtraData(extraDataSize);
                config->read(columnExtraData.get_ptr(), columnExtraData.get_size(), p_abort);
                stream_reader_memblock_ref columnReader(columnExtraData);
                panels[i]->read_extra(&columnReader, p_abort);
            }

            m_panels = panels;
        }
    }
}

void FlatSplitterPanel::import_config(stream_reader* p_reader, size_t p_size, abort_callback& p_abort)
{
    read_config(p_reader, p_size, true, p_abort);
}

void FlatSplitterPanel::set_config(stream_reader* config, size_t p_size, abort_callback& p_abort)
{
    read_config(config, p_size, false, p_abort);
}

uie::splitter_item_t* FlatSplitterPanel::get_panel(size_t index) const
{
    if (index < m_panels.size()) {
        return m_panels[index]->create_splitter_item();
    }
    return nullptr;
}

size_t FlatSplitterPanel::get_panel_count() const
{
    return m_panels.size();
}

void FlatSplitterPanel::remove_panel(size_t index)
{
    if (index >= m_panels.size())
        return;

    m_panels[index]->destroy();
    m_panels.erase(m_panels.begin() + index);

    if (get_wnd())
        refresh_children();
}

unsigned FlatSplitterPanel::get_type() const
{
    return ui_extension::type_layout | uie::type_splitter;
}

void FlatSplitterPanel::get_category(pfc::string_base& p_out) const
{
    p_out = "Splitters";
}

int FlatSplitterPanel::g_get_caption_size()
{
    return uih::get_font_height(g_font_menu_horizontal.get()) + 8_spx;
}

void FlatSplitterPanel::FlatSplitterPanelHost::relinquish_ownership(HWND wnd)
{
    const auto iter = m_this->find_panel_by_panel_wnd(wnd);

    if (iter == m_this->m_panels.end())
        return;

    const auto panel = *iter;

    if (GetAncestor(wnd, GA_PARENT) == panel->m_wnd)
        console::warning("window left by ui extension");

    // 自編版新增：視窗移到別的地方就不再是這裡的播放列（沒掛過時什麼都不做）
    RemoveWindowSubclass(wnd, s_now_playing_bar_subclass_proc, now_playing_bar_subclass_id);

    panel->m_wnd_child = nullptr;
    panel->m_wnd = nullptr;
    panel->m_child.release();
    panel->destroy();
    m_this->m_panels.erase(iter);

    if (!m_this->m_refresh_children_in_progress) {
        m_this->get_host()->on_size_limit_change(m_this->get_wnd(), uie::size_limit_all);
        m_this->on_size_changed();
    }
}

bool FlatSplitterPanel::FlatSplitterPanelHost::set_window_visibility(HWND wnd, bool visibility)
{
    if (!m_this->get_host()->is_visible(m_this->get_wnd()))
        return m_this->get_host()->set_window_visibility(m_this->get_wnd(), visibility);

    const auto iter = m_this->find_panel_by_panel_wnd(wnd);
    if (iter == m_this->m_panels.end())
        return false;

    const auto panel = *iter;

    if (panel->m_autohide)
        return false;

    panel->m_hidden = !visibility;
    m_this->get_host()->on_size_limit_change(m_this->get_wnd(), uie::size_limit_all);
    m_this->on_size_changed();

    return true;
}

bool FlatSplitterPanel::FlatSplitterPanelHost::is_visibility_modifiable(HWND wnd, bool desired_visibility) const
{
    if (!m_this->get_host()->is_visible(m_this->get_wnd()))
        return m_this->get_host()->is_visibility_modifiable(m_this->get_wnd(), desired_visibility);

    const auto iter = m_this->find_panel_by_panel_wnd(wnd);
    if (iter == m_this->m_panels.end())
        return false;

    const auto panel = *iter;
    return !panel->m_autohide;
}

bool FlatSplitterPanel::FlatSplitterPanelHost::is_visible(HWND wnd) const
{
    if (!m_this->get_host()->is_visible(m_this->get_wnd()))
        return false;

    const auto iter = m_this->find_panel_by_panel_wnd(wnd);
    if (iter == m_this->m_panels.end())
        return false;

    const auto panel = *iter;
    return !panel->m_hidden;
}

bool FlatSplitterPanel::FlatSplitterPanelHost::override_status_text_create(
    service_ptr_t<ui_status_text_override>& p_out)
{
    return m_this->get_host()->override_status_text_create(p_out);
}

bool FlatSplitterPanel::FlatSplitterPanelHost::request_resize(HWND wnd, unsigned flags, unsigned width, unsigned height)
{
    if (!flags)
        return true;

    if (flags & (get_orientation() == horizontal ? ui_extension::size_height : uie::size_width))
        return false;

    if (!(flags & (get_orientation() == vertical ? ui_extension::size_height : uie::size_width)))
        return false;

    const auto iter = m_this->find_panel_by_panel_wnd(wnd);

    if (iter == m_this->m_panels.end())
        return false;

    const int delta = (get_orientation() == horizontal ? width : height) - (*iter)->m_size;

    const size_t index = std::distance(m_this->m_panels.begin(), iter);
    m_this->override_size(index, delta);
    return true;
}

unsigned FlatSplitterPanel::FlatSplitterPanelHost::is_resize_supported(HWND wnd) const
{
    return get_orientation() == vertical ? ui_extension::size_height : uie::size_width;
}

Orientation FlatSplitterPanel::FlatSplitterPanelHost::get_orientation() const
{
    return m_this.is_valid() ? m_this->get_orientation() : vertical;
}

void FlatSplitterPanel::FlatSplitterPanelHost::on_size_limit_change(HWND wnd, unsigned flags)
{
    const auto iter = m_this->find_panel_by_panel_wnd(wnd);

    if (iter == m_this->m_panels.end())
        return;

    const auto p_ext = *iter;
    MINMAXINFO mmi{};
    mmi.ptMaxTrackSize.x = MAXLONG;
    mmi.ptMaxTrackSize.y = MAXLONG;
    SendMessage(wnd, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&mmi));
    helpers::clip_minmaxinfo(mmi);

    p_ext->m_size_limits.min_width = mmi.ptMinTrackSize.x;
    p_ext->m_size_limits.min_height = mmi.ptMinTrackSize.y;
    p_ext->m_size_limits.max_height = mmi.ptMaxTrackSize.y;
    p_ext->m_size_limits.max_width = mmi.ptMaxTrackSize.x;
    pfc::string8 name;
    p_ext->m_child->get_name(name);

    m_this->on_size_changed();

    m_this->get_host()->on_size_limit_change(m_this->get_wnd(), flags);
}

void FlatSplitterPanel::FlatSplitterPanelHost::get_children(pfc::list_base_t<window::ptr>& p_out)
{
    if (m_this.is_valid()) {
        size_t count = m_this->m_panels.size();
        for (size_t i = 0; i < count; i++) {
            if (m_this->m_panels[i]->m_child.is_valid())
                p_out.add_item(m_this->m_panels[i]->m_child);
        }
    }
}

bool FlatSplitterPanel::FlatSplitterPanelHost::get_keyboard_shortcuts_enabled() const
{
    return m_this->get_host()->get_keyboard_shortcuts_enabled();
}

const GUID& FlatSplitterPanel::FlatSplitterPanelHost::get_host_guid() const
{
    // {FC0ED6EF-DCA2-4679-B7FE-48162DE321FC}
    static const GUID rv = {0xfc0ed6ef, 0xdca2, 0x4679, {0xb7, 0xfe, 0x48, 0x16, 0x2d, 0xe3, 0x21, 0xfc}};
    return rv;
}

void FlatSplitterPanel::s_on_size_change()
{
    for (size_t index = 0; index < g_instances.get_count(); index++) {
        g_instances[index]->on_size_changed();
    }
}

FlatSplitterPanel::Panel::Ptr FlatSplitterPanel::find_panel_by_custom_title(std::string_view title) const
{
    const auto iter
        = std::ranges::find_if(m_panels, [title](const Panel::Ptr& panel) { return panel->has_custom_title(title); });

    return iter != m_panels.end() ? *iter : nullptr;
}

FlatSplitterPanel::Panel::Ptr FlatSplitterPanel::get_now_playing_bar_panel() const
{
    if (get_orientation() != vertical || !find_panel_by_custom_title(now_playing_main_title))
        return nullptr;

    return find_panel_by_custom_title(now_playing_bar_title);
}

FlatSplitterPanel::Panel::Ptr FlatSplitterPanel::get_now_playing_side_panel() const
{
    if (!get_now_playing_bar_panel())
        return nullptr;

    return find_panel_by_custom_title(now_playing_side_title);
}

void FlatSplitterPanel::s_refresh_now_playing()
{
    for (size_t index = 0; index < g_instances.get_count(); index++) {
        const auto splitter = g_instances[index];

        if (splitter->get_now_playing_side_panel())
            splitter->on_size_changed();
    }
}

bool FlatSplitterPanel::s_toggle_now_playing()
{
    for (size_t index = 0; index < g_instances.get_count(); index++) {
        const auto splitter = g_instances[index];

        if (!splitter->get_now_playing_bar_panel())
            continue;

        s_last_now_playing_toggle_tick = GetTickCount64();
        splitter->m_now_playing_expanded = !splitter->m_now_playing_expanded;
        // 自編版（展開頁動畫）：不再一次切換區域，改成由下往上揭開／由上往下蓋回
        splitter->start_now_playing_animation();
        return true;
    }

    return false;
}

void FlatSplitterPanel::start_now_playing_animation()
{
    const auto now_playing_bar = get_now_playing_bar_panel();

    if (!now_playing_bar || !now_playing_bar->m_wnd) {
        on_size_changed();
        return;
    }

    // 從目前看得到的區域開始；動畫進行中再切換時，就從當下的位置倒回去
    // 沒設區域（展開中）時 GetWindowRgnBox 傳回 ERROR，起點是整個視窗
    RECT from{};
    if (GetWindowRgnBox(now_playing_bar->m_wnd, &from) == ERROR)
        GetClientRect(now_playing_bar->m_wnd, &from);

    m_now_playing_animation_from = from;
    m_now_playing_animation_start = std::chrono::steady_clock::now();
    m_now_playing_animating = true;

    on_size_changed();
    SetTimer(m_wnd, now_playing_animation_timer_id, USER_TIMER_MINIMUM, nullptr);
}

void FlatSplitterPanel::on_now_playing_animation_timer()
{
    const auto now_playing_bar = get_now_playing_bar_panel();
    const auto elapsed = std::chrono::steady_clock::now() - m_now_playing_animation_start;

    if (!now_playing_bar || !now_playing_bar->m_wnd
        || elapsed >= std::chrono::milliseconds(now_playing_animation_duration_ms)) {
        KillTimer(m_wnd, now_playing_animation_timer_id);
        m_now_playing_animating = false;
        // 最終區域、展開頁右側面板
        on_size_changed();
    } else {
        RECT client_rect{};
        GetClientRect(m_wnd, &client_rect);
        const auto animation_rect = get_now_playing_animation_rect(client_rect.right, client_rect.bottom);
        SetWindowRgn(now_playing_bar->m_wnd, CreateRectRgnIndirect(&animation_rect), TRUE);
    }

    // 新露出來的部分（展開時是展開頁，收合時是主內容）馬上畫，不等訊息迴圈
    RedrawWindow(m_wnd, nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN);

    // 等這一格真的顯示到螢幕上再處理下一格，每格間隔對齊螢幕更新，不會忽快忽慢
    // （計時器本身約 15.6ms 才跳一次，跟螢幕更新不同步）
    if (m_now_playing_animating) {
        [[maybe_unused]] const auto hr = DwmFlush();
    }
}

RECT FlatSplitterPanel::get_now_playing_animation_rect(unsigned width, unsigned height) const
{
    const auto elapsed_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - m_now_playing_animation_start)
                                .count();
    const auto linear_progress = std::clamp(elapsed_ms / now_playing_animation_duration_ms, 0.0, 1.0);

    // 展開 ease-out sine（一開始就動、慢慢停下），收合 ease-in-out sine（兩頭都慢）
    // 原本的 (0.2, 0, 0, 1)／(0.3, 0, 0.8, 0.15) 中段太陡，展開區約 1046px 時最快一格移動 250px 以上，看起來跳；
    // 換成這兩條、400ms 後最快一格約 70px（60Hz）
    const auto progress = m_now_playing_expanded ? cubic_bezier(0.61, 1.0, 0.88, 1.0, linear_progress)
                                                 : cubic_bezier(0.37, 0.0, 0.63, 1.0, linear_progress);

    const RECT target = m_now_playing_expanded
        ? RECT{0, 0, gsl::narrow<LONG>(width), gsl::narrow<LONG>(height)}
        : m_now_playing_bar_rect;
    const auto& from = m_now_playing_animation_from;
    const auto interpolate
        = [progress](LONG from_value, LONG to_value) { return std::lround(from_value + (to_value - from_value) * progress); };

    return {interpolate(from.left, target.left), interpolate(from.top, target.top),
        interpolate(from.right, target.right), interpolate(from.bottom, target.bottom)};
}

bool FlatSplitterPanel::s_is_now_playing_expanded()
{
    for (size_t index = 0; index < g_instances.get_count(); index++) {
        const auto splitter = g_instances[index];

        if (splitter->get_now_playing_bar_panel())
            return splitter->m_now_playing_expanded;
    }

    return false;
}

LRESULT CALLBACK FlatSplitterPanel::s_now_playing_bar_subclass_proc(
    HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref_data)
{
    // ref_data 為 1：剛吞掉一個雙擊，接著那個放開也要吞掉
    switch (msg) {
    case WM_LBUTTONDBLCLK:
        // ESLyric 雙擊固定會進內建全螢幕（沒有設定可關），所以雙擊一律不交給它。
        // 第一下剛觸發切換播放頁：第二下整個吞掉，不然會再切一次；
        // 否則當成一般的按下，連點下一首這類按鈕照常作用
        if (GetTickCount64() - s_last_now_playing_toggle_tick <= GetDoubleClickTime()) {
            SetWindowSubclass(wnd, s_now_playing_bar_subclass_proc, id, 1);
            return 0;
        }
        return DefSubclassProc(wnd, WM_LBUTTONDOWN, wp, lp);
    case WM_LBUTTONDOWN:
        // 上一個放開沒送到這裡（例如按著拖出去）
        if (ref_data)
            SetWindowSubclass(wnd, s_now_playing_bar_subclass_proc, id, 0);
        break;
    case WM_LBUTTONUP:
        if (ref_data) {
            SetWindowSubclass(wnd, s_now_playing_bar_subclass_proc, id, 0);
            return 0;
        }
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(wnd, s_now_playing_bar_subclass_proc, id);
        break;
    }
    return DefSubclassProc(wnd, msg, wp, lp);
}

} // namespace cui::panels::splitter
