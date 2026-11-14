#include "pch.h"

#include <mutex>

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
#include "config.h"
#include "nocturne_text.h"
#include "next_track_toast.h"
#include "volume_chip.h"
#include "expanded_cover_bg.h"
#include "now_playing_cover_zoom.h"
#include "now_playing_style_transition.h"
#include "now_playing_mode_picker.h"
#include "playlist_item_helpers.h"
#include "cover_line.h"

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

// 自編版修改：fb2k 核心記住的「正在播的那一項」被移出清單時（例如 Album list 自動送了別的資料夾，換回來後是新加入的項目），
// highlight_playing_item 會失敗，改成在目前清單裡找同一首歌，選取並捲到那一列（2026-11-02）
void activate_now_playing_item()
{
    const auto playlists = playlist_manager::get();

    if (playlists->highlight_playing_item())
        return;

    metadb_handle_ptr track;
    if (!play_control::get()->get_now_playing(track))
        return;

    const auto playlist = playlists->get_active_playlist();
    if (playlist == pfc_infinite)
        return;

    pfc::list_t<metadb_handle_ptr> items;
    playlists->playlist_get_all_items(playlist, items);

    const auto index = items.find_item(track);
    if (index == pfc_infinite)
        return;

    playlists->playlist_set_focus_item(playlist, index);
    playlists->playlist_set_selection(playlist, bit_array_true(), bit_array_one(index));
}

const MainMenuCommand activate_now_playing{activate_now_playing_id, "Activate now playing",
    "Activates the currently playing item.", [] { activate_now_playing_item(); }};

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

// 自編版新增：「暫停就位」（2026-11-12）——規則是「沒在播就沒有展開頁」
// - 停止中點播放列、佇列有歌：讓佇列第一首暫停在 0:00 就位再展開，展開頁就是那首真正的樣子（封面、歌詞、背景）
// - 還沒按過播放就停止或收起：把那首放回佇列最前面，等於什麼都沒發生（播放次數也不會動）
// - 按過播放（取消暫停）之後就是一般的播放，停止時不放回
namespace staged {

// 就位前記下的佇列第一首（含它原本在哪個播放清單第幾首，放回時盡量照原樣）
t_playback_queue_item g_head{};
// 已經呼叫 start()、還在等這首開檔
bool g_pending{};
// 這首已經就位、還沒按過播放
bool g_active{};

void clear()
{
    g_head = {};
    g_pending = false;
    g_active = false;
}

bool start_from_queue()
{
    const auto playlists = playlist_manager::get();
    pfc::list_t<t_playback_queue_item> queue;
    playlists->queue_get_contents(queue);

    if (queue.get_count() == 0)
        return false;

    g_head = queue[0];
    g_pending = true;
    g_active = false;
    playback_control::get()->start(playback_control::track_command_play, true);
    return true;
}

// 把就位的那首放回佇列最前面：佇列沒有「插到第幾個」的 API，只能清掉再照順序加回去
void put_head_back()
{
    const auto playlists = playlist_manager::get();
    pfc::list_t<t_playback_queue_item> rest;
    playlists->queue_get_contents(rest);

    const auto add = [&playlists](const t_playback_queue_item& entry) {
        // 播放清單在這段時間被改過（刪歌、排序）時，原本的位置可能已經是別首，改用曲目本身加
        metadb_handle_ptr at_position;
        if (entry.m_playlist != pfc_infinite
            && playlists->playlist_get_item_handle(at_position, entry.m_playlist, entry.m_item)
            && at_position == entry.m_handle)
            playlists->queue_add_item_playlist(entry.m_playlist, entry.m_item);
        else
            playlists->queue_add_item(entry.m_handle);
    };

    playlists->queue_flush();
    add(g_head);
    for (size_t index = 0; index < rest.get_count(); index++)
        add(rest[index]);
}

} // namespace staged

const MainMenuCommand toggle_now_playing_view{toggle_now_playing_view_id, nocturne_text::toggle_now_playing_name,
    nocturne_text::toggle_now_playing_description,
    [] {
        const auto playback = playback_control::get();

        if (!FlatSplitterPanel::s_is_now_playing_expanded()) {
            // 停止中：佇列有歌就讓第一首暫停就位再展開，沒有就不展開；暫停算播放中
            if (!playback->is_playing() && !staged::start_from_queue())
                return;
        } else if (staged::g_pending || staged::g_active) {
            // 就位後還沒播就收起：停止並放回佇列（停止時的回呼會收合展開頁），回到點開前的樣子
            playback->stop();
            return;
        }

        FlatSplitterPanel::s_toggle_now_playing();
    },
    [] { return FlatSplitterPanel::s_is_now_playing_expanded(); }};

// 自編版新增：ESLyric 播放列的停止鈕用 mm://View/Layout/停止或收起播放頁
// - 播放中（含暫停）：照常停止，展開頁跟著收起（見下面的 NowPlayingViewPlayCallback）
// - 已經停止、展開頁還開著（照理不會發生，保險用）：收起展開頁
// - 已經停止而且已收合：什麼都不做
const MainMenuCommand stop_or_collapse{stop_or_collapse_id, nocturne_text::stop_or_collapse_name,
    nocturne_text::stop_or_collapse_description,
    [] {
        const auto playback = playback_control::get();

        if (playback->is_playing()) {
            playback->stop();
            return;
        }

        if (FlatSplitterPanel::s_is_now_playing_expanded())
            FlatSplitterPanel::s_toggle_now_playing();
    }};

// 自編版新增：展開頁右上角的「下一首」提示開關（View/Layout/下一首提示），設定在設定頁也看得到，是同一個設定
const MainMenuCommand next_track_toast_toggle{next_track_toast_toggle_id, nocturne_text::next_track_toggle_name,
    nocturne_text::next_track_toggle_description, [] { next_track_toast::enabled = !next_track_toast::enabled.get(); },
    [] { return next_track_toast::enabled.get(); }};

// 自編版新增：播放停止（按停止、清單播完）時，展開中的播放頁一律收回（2026-11-12 起佇列有歌也收，見上面的 staged）；
// 換曲與結束 fb2k 不處理。也在這裡追蹤「暫停就位」的那首有沒有真的播過
class NowPlayingViewPlayCallback : public play_callback_static {
public:
    unsigned get_flags() override
    {
        return flag_on_playback_starting | flag_on_playback_new_track | flag_on_playback_pause | flag_on_playback_stop;
    }

    void on_playback_stop(play_control::t_stop_reason reason) override
    {
        // 換曲（按下一首、雙擊別首）：就位的那首被跳過了，不放回
        if (reason == play_control::stop_reason_starting_another || reason == play_control::stop_reason_shutting_down) {
            staged::clear();
            return;
        }

        if (staged::g_active)
            staged::put_head_back();
        staged::clear();

        if (FlatSplitterPanel::s_is_now_playing_expanded())
            FlatSplitterPanel::s_toggle_now_playing();
    }

    // 不是暫停開始的播放（按播放、雙擊）：就不是就位了
    void on_playback_starting(play_control::t_track_command command, bool paused) override
    {
        if (!paused)
            staged::clear();
    }

    void on_playback_new_track(metadb_handle_ptr track) override
    {
        staged::g_active = staged::g_pending && track == staged::g_head.m_handle;
        staged::g_pending = false;
    }

    // 取消暫停＝真的開始播了
    void on_playback_pause(bool state) override
    {
        if (!state)
            staged::clear();
    }

    void on_playback_seek(double time) override {}
    void on_playback_edited(metadb_handle_ptr track) override {}
    void on_playback_dynamic_info(const file_info& info) override {}
    void on_playback_dynamic_info_track(const file_info& info) override {}
    void on_playback_time(double time) override {}
    void on_volume_change(float new_value) override {}
};

play_callback_static_factory_t<NowPlayingViewPlayCallback> now_playing_view_play_callback;

// 自編版新增：展開頁樣式，1 封面（預設）、3 曲目資訊卡（在封面的位置切換；原本叫「置中」「靠左＋資訊卡」），ESLyric 佈局用 %cui_now_playing_style% 切換
// （原本的 2「靠左＋VU 表」已在 2026-11-07 拿掉，編號不重排：舊設定裡存的值是 2 的話一律當成 1，見 now_playing_style_value）
// ESLyric 的 visibility 不會隨時重算，所以切換後對正在播放的曲目送一次 dispatch_refresh，讓佈局重新套用
fbh::ConfigUint32 now_playing_style(
    GUID{0xdd906dca, 0x9b49, 0x492c, {0x8b, 0xb2, 0x43, 0xb9, 0xef, 0xe4, 0xf1, 0x8f}}, 1);

// 目前生效的樣式：只有 3 是曲目資訊卡，其他值（含已拿掉的 2）都是封面
uint32_t now_playing_style_value()
{
    return now_playing_style.get() == 3 ? 3 : 1;
}

// 對正在播放的曲目送一次 dispatch_refresh，讓 ESLyric 重算標題格式與 visibility
void refresh_now_playing_track()
{
    metadb_handle_ptr track;
    if (playback_control::get()->get_now_playing(track))
        metadb_io::get()->dispatch_refresh(track);
}

void set_now_playing_style(uint32_t style)
{
    const auto apply = [style] {
        now_playing_style = style;
        refresh_now_playing_track();
        // 封面右下角的放大鈕只在封面模式顯示（資訊卡模式藏起來）
        cover_zoom::refresh_layouts();
    };

    // 樣式沒變（例如主選單點了目前這個）就不做動畫
    if ((style == 3 ? 3u : 1u) == now_playing_style_value()) {
        apply();
        return;
    }

    // 封面⇄資訊卡的過渡動畫（展開頁完全展開、找得到封面位置時才有，見 now_playing_style_transition.h）
    style_transition::run(apply);
}

// 自編版新增（2026-11-11，v1.5 待做清單第 4 個）：展開頁顯示內容，1 歌詞（預設）、2 純封面、3 VU 表。
// ESLyric 佈局用 %cui_now_playing_mode% 切換歌詞欄與純封面那一塊；VU 表是 Columns UI 的另一個面板（自訂標題「展開頁右側」），
// 由分割面板疊到歌詞欄的位置。跟封面⇄資訊卡（now_playing_style）是兩回事：模式換了樣式不動，收合也不會重設
fbh::ConfigUint32 now_playing_mode(
    GUID{0x0bf54940, 0x3466, 0x4045, {0x84, 0xb7, 0x54, 0x87, 0x83, 0xad, 0xe7, 0x5a}}, 1);

void refresh_now_playing_track();

// 標題格式欄位可能在別的執行緒算，不能在那裡查版面，所以「有沒有 VU 表」在主執行緒每次查的時候存一份給欄位用；
// 變了（例如啟動時版面剛建好）就通知 ESLyric 重算
std::atomic<bool> vu_available_for_fields{false};

// VU 表模式要 main.xml 標記有歌詞欄位置（不然不知道放哪）。要在主執行緒呼叫
// 2026-11-11 起 VU 表改由 Columns UI 自己畫（now_playing_vu_meter），不再需要外掛 VU 元件與「展開頁右側」面板
bool vu_mode_available()
{
    // 用一個夠大的假展開頁問標記有沒有歌詞欄位置（只看有沒有，不看大小）
    const auto available = cover_zoom::lyrics_column_in_stage(10000, 10000).has_value();
    if (vu_available_for_fields.exchange(available) != available
        && main_menu::mode_needs_lyrics_column(static_cast<main_menu::NowPlayingMode>(now_playing_mode.get())))
        fb2k::inMainThread([] { refresh_now_playing_track(); });
    return available;
}

// 設定頁勾選的模式（bit0 歌詞、bit1 純封面、bit2 VU 表）：給標題格式欄位用的副本，主執行緒每次讀設定時更新
std::atomic<uint32_t> enabled_modes_for_fields{main_menu::now_playing_modes::all_modes};

// 主執行緒：讀勾選的模式，變了就通知 ESLyric 重算
uint32_t enabled_modes_mask()
{
    const auto mask = main_menu::now_playing_modes::get_mask();
    if (enabled_modes_for_fields.exchange(mask) != mask)
        fb2k::inMainThread([] { refresh_now_playing_track(); });
    return mask;
}

// 存的模式在「有沒有勾選」「VU 表／頻譜能不能用」的限制下實際顯示哪一種：存的那種不能用時取第一個能用的
// （歌詞、純封面、VU 表、頻譜的順序），都不能用時是歌詞。純函式，任何執行緒都可以用
uint32_t effective_mode(uint32_t stored, uint32_t mask, bool vu_available)
{
    constexpr auto count = main_menu::now_playing_mode_count;
    const auto usable = [&](uint32_t mode) {
        return (mask & (1u << (mode - 1))) != 0
            && (vu_available || !main_menu::mode_needs_lyrics_column(static_cast<main_menu::NowPlayingMode>(mode)));
    };
    if (stored >= 1 && stored <= count && usable(stored))
        return stored;
    for (const auto mode : main_menu::now_playing_mode_order)
        if (usable(static_cast<uint32_t>(mode)))
            return static_cast<uint32_t>(mode);
    return 1;
}

// 目前生效的模式（主執行緒）
main_menu::NowPlayingMode now_playing_mode_value()
{
    return static_cast<main_menu::NowPlayingMode>(
        effective_mode(now_playing_mode.get(), enabled_modes_mask(), vu_mode_available()));
}

// 這一種現在能不能選（有勾選、VU 表要版面能用）
bool mode_usable(main_menu::NowPlayingMode mode)
{
    if ((enabled_modes_mask() & main_menu::now_playing_modes::bit_of(mode)) == 0)
        return false;
    return !main_menu::mode_needs_lyrics_column(mode) || vu_mode_available();
}

// 給標題格式欄位用（任何執行緒）：同 now_playing_mode_value，但只讀主執行緒存好的副本
uint32_t now_playing_mode_for_fields()
{
    return effective_mode(now_playing_mode.get(), enabled_modes_for_fields.load(), vu_available_for_fields.load());
}

// 「換下一種」時左上角按鈕旁邊的提示：顯示中的模式（1～4），0＝不顯示。約 1.1 秒後自動清掉
std::atomic<uint32_t> mode_toast{0};
UINT_PTR mode_toast_timer{};
constexpr UINT mode_toast_ms = 1100;

void hide_mode_toast()
{
    if (mode_toast_timer) {
        KillTimer(nullptr, std::exchange(mode_toast_timer, 0));
    }
    if (mode_toast.exchange(0) != 0)
        refresh_now_playing_track();
}

void show_mode_toast(main_menu::NowPlayingMode mode)
{
    if (mode_toast_timer)
        KillTimer(nullptr, std::exchange(mode_toast_timer, 0));
    mode_toast = static_cast<uint32_t>(mode);
    refresh_now_playing_track();
    mode_toast_timer = SetTimer(nullptr, 0, mode_toast_ms, [](HWND, UINT, UINT_PTR, DWORD) { hide_mode_toast(); });
}

void set_now_playing_mode(main_menu::NowPlayingMode mode)
{
    if (!mode_usable(mode))
        return;

    const auto apply = [mode] {
        now_playing_mode = static_cast<uint32_t>(mode);
        refresh_now_playing_track();
        // 放大鈕（純封面時藏起來）、VU 表面板、歌詞防誤拖蓋層跟著重排
        panels::splitter::FlatSplitterPanel::s_refresh_now_playing();
    };

    if (mode == now_playing_mode_value()) {
        apply();
        return;
    }

    // 整個展開頁淡入淡出（展開頁完全展開時才有，見 now_playing_style_transition.h）
    style_transition::run_stage(apply);
}

// 「換下一種」的順序照 now_playing_mode_order（純封面 → 歌詞 → VU 表 → 頻譜 → 歌詞＋VU → 歌詞＋頻譜 → 純封面），
// 跳過沒勾選或不能用的；只剩一種時就是它自己
main_menu::NowPlayingMode next_now_playing_mode()
{
    constexpr auto count = main_menu::now_playing_mode_count;
    const auto current_mode = now_playing_mode_value();
    uint32_t current = 0;
    for (uint32_t i = 0; i < count; ++i)
        if (main_menu::now_playing_mode_order[i] == current_mode)
            current = i;
    for (uint32_t step = 1; step <= count; ++step) {
        const auto candidate = main_menu::now_playing_mode_order[(current + step) % count];
        if (mode_usable(candidate))
            return candidate;
    }
    return current_mode;
}

// 「跳出選單」：只列設定頁勾選的。2026-11-12 起用版面縮圖卡（now_playing_mode_picker，使用者看過預覽選 B）；
// 卡片開不起來（GDI+ 字型還沒暖機好）時才用下面的系統選單：在游標位置、目前的打勾、版面不支援的變灰並註明
void show_now_playing_mode_menu()
{
    {
        const auto current = now_playing_mode_value();
        const auto vu_available = vu_mode_available();
        const auto mask = enabled_modes_mask();
        std::vector<mode_picker::Item> items;
        for (const auto mode : main_menu::now_playing_mode_order) {
            if ((mask & main_menu::now_playing_modes::bit_of(mode)) == 0)
                continue;
            const auto available = vu_available || !main_menu::mode_needs_lyrics_column(mode);
            items.push_back({mode, nocturne_text::mode_short_names[main_menu::mode_name_index(mode)], available});
        }
        if (mode_picker::open(std::move(items), current, [](main_menu::NowPlayingMode mode) { set_now_playing_mode(mode); }))
            return;
    }

    const wil::unique_hmenu menu(CreatePopupMenu());
    if (!menu)
        return;

    const auto current = now_playing_mode_value();
    const auto vu_available = vu_mode_available();
    const auto mask = enabled_modes_mask();
    const auto add = [&](main_menu::NowPlayingMode mode, const wchar_t* text, bool enabled) {
        if ((mask & main_menu::now_playing_modes::bit_of(mode)) == 0)
            return;
        UINT flags = MF_STRING;
        if (mode == current)
            flags |= MF_CHECKED;
        if (!enabled)
            flags |= MF_GRAYED;
        AppendMenu(menu.get(), flags, static_cast<UINT_PTR>(mode), text);
    };
    for (const auto mode : main_menu::now_playing_mode_order) {
        const auto available = vu_available || !main_menu::mode_needs_lyrics_column(mode);
        add(mode, nocturne_text::mode_short_names[main_menu::mode_name_index(mode, available)], available);
    }

    POINT cursor{};
    GetCursorPos(&cursor);
    const auto owner = core_api::get_main_window();
    // TrackPopupMenu 規定擁有者要在前景，不然點選單外面不會關
    SetForegroundWindow(owner);
    const auto chosen
        = TrackPopupMenu(menu.get(), TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN, cursor.x, cursor.y, 0, owner, nullptr);
    if (chosen >= 1 && chosen <= static_cast<int>(main_menu::now_playing_mode_count))
        set_now_playing_mode(static_cast<main_menu::NowPlayingMode>(chosen));
}

// 左上角按鈕左鍵：換下一種（旁邊提示模式名稱）。右鍵跳卡片在 splitter_window.cpp 的播放列子類別處理
// （2026-11-12 使用者同意：原本設定頁二選一，改成左右鍵各一種，拿掉設定）
void on_now_playing_mode_button()
{
    const auto next = next_now_playing_mode();
    set_now_playing_mode(next);
    show_mode_toast(next);
}

// 自編版新增：暫停時凍結進度條用的位置與曲長（毫秒），沒在暫停時位置為 -1
// Output 用 ASIO+DSD 時，暫停中 fb2k 回報的位置會每 0.2 秒往前爬又退回（2026-10-27 量測），ESLyric 的 seekbar 跟著抖，
// 所以按下暫停時記下當下的值，ESLyric 佈局暫停時改顯示讀這兩個值的 progressbar；暫停中跳轉改記跳轉目標
// 標題格式可能在別的執行緒算，所以存 atomic；欄位以位置判斷有沒有值，曲長只在位置有值時讀，所以先寫曲長再寫位置
std::atomic<int64_t> paused_position_ms{-1};
std::atomic<int64_t> paused_length_ms{-1};

// 進度條預覽的位置（毫秒），沒有預覽時為 -1（見 main_menu::set_seek_preview）；給 %cui_time_text% 用
std::atomic<int64_t> seek_preview_ms{-1};

// 跟 %playback_time% 同樣的寫法：不到一小時「M:SS」，一小時以上「H:MM:SS」
std::string format_clock(int64_t milliseconds)
{
    const auto total = milliseconds / 1000;
    const auto hours = total / 3600;
    const auto minutes = (total % 3600) / 60;
    const auto seconds = total % 60;

    if (hours > 0)
        return fmt::format("{}:{:02}:{:02}", hours, minutes, seconds);

    return fmt::format("{}:{:02}", minutes, seconds);
}

void clear_paused_position()
{
    if (paused_position_ms.exchange(-1) >= 0)
        refresh_now_playing_track();
}

void set_paused_position(double position)
{
    const double length = playback_control::get()->playback_get_length();

    // 長度不明（網路串流等）沒有進度可凍結
    if (length <= 0) {
        clear_paused_position();
        return;
    }

    paused_length_ms = std::llround(length * 1000);
    paused_position_ms = std::llround(std::clamp(position, 0.0, length) * 1000);
    refresh_now_playing_track();
}

class PausedPositionPlayCallback : public play_callback_static {
public:
    unsigned get_flags() override
    {
        return flag_on_playback_new_track | flag_on_playback_seek | flag_on_playback_pause | flag_on_playback_stop;
    }

    // 換曲時通常已在播放；開 fb2k 時恢復成暫停的情況，換曲當下就是暫停
    void on_playback_new_track(metadb_handle_ptr track) override
    {
        const auto api = playback_control::get();
        if (api->is_paused())
            set_paused_position(api->playback_get_position());
        else
            clear_paused_position();
    }

    void on_playback_seek(double time) override
    {
        if (playback_control::get()->is_paused())
            set_paused_position(time);
        else
            clear_paused_position();
    }

    void on_playback_pause(bool state) override
    {
        if (state)
            set_paused_position(playback_control::get()->playback_get_position());
        else
            clear_paused_position();
    }

    void on_playback_stop(play_control::t_stop_reason reason) override { clear_paused_position(); }

    void on_playback_starting(play_control::t_track_command command, bool paused) override {}
    void on_playback_edited(metadb_handle_ptr track) override {}
    void on_playback_dynamic_info(const file_info& info) override {}
    void on_playback_dynamic_info_track(const file_info& info) override {}
    void on_playback_time(double time) override {}
    void on_volume_change(float new_value) override {}
};

play_callback_static_factory_t<PausedPositionPlayCallback> paused_position_play_callback;

// 自編版新增：記住正在播放的檔案，給標題格式欄位 %cui_now_playing_file% 用
// 播放清單的 ▶ 標記原本靠核心記的「正在播的那一項」，Album list 換了資料夾、再換回來之後那一項已經不在清單裡，標記就沒了；
// 這個欄位改成直接比對檔案，同一首歌出現在任何清單的哪一列都標得出來（2026-11-02）
// 標題格式可能在別的執行緒算，所以用 mutex 保護
std::mutex now_playing_file_mutex;
metadb_handle_ptr now_playing_file;

void set_now_playing_file(const metadb_handle_ptr& track)
{
    metadb_handle_ptr previous;
    {
        std::scoped_lock lock(now_playing_file_mutex);
        previous = now_playing_file;
        now_playing_file = track;
    }
    // 舊的與新的那幾列都要重算（▶ 要換位置）
    panels::playlist_view::PlaylistView::s_on_now_playing_file_change(previous, track);
}

class NowPlayingFilePlayCallback : public play_callback_static {
public:
    unsigned get_flags() override
    {
        return flag_on_playback_new_track | flag_on_playback_pause | flag_on_playback_stop;
    }

    void on_playback_new_track(metadb_handle_ptr track) override { set_now_playing_file(track); }

    // 暫停時符號從 ▶ 變 ❚❚，這幾列也要重算
    void on_playback_pause(bool state) override
    {
        metadb_handle_ptr track;
        {
            std::scoped_lock lock(now_playing_file_mutex);
            track = now_playing_file;
        }
        panels::playlist_view::PlaylistView::s_on_now_playing_file_change(track, track);
    }

    void on_playback_stop(play_control::t_stop_reason reason) override { set_now_playing_file({}); }

    void on_playback_starting(play_control::t_track_command command, bool paused) override {}
    void on_playback_seek(double time) override {}
    void on_playback_edited(metadb_handle_ptr track) override {}
    void on_playback_dynamic_info(const file_info& info) override {}
    void on_playback_dynamic_info_track(const file_info& info) override {}
    void on_playback_time(double time) override {}
    void on_volume_change(float new_value) override {}
};

play_callback_static_factory_t<NowPlayingFilePlayCallback> now_playing_file_play_callback;

// 自編版新增：標題格式欄位 %cui_now_playing_expanded%，播放頁展開時為 1、收合時不存在，給 ESLyric 佈局切換 ⌃／⌄
// 以及 %cui_now_playing_style%（展開頁樣式 1～3）
// 以及 %cui_paused_position_ms%／%cui_paused_length_ms%（暫停時的位置與曲長，毫秒；沒在暫停時不存在）
// 以及 %cui_now_playing_file%（這一列的歌就是正在播放的檔案時為 1，否則不存在；播放清單的 ▶ 標記用）
// 標題格式可能在別的執行緒算，所以展開狀態讀 atomic，不碰分割面板本身；樣式只是單一 32 位元整數設定值，直接讀
class NowPlayingViewFieldProvider : public metadb_display_field_provider {
public:
    t_uint32 get_field_count() override { return 15; }
    void get_field_name(t_uint32 index, pfc::string_base& out) override
    {
        constexpr const char* names[]{"cui_now_playing_expanded", "cui_now_playing_style", "cui_paused_position_ms",
            "cui_paused_length_ms", "cui_now_playing_file", "cui_time_text", "cui_volume_icon", "cui_expanded_bg",
            "cui_now_playing_mode", "cui_now_playing_mode_toast", "cui_queue_allow_duplicates", "cui_expanded_compact",
            "cui_cover_line", "cui_next_title", "cui_next_artist"};
        out = names[index];
    }

    bool process_field(t_uint32 index, metadb_handle* handle, titleformat_text_out* out) override
    {
        // %cui_cover_line%：精簡時封面下那一排顯示什麼（1 下一首、2 等級＋格式、3 播放統計；右鍵挑選卡選，見 cover_line.h）
        if (index == 12) {
            out->write(titleformat_inputtypes::meta, pfc::format_uint(cover_line::style_for_fields()));
            return true;
        }
        // %cui_next_title%／%cui_next_artist%：下一首的歌名、藝人（規則同下一首提示；沒有下一首時不存在）
        if (index == 13 || index == 14) {
            std::string text;
            if (!cover_line::next_text_for_fields(index == 14, text))
                return false;
            out->write(titleformat_inputtypes::meta, text.c_str());
            return true;
        }

        // %cui_expanded_compact%：設定頁「封面下方資訊」選精簡時為 1，詳細時不存在（2026-11-14）。
        // main.xml 用它換展開頁左欄（精簡只留規格列、封面放大）；舊版 DLL 沒有這個欄位時版面自然當成詳細
        if (index == 11) {
            if (!cover_zoom::compact_info.get())
                return false;
            out->write(titleformat_inputtypes::meta, "1");
            return true;
        }

        // %cui_queue_allow_duplicates%：設定頁勾了「同一首可以重複加入佇列」時為 1，沒勾時不存在（2026-11-12）。
        // 標頭網頁按「加入佇列」前讀它，決定要不要跳過已在佇列的歌
        if (index == 10) {
            if (!playlist_item_helpers::allow_duplicate_queue.get())
                return false;
            out->write(titleformat_inputtypes::meta, "1");
            return true;
        }

        if (index == 1) {
            out->write(titleformat_inputtypes::meta, pfc::format_uint(now_playing_style_value()));
            return true;
        }

        // %cui_now_playing_mode%：展開頁顯示內容 1 歌詞、2 純封面、3 VU 表（2026-11-11）、4 頻譜、5 歌詞＋VU 表、6 歌詞＋頻譜（2026-11-12）
        if (index == 8) {
            out->write(titleformat_inputtypes::meta, pfc::format_uint(now_playing_mode_for_fields()));
            return true;
        }

        // %cui_now_playing_mode_toast%：左上角按鈕「換下一種」後約 1.1 秒內是新模式的名稱，其餘時間不存在
        if (index == 9) {
            const auto toast = mode_toast.load();
            if (toast < 1 || toast > main_menu::now_playing_mode_count)
                return false;
            // 名稱跟彈出卡、設定頁共用 mode_short_names（2026-11-12 拿掉重複的 mode_toast_names）
            const pfc::stringcvt::string_utf8_from_wide name(
                nocturne_text::mode_short_names[main_menu::mode_name_index(static_cast<main_menu::NowPlayingMode>(toast))]);
            out->write(titleformat_inputtypes::meta, name.get_ptr());
            return true;
        }

        // %cui_expanded_bg%：展開頁「封面虛化背景」用的圖檔完整路徑（2026-11-08）。沒勾、沒封面或還沒做好時不存在。
        // ESLyric 面板參數選項 → 外觀「背景類型：圖片」、圖片頁「來源：自訂、值填 %cui_expanded_bg%」會把它當整個展開頁的背景（見 expanded_cover_bg.h）
        if (index == 7) {
            if (!handle)
                return false;

            const auto path = expanded_cover_bg::background_path_for(metadb_handle_ptr(handle));
            if (!path || path->empty())
                return false;

            out->write(titleformat_inputtypes::meta, path->c_str());
            return true;
        }

        // %cui_volume_icon%：設定頁勾了「音量條收成圖示」時為 1，沒勾時不存在（2026-11-06）。ESLyric 佈局用
        // visibility="$if(%cui_volume_icon%,0,1)" 隱藏 ESLyric 的音量條、dB 與靜音鈕（這時改由 Columns UI 畫喇叭圖示與彈出滑桿），
        // 隱藏的元件不占版面，右邊那串自然縮成 144。只是一個設定值，別的執行緒直接讀
        if (index == 6) {
            if (!volume_chip::enabled.get())
                return false;

            out->write(titleformat_inputtypes::meta, "1");
            return true;
        }

        // %cui_time_text%：進度條預覽的位置（拖曳、滾輪、等 fb2k 跳完），沒有預覽時是暫停的位置，兩者都沒有就不存在
        // （佈局用 $if2(%cui_time_text%,%playback_time%) 取代只寫 %playback_time%）
        if (index == 5) {
            auto position = seek_preview_ms.load();
            if (position < 0)
                position = paused_position_ms.load();
            if (position < 0)
                return false;

            out->write(titleformat_inputtypes::meta, format_clock(position).c_str());
            return true;
        }

        if (index == 4) {
            std::scoped_lock lock(now_playing_file_mutex);
            if (!handle || handle != now_playing_file.get_ptr())
                return false;

            out->write(titleformat_inputtypes::meta, "1");
            return true;
        }

        if (index >= 2) {
            const auto position = paused_position_ms.load();
            if (position < 0)
                return false;

            out->write(titleformat_inputtypes::meta, pfc::format_int(index == 2 ? position : paused_length_ms.load()));
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
const MainMenuCommand show_now_playing_cover{show_now_playing_cover_id, nocturne_text::show_cover_name,
    nocturne_text::show_cover_description,
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

// 自編版新增：主題字型。2026-11-12 起打開設定頁的「字型」分頁（舊對話框退休；命令名稱與 GUID 不變，綁過的快速鍵照用）
const MainMenuCommand theme_font{theme_font_id, nocturne_text::theme_font_name, nocturne_text::theme_font_description,
    [] { prefs::show_nocturne_font_tab(); }};

const MainMenuCommand now_playing_style_centered{now_playing_style_centered_id, nocturne_text::style_centered_name,
    nocturne_text::style_centered_description, [] { set_now_playing_style(1); }, {},
    [] { return now_playing_style_value() == 1; }};

const MainMenuCommand now_playing_style_info{now_playing_style_info_id, nocturne_text::style_info_name,
    nocturne_text::style_info_description, [] { set_now_playing_style(3); }, {},
    [] { return now_playing_style_value() == 3; }};

// ESLyric 展開頁的封面／資訊卡熱區用 mm://View/Layout/切換展開頁樣式，在封面與曲目資訊卡之間切換
const MainMenuCommand cycle_now_playing_style{cycle_now_playing_style_id, nocturne_text::cycle_style_name,
    nocturne_text::cycle_style_description,
    [] { set_now_playing_style(now_playing_style_value() == 1 ? 3 : 1); }};

// 展開頁顯示內容（2026-11-11）：三種各一個選單項目（目前的打圓點），VU 表沒有時不出現
const MainMenuCommand now_playing_mode_lyrics{now_playing_mode_lyrics_id, nocturne_text::mode_lyrics_name,
    nocturne_text::mode_lyrics_description, [] { set_now_playing_mode(NowPlayingMode::lyrics); }, {},
    [] { return now_playing_mode_value() == NowPlayingMode::lyrics; }};

const MainMenuCommand now_playing_mode_cover{now_playing_mode_cover_id, nocturne_text::mode_cover_name,
    nocturne_text::mode_cover_description, [] { set_now_playing_mode(NowPlayingMode::cover); }, {},
    [] { return now_playing_mode_value() == NowPlayingMode::cover; }};

const MainMenuCommand now_playing_mode_vu{.guid = now_playing_mode_vu_id,
    .name = nocturne_text::mode_vu_name,
    .description = nocturne_text::mode_vu_description,
    .execute_callback = [] { set_now_playing_mode(NowPlayingMode::vu); },
    .is_radio_selected_callback = [] { return now_playing_mode_value() == NowPlayingMode::vu; },
    .is_available_callback = [] { return vu_mode_available(); }};

// ESLyric 展開頁左上角的按鈕用 mm://View/Layout/切換展開頁顯示內容
const MainMenuCommand now_playing_mode_spectrum{.guid = now_playing_mode_spectrum_id,
    .name = nocturne_text::mode_spectrum_name,
    .description = nocturne_text::mode_spectrum_description,
    .execute_callback = [] { set_now_playing_mode(NowPlayingMode::spectrum); },
    .is_radio_selected_callback = [] { return now_playing_mode_value() == NowPlayingMode::spectrum; },
    .is_available_callback = [] { return vu_mode_available(); }};

const MainMenuCommand now_playing_mode_lyrics_vu{.guid = now_playing_mode_lyrics_vu_id,
    .name = nocturne_text::mode_lyrics_vu_name,
    .description = nocturne_text::mode_lyrics_vu_description,
    .execute_callback = [] { set_now_playing_mode(NowPlayingMode::lyrics_vu); },
    .is_radio_selected_callback = [] { return now_playing_mode_value() == NowPlayingMode::lyrics_vu; },
    .is_available_callback = [] { return vu_mode_available(); }};

const MainMenuCommand now_playing_mode_lyrics_spectrum{.guid = now_playing_mode_lyrics_spectrum_id,
    .name = nocturne_text::mode_lyrics_spectrum_name,
    .description = nocturne_text::mode_lyrics_spectrum_description,
    .execute_callback = [] { set_now_playing_mode(NowPlayingMode::lyrics_spectrum); },
    .is_radio_selected_callback = [] { return now_playing_mode_value() == NowPlayingMode::lyrics_spectrum; },
    .is_available_callback = [] { return vu_mode_available(); }};

const MainMenuCommand now_playing_mode_button{now_playing_mode_button_id, nocturne_text::mode_button_name,
    nocturne_text::mode_button_description, [] { on_now_playing_mode_button(); }};

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
    stop_or_collapse, show_now_playing_cover, now_playing_style_centered, now_playing_style_info, cycle_now_playing_style,
    now_playing_mode_cover, now_playing_mode_lyrics, now_playing_mode_vu, now_playing_mode_spectrum,
    now_playing_mode_lyrics_vu, now_playing_mode_lyrics_spectrum, now_playing_mode_button, next_track_toast_toggle,
    theme_font);

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
    return commands::now_playing_style_value();
}

NowPlayingMode get_now_playing_mode()
{
    return commands::now_playing_mode_value();
}

bool is_vu_mode_available()
{
    return commands::vu_mode_available();
}

void show_now_playing_mode_picker()
{
    commands::hide_mode_toast();
    commands::show_now_playing_mode_menu();
}

void on_now_playing_modes_changed()
{
    // 讀一次勾選（同時更新欄位用的副本），再讓 ESLyric 重算、放大鈕／VU 表／歌詞防誤拖跟著重排
    commands::enabled_modes_mask();
    commands::refresh_now_playing_track();
    panels::splitter::FlatSplitterPanel::s_refresh_now_playing();
}

void reset_now_playing_style_to_cover()
{
    if (commands::now_playing_style_value() != 3)
        return;

    commands::now_playing_style = 1;
    commands::refresh_now_playing_track();
    cover_zoom::refresh_layouts();
}

std::optional<double> get_paused_position()
{
    const auto position = commands::paused_position_ms.load();
    if (position < 0)
        return {};

    return static_cast<double>(position) / 1000.0;
}

void set_seek_preview(std::optional<double> seconds)
{
    // 只在主執行緒呼叫，所以「上次通知時顯示的整秒」與時間戳不用 atomic；毫秒值給標題格式（可能在別的執行緒）讀，用 atomic
    static int64_t shown_second = -1;
    static ULONGLONG last_refresh_tick = 0;

    const auto position_ms = seconds ? std::llround(std::max(0.0, *seconds) * 1000.0) : int64_t{-1};
    commands::seek_preview_ms = position_ms;

    const auto second = position_ms < 0 ? int64_t{-1} : position_ms / 1000;
    if (second == shown_second)
        return;

    // 拖得很快時略過太密的通知，下一次呼叫（進度條每格都會呼叫）會補上；結束預覽（回到 -1）一律馬上通知
    const auto now = GetTickCount64();
    if (second >= 0 && now - last_refresh_tick < 30)
        return;

    shown_second = second;
    last_refresh_tick = now;
    commands::refresh_now_playing_track();
}

void refresh_layout_fields()
{
    // 正在播放：對播放中的曲目送 dispatch_refresh（跟切換展開頁樣式同一招）。沒在播放時 get_now_playing 會失敗，
    // 改對播放清單反白的那首送，讓停止狀態下切換設定也有機會讓 ESLyric 重算 visibility
    metadb_handle_ptr track;
    if (playback_control::get()->get_now_playing(track)) {
        metadb_io::get()->dispatch_refresh(track);
        return;
    }

    if (playlist_manager::get()->activeplaylist_get_focus_item_handle(track) && track.is_valid())
        metadb_io::get()->dispatch_refresh(track);
}

} // namespace cui::main_menu
