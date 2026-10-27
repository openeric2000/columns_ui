#include "pch.h"
#include "menu_items.h"
#include "layout.h"
#include "ng_playlist/ng_playlist.h"
#include "main_window.h"
#include "button_items.h"
#include "config_appearance.h"
#include "filter_search_bar.h"
#include "system_appearance_manager.h"
#include "tab_main_window.h"
#include "splitter.h"

namespace cui::main_menu {

namespace groups {

constexpr GUID view_columns_part{0xae078354, 0x60ea, 0x48a9, {0x94, 0x5, 0xda, 0xa, 0xef, 0x0, 0xc5, 0x8d}};
constexpr GUID view_playlist_popup{0x16610091, 0x8bd9, 0x4e38, {0x9d, 0xe5, 0x43, 0x29, 0xbe, 0x14, 0x12, 0x1b}};
constexpr GUID playlist_font_part{0xcef3f5a, 0x2c3, 0x4865, {0x8f, 0x62, 0x37, 0x92, 0x80, 0xe4, 0xec, 0x3a}};
constexpr GUID playlist_settings_part{0xfee5a195, 0xd907, 0x4475, {0x8c, 0xdc, 0xdc, 0x17, 0x61, 0x4, 0xb8, 0xa8}};
constexpr GUID playlist_misc_part{0xf74d3c19, 0x91cc, 0x4179, {0x95, 0x4a, 0x7b, 0xe5, 0x4e, 0x9b, 0xbc, 0x80}};
constexpr GUID view_layout_popup{0x533fdc34, 0xee1b, 0x4317, {0x9d, 0xc, 0x48, 0x9e, 0xe1, 0x21, 0x5d, 0x9f}};
constexpr GUID view_layout_commands{0x964c3b5a, 0x1908, 0x4407, {0x9b, 0x8e, 0xfc, 0x73, 0x31, 0xa9, 0x9e, 0xd1}};
constexpr GUID view_layout_presets{0xc36ef588, 0xabdf, 0x495e, {0xa7, 0xd4, 0xd0, 0x21, 0x84, 0x6f, 0x50, 0xee}};
constexpr GUID view_mode_popup{0x7859ac67, 0xf8b4, 0x461c, {0x9b, 0xc3, 0xd1, 0x33, 0x62, 0xa1, 0x60, 0x7e}};
constexpr GUID view_mode_toggle_part{0xe3924405, 0x484d, 0x468c, {0x87, 0xdf, 0xc9, 0x46, 0xd1, 0x42, 0xe1, 0xab}};
constexpr GUID view_mode_modes_part{0xdb6a0be7, 0x45b3, 0x47b7, {0xb0, 0x26, 0xf7, 0x6b, 0x9c, 0xf4, 0x36, 0x7e}};

namespace {

mainmenu_group_factory _mainmenu_group_view_columns_part(
    view_columns_part, mainmenu_groups::view, mainmenu_commands::sort_priority_base + 1);

mainmenu_group_popup_factory _mainmenu_group_view_mode_popup(
    view_mode_popup, view_columns_part, mainmenu_commands::sort_priority_base, "Mode");

mainmenu_group_popup_factory _mainmenu_group_view_layout_popup(
    view_layout_popup, view_columns_part, mainmenu_commands::sort_priority_base + 1, "Layout");

mainmenu_group_popup_factory _mainmenu_group_view_playlist_popup(
    view_playlist_popup, view_columns_part, mainmenu_commands::sort_priority_base + 2, "Playlist view");

mainmenu_group_factory _mainmenu_group_playlist_misc_part(
    playlist_misc_part, view_playlist_popup, mainmenu_commands::sort_priority_base);

mainmenu_group_factory _mainmenu_group_playlist_settings_part(
    playlist_settings_part, view_playlist_popup, mainmenu_commands::sort_priority_base + 1);

mainmenu_group_factory _mainmenu_group_playlist_font_part(
    playlist_font_part, view_playlist_popup, mainmenu_commands::sort_priority_base + 2);

mainmenu_group_factory _mainmenu_group_view_layout_commands_part(
    view_layout_commands, view_layout_popup, mainmenu_commands::sort_priority_base);

mainmenu_group_factory _mainmenu_group_view_layout_presets_part(
    view_layout_presets, view_layout_popup, mainmenu_commands::sort_priority_base + 1);

mainmenu_group_factory _mainmenu_group_view_mode_toggle_part(
    view_mode_toggle_part, view_mode_popup, mainmenu_commands::sort_priority_base);

mainmenu_group_factory _mainmenu_group_view_mode_modes_part(
    view_mode_modes_part, view_mode_popup, mainmenu_commands::sort_priority_base + 1);

} // namespace

} // namespace groups

namespace commands {

namespace {

constexpr GUID playlist_view_search_id{0xb284f957, 0x32cb, 0x4ab1, {0xb2, 0xb5, 0x4c, 0x07, 0x1f, 0x7f, 0x9d, 0x5e}};

const MainMenuCommand playlist_view_search{.guid = playlist_view_search_id,
    .name = "Search playlist",
    .description = "Opens and activates the playlist view search bar.",
    .execute_callback = [] { panels::playlist_view::PlaylistView::s_show_search_bar(); },
    .is_available_callback = [] { return panels::playlist_view::PlaylistView::s_has_active_instance(); }};

const MainMenuCommand activate_now_playing{activate_now_playing_id, "Activate now playing",
    "Activates the currently playing item.", [] { playlist_manager::get()->highlight_playing_item(); }};

const MainMenuCommand show_groups{show_groups_id, "Show groups", "Shows or hides playlist view groups.",
    [] {
        panels::playlist_view::cfg_grouping = !panels::playlist_view::cfg_grouping;
        panels::playlist_view::PlaylistView::g_on_groups_change();
    },
    [] { return static_cast<bool>(panels::playlist_view::cfg_grouping); }};

const MainMenuCommand show_artwork{show_artwork_id, "Show artwork", "Shows or hides playlist view artwork in groups.",
    [] {
        panels::playlist_view::cfg_show_artwork = !panels::playlist_view::cfg_show_artwork;
        panels::playlist_view::PlaylistView::g_on_show_artwork_change();
    },
    [] { return panels::playlist_view::cfg_show_artwork.get(); }};

const MainMenuCommand decrease_font{{0xf2bc9f43, 0xf709, 0x4f6f, {0x9c, 0x65, 0x78, 0x73, 0x3b, 0x8, 0xc7, 0x77}},
    "Decrease font size", "Decreases the playlist view font size.",
    [] { panels::playlist_view::set_font_size(-1.0f); }};

const MainMenuCommand increase_font{{0x8553d7fd, 0xebc5, 0x4ae7, {0xaa, 0x28, 0xb2, 0x6, 0xfe, 0x94, 0xa0, 0xb4}},
    "Increase font size", "Increases the playlist font size.", [] { panels::playlist_view::set_font_size(1.0f); }};

const MainMenuCommand toggle_lock_main_window_size{
    .guid{0xb6fc1a63, 0xf588, 0x4f50, {0x85, 0x82, 0x08, 0x44, 0x0a, 0x77, 0x29, 0x8f}},
    .name = "Lock window size",
    .description = "Toggles whether resizing the main window using its border is allowed.",
    .execute_callback = [] { lock_main_window_size = !lock_main_window_size; },
    .is_ticked_callback = [] { return lock_main_window_size.get(); }};

const MainMenuCommand toggle_transparency{
    .guid{0x0d0dbf0a, 0x7c93, 0x414a, {0x99, 0xef, 0xcc, 0x25, 0x90, 0x19, 0x6a, 0x62}},
    .name = "Transparent window",
    .description = "Enables or disables main window transparency.",
    .execute_callback =
        [] {
            main_window::config_set_transparency_enabled(!main_window::config_get_transparency_enabled());
            prefs::update_main_window_tab_transparency_enabled();
        },
    .is_ticked_callback = [] { return main_window::config_get_transparency_enabled(); },
    .hide_without_shift_key = true};

const MainMenuCommand show_status_bar{{0x5f944522, 0x843b, 0x43d2, {0x87, 0x14, 0xe3, 0xca, 0x1b, 0x78, 0x2b, 0x1f}},
    "Show status bar", "Shows or hides the Columns UI status bar.",
    [] {
        cfg_status = !cfg_status;
        on_show_status_change();
    },
    [] { return cfg_status != 0; }};

const MainMenuCommand show_status_pane{{0x6ffa8da6, 0x5cd, 0x403e, {0xa4, 0x87, 0x7e, 0x57, 0x2b, 0xb0, 0xc3, 0x20}},
    "Show status pane", "Shows or hides the Columns UI status pane.",
    [] {
        settings::show_status_pane = !settings::show_status_pane;
        on_show_status_pane_change();
    },
    [] { return settings::show_status_pane != 0; }};

const MainMenuCommand show_toolbars{{0x2dbb8dd3, 0x2d94, 0x4fd8, {0xa5, 0x7d, 0xbe, 0xb6, 0x42, 0xf5, 0xbf, 0x3b}},
    "Show toolbars", "Shows or hides the Columns UI toolbars.",
    [] {
        cfg_toolbars = !cfg_toolbars;
        on_show_toolbars_change();
    },
    [] { return cfg_toolbars != 0; }, {}, true};

const MainMenuCommand live_editing{toggle_live_editing_id, "Live editing",
    "Enables or disables live editing of the Columns UI layout.",
    [] {
        bool val = !g_layout_window.get_layout_editing_active();
        g_layout_window.set_layout_editing_active(val);
        button_items::LiveLayoutEditingButton::s_on_change();
    },
    [] { return g_layout_window.get_layout_editing_active(); }};

// 自編版新增：ESLyric 播放列用 mm://View/Layout/切換播放頁 展開／收合播放頁
// 版面裡要有同一層、自訂標題分別為「主內容」「播放列」的兩個面板（標題常數在 splitter.h）；找不到時什麼都不做
using panels::splitter::FlatSplitterPanel;

const MainMenuCommand toggle_now_playing_view{toggle_now_playing_view_id, "切換播放頁",
    "讓自訂標題為「播放列」的面板蓋住同一層自訂標題為「主內容」的面板，或縮回原位。停止播放時不展開。",
    [] {
        // 沒在播放（停止）時不展開；暫停算播放中。收合一律可以
        if (!FlatSplitterPanel::s_is_now_playing_expanded() && !playback_control::get()->is_playing())
            return;

        FlatSplitterPanel::s_toggle_now_playing();
    },
    [] { return FlatSplitterPanel::s_is_now_playing_expanded(); }};

// 自編版新增：播放停止（按停止、清單播完）時，展開中的播放頁自動收回；換曲與結束 fb2k 不處理
class NowPlayingViewPlayCallback : public play_callback_static {
public:
    unsigned get_flags() override { return flag_on_playback_stop; }

    void on_playback_stop(play_control::t_stop_reason reason) override
    {
        if (reason == play_control::stop_reason_starting_another || reason == play_control::stop_reason_shutting_down)
            return;

        if (FlatSplitterPanel::s_is_now_playing_expanded())
            FlatSplitterPanel::s_toggle_now_playing();
    }

    void on_playback_starting(play_control::t_track_command command, bool paused) override {}
    void on_playback_new_track(metadb_handle_ptr track) override {}
    void on_playback_seek(double time) override {}
    void on_playback_pause(bool state) override {}
    void on_playback_edited(metadb_handle_ptr track) override {}
    void on_playback_dynamic_info(const file_info& info) override {}
    void on_playback_dynamic_info_track(const file_info& info) override {}
    void on_playback_time(double time) override {}
    void on_volume_change(float new_value) override {}
};

play_callback_static_factory_t<NowPlayingViewPlayCallback> now_playing_view_play_callback;

// 自編版新增：展開頁樣式，1 置中（預設）、2 靠左＋VU 表、3 靠左＋資訊卡，ESLyric 佈局用 %cui_now_playing_style% 切換
// ESLyric 的 visibility 不會隨時重算，所以切換後對正在播放的曲目送一次 dispatch_refresh，讓佈局重新套用
// 樣式 2 的 VU 表是另一個面板，由分割面板排版時疊上去，所以切換後也要重新排版
constexpr uint32_t now_playing_style_count = 3;
fbh::ConfigUint32 now_playing_style(
    GUID{0xdd906dca, 0x9b49, 0x492c, {0x8b, 0xb2, 0x43, 0xb9, 0xef, 0xe4, 0xf1, 0x8f}}, 1);

void set_now_playing_style(uint32_t style)
{
    now_playing_style = style;
    FlatSplitterPanel::s_refresh_now_playing();

    metadb_handle_ptr track;
    if (playback_control::get()->get_now_playing(track))
        metadb_io::get()->dispatch_refresh(track);
}

// 自編版新增：標題格式欄位 %cui_now_playing_expanded%，播放頁展開時為 1、收合時不存在，給 ESLyric 佈局切換 ⌃／⌄
// 以及 %cui_now_playing_style%（展開頁樣式 1～3）
// 標題格式可能在別的執行緒算，所以展開狀態讀 atomic，不碰分割面板本身；樣式只是單一 32 位元整數設定值，直接讀
class NowPlayingViewFieldProvider : public metadb_display_field_provider {
public:
    t_uint32 get_field_count() override { return 2; }
    void get_field_name(t_uint32 index, pfc::string_base& out) override
    {
        out = index == 0 ? "cui_now_playing_expanded" : "cui_now_playing_style";
    }

    bool process_field(t_uint32 index, metadb_handle* handle, titleformat_text_out* out) override
    {
        if (index == 1) {
            out->write(titleformat_inputtypes::meta, pfc::format_uint(now_playing_style.get()));
            return true;
        }

        if (!FlatSplitterPanel::s_now_playing_expanded_for_fields)
            return false;

        out->write(titleformat_inputtypes::meta, "1");
        return true;
    }
};

service_factory_single_t<NowPlayingViewFieldProvider> now_playing_view_field_provider;

// 自編版新增：ESLyric 展開頁的大封面用 mm://View/Layout/顯示封面大圖 開 foobar2000 picture viewer
// 雙擊播放列會被轉成第二次按下（見 splitter_window.cpp 的 s_now_playing_bar_subclass_proc），
// 所以雙擊間隔內的第二次呼叫直接略過，不會開出兩個視窗
const MainMenuCommand show_now_playing_cover{show_now_playing_cover_id, "顯示封面大圖",
    "在 foobar2000 picture viewer 開啟正在播放曲目的封面。",
    [] {
        static ULONGLONG last_tick{};
        const auto now = GetTickCount64();
        if (now - last_tick <= GetDoubleClickTime())
            return;
        last_tick = now;

        metadb_handle_ptr track;
        if (!playback_control::get()->get_now_playing(track))
            return;

        if (fb2k::imageViewer::ptr api; fb2k::imageViewer::tryGet(api))
            api->load_and_show(core_api::get_main_window(), pfc::list_single_ref_t<metadb_handle_ptr>(track),
                album_art_ids::cover_front);
    }};

const MainMenuCommand now_playing_style_centered{now_playing_style_centered_id, "展開頁：置中",
    "展開頁的封面與歌詞整組置中。", [] { set_now_playing_style(1); }, {},
    [] { return now_playing_style.get() == 1; }};

const MainMenuCommand now_playing_style_spectrum{now_playing_style_spectrum_id, "展開頁：靠左＋VU 表",
    "展開頁的封面靠左，右邊顯示 VU 表（版面裡自訂標題「展開頁右側」的面板）。", [] { set_now_playing_style(2); }, {},
    [] { return now_playing_style.get() == 2; }};

const MainMenuCommand now_playing_style_info{now_playing_style_info_id, "展開頁：靠左＋資訊卡",
    "展開頁的封面靠左，右邊顯示曲目資訊。", [] { set_now_playing_style(3); }, {},
    [] { return now_playing_style.get() == 3; }};

// ESLyric 展開頁的樣式按鈕用 mm://View/Layout/切換展開頁樣式，依序換到下一種
const MainMenuCommand cycle_now_playing_style{cycle_now_playing_style_id, "切換展開頁樣式",
    "依序切換展開頁樣式：置中、靠左＋VU 表、靠左＋資訊卡。",
    [] { set_now_playing_style(now_playing_style.get() % now_playing_style_count + 1); }};

const MainMenuCommand focus_filter_search{
    .guid{0x9ff94c30, 0x7b34, 0x4b5f, {0x99, 0x52, 0xed, 0x14, 0x8a, 0x9a, 0x3f, 0x5c}},
    .name = "Focus Filter search",
    .description = "Focuses the Filter search toolbar.",
    .execute_callback = [] { panels::filter::FilterSearchToolbar::s_activate(true); },
    .hide_without_shift_key = true};

const MainMenuCommand toggle_mode{.guid{0x63798f1c, 0x24f0, 0x41b7, {0x84, 0xc2, 0xa8, 0x82, 0x54, 0x11, 0x10, 0x8e}},
    .name = "Switch to other mode",
    .description = "Switches between dark and light mode.",
    .execute_callback =
        [] {
            if (!system_appearance_manager::is_dark_mode_available())
                return;

            if (colours::is_dark_mode_active())
                colours::dark_mode_status.set(WI_EnumValue(colours::DarkModeStatus::Disabled));
            else
                colours::dark_mode_status.set(WI_EnumValue(colours::DarkModeStatus::Enabled));
        },
    .hide_without_shift_key = true,
    .is_available_callback = [] { return system_appearance_manager::is_dark_mode_available(); }};

const MainMenuCommand use_light_mode{
    .guid{0x18f3f46b, 0xb2ad, 0x49b0, {0xa9, 0x78, 0x62, 0x07, 0xc0, 0xd1, 0x35, 0x5b}},
    .name = "Light",
    .description = "Use light mode.",
    .execute_callback = [] { colours::dark_mode_status.set(WI_EnumValue(colours::DarkModeStatus::Disabled)); },
    .is_radio_selected_callback
    = [] { return colours::dark_mode_status == WI_EnumValue(colours::DarkModeStatus::Disabled); },
    .is_available_callback = [] { return system_appearance_manager::is_dark_mode_available(); }};

const MainMenuCommand use_dark_mode{.guid{0x39913e3f, 0xab8e, 0x4678, {0x86, 0x16, 0x59, 0x99, 0x73, 0x6f, 0xe9, 0xe0}},
    .name = "Dark",
    .description = "Use dark mode.",
    .execute_callback = [] { colours::dark_mode_status.set(WI_EnumValue(colours::DarkModeStatus::Enabled)); },
    .is_radio_selected_callback
    = [] { return colours::dark_mode_status == WI_EnumValue(colours::DarkModeStatus::Enabled); },
    .is_available_callback = [] { return system_appearance_manager::is_dark_mode_available(); }};

const MainMenuCommand use_system_mode{
    .guid{0x4da59c73, 0x185b, 0x4553, {0x86, 0xa1, 0x1c, 0xa6, 0xba, 0xa9, 0x7c, 0x17}},
    .name = "Use system setting",
    .description = "Use the Windows dark mode setting.",
    .execute_callback = [] { colours::dark_mode_status.set(WI_EnumValue(colours::DarkModeStatus::UseSystemSetting)); },
    .is_radio_selected_callback
    = [] { return colours::dark_mode_status == WI_EnumValue(colours::DarkModeStatus::UseSystemSetting); },
    .is_available_callback = [] { return system_appearance_manager::is_dark_mode_available(); }};

template <class Base = mainmenu_commands>
class GenericMainMenuCommands : public Base {
public:
    template <class... Commands>
    GenericMainMenuCommands(GUID parent, uint32_t sort_priority, Commands&&... commands)
        : m_parent(parent)
        , m_sort_priority(sort_priority)
        , m_commands{std::forward<Commands>(commands)...}
    {
    }

    uint32_t get_command_count() override { return gsl::narrow_cast<uint32_t>(m_commands.size()); }
    GUID get_command(uint32_t p_index) override { return m_commands[p_index].guid; }
    void get_name(uint32_t p_index, pfc::string_base& p_out) override { p_out = m_commands[p_index].name; }

    bool get_description(uint32_t p_index, pfc::string_base& p_out) override
    {
        p_out = m_commands[p_index].description;
        return true;
    }

    GUID get_parent() override { return m_parent; }
    uint32_t get_sort_priority() override { return m_sort_priority; }

    bool get_display(uint32_t p_index, pfc::string_base& p_text, uint32_t& p_flags) override
    {
        const auto& command = m_commands[p_index];
        p_text = command.name;

        if (main_window.get_wnd() == nullptr)
            return false;

        if (command.is_available_callback && !command.is_available_callback())
            return false;

        if (command.is_ticked_callback && command.is_ticked_callback())
            p_flags |= mainmenu_commands::flag_checked;

        if (command.is_radio_selected_callback && command.is_radio_selected_callback())
            p_flags |= mainmenu_commands::flag_radiochecked;

        const bool shift_down = GetKeyState(VK_SHIFT) < 0;
        return !command.hide_without_shift_key || shift_down;
    }

    void execute(uint32_t p_index, service_ptr_t<service_base> p_callback) override
    {
        m_commands[p_index].execute_callback();
    }

protected:
    GUID m_parent{};
    uint32_t m_sort_priority{};
    std::vector<MainMenuCommand> m_commands;
};

using MainMenuCommands = GenericMainMenuCommands<mainmenu_commands>;

class MainMenuCommandsWithState : public GenericMainMenuCommands<mainmenu_commands_v3> {
public:
    using GenericMainMenuCommands::GenericMainMenuCommands;

    uint32_t allowed_check_flags(uint32_t idx, const GUID& subCmd) override
    {
        const auto& command = m_commands[idx];
        return (command.is_ticked_callback ? mainmenu_commands::flag_checked : 0)
            | (command.is_radio_selected_callback ? mainmenu_commands::flag_radiochecked : 0);
    }
    void add_state_callback(state_callback* callback) override { m_state_callbacks.emplace_back(callback); }
    void remove_state_callback(state_callback* callback) override { std::erase(m_state_callbacks, callback); }

    void dispatch_state_changed_event(GUID command_id) const
    {
        for (auto* callback : m_state_callbacks)
            callback->menu_state_changed(command_id, GUID{});
    }

private:
    std::vector<state_callback*> m_state_callbacks;
};

service_factory_single_t<MainMenuCommandsWithState> mainmenu_commands_view_always_on_top_group(
    mainmenu_groups::view_alwaysontop, mainmenu_commands::sort_priority_base + 1, toggle_lock_main_window_size,
    toggle_transparency);

service_factory_single_t<MainMenuCommands> _mainmenu_commands_view_ui_parts(groups::view_columns_part,
    mainmenu_commands::sort_priority_base + 3, show_status_bar, show_status_pane, show_toolbars);

service_factory_single_t<MainMenuCommands> _mainmenu_commands_layout(
    groups::view_layout_commands, mainmenu_commands::sort_priority_base, live_editing, toggle_now_playing_view,
    show_now_playing_cover, now_playing_style_centered, now_playing_style_spectrum, now_playing_style_info,
    cycle_now_playing_style);

service_factory_single_t<MainMenuCommands> _mainmenu_commands_playlist_misc(
    groups::playlist_misc_part, mainmenu_commands::sort_priority_dontcare, playlist_view_search, activate_now_playing);

service_factory_single_t<MainMenuCommands> _mainmenu_commands_playlist_settings(
    groups::playlist_settings_part, mainmenu_commands::sort_priority_dontcare, show_groups, show_artwork);

service_factory_single_t<MainMenuCommands> _mainmenu_commands_playlist_font(
    groups::playlist_font_part, mainmenu_commands::sort_priority_dontcare, increase_font, decrease_font);

service_factory_single_t<MainMenuCommands> _mainmenu_commands_view_misc(
    mainmenu_groups::view, mainmenu_commands::sort_priority_dontcare, focus_filter_search);

service_factory_single_t<MainMenuCommands> _mainmenu_commands_view_mode_toggle(
    groups::view_mode_toggle_part, mainmenu_commands::sort_priority_dontcare, toggle_mode);

service_factory_single_t<MainMenuCommands> _mainmenu_commands_view_mode_modes(groups::view_mode_modes_part,
    mainmenu_commands::sort_priority_dontcare, use_light_mode, use_dark_mode, use_system_mode);

class MainMenuLayoutPresets : public mainmenu_commands {
    uint32_t get_command_count() override { return gsl::narrow<uint32_t>(cfg_layout.get_presets().size()); }
    GUID get_command(uint32_t p_index) override
    {
        pfc::string8 name;
        pfc::string8 buff;
        get_name(p_index, name);
        buff << "[View/Layout] " << name;
        return hasher_md5::get()->process_single_guid(buff.get_ptr(), buff.get_length());
    }
    void get_name(uint32_t p_index, pfc::string_base& p_out) override
    {
        if (p_index < cfg_layout.get_presets().size())
            p_out = cfg_layout.get_presets()[p_index].name;
    }
    bool get_description(uint32_t p_index, pfc::string_base& p_out) override
    {
        if (p_index < cfg_layout.get_presets().size()) {
            pfc::string8 name;
            get_name(p_index, name);
            p_out.reset();
            p_out << "Changes active layout to " << name << ".";
        }
        return false;
    }
    bool get_display(uint32_t p_index, pfc::string_base& p_text, uint32_t& p_flags) override
    {
        if (g_layout_window.get_wnd()) {
            p_flags = p_index == cfg_layout.get_active() ? flag_radiochecked : 0;
            get_name(p_index, p_text);
            return true;
        }
        return false;
    }
    void execute(uint32_t p_index, service_ptr_t<service_base> p_callback) override
    {
        fb2k::inMainThread([p_index] {
            if (!g_layout_window.get_wnd() || p_index >= cfg_layout.get_presets().size())
                return;

            cfg_layout.save_active_preset();
            cfg_layout.set_active_preset(p_index);
        });
    }

    GUID get_parent() override { return groups::view_layout_presets; }
    uint32_t get_sort_priority() override { return sort_priority_dontcare; }
};

mainmenu_commands_factory_t<MainMenuLayoutPresets> _mainmenu_commands_layout_presets;

} // namespace

void on_transparency_changed()
{
    mainmenu_commands_view_always_on_top_group.get_static_instance().dispatch_state_changed_event(
        toggle_transparency.guid);
}

void on_lock_window_size_changed()
{
    mainmenu_commands_view_always_on_top_group.get_static_instance().dispatch_state_changed_event(
        toggle_lock_main_window_size.guid);
}

} // namespace commands

uint32_t get_now_playing_style()
{
    return commands::now_playing_style.get();
}

} // namespace cui::main_menu
