#include "pch.h"

// 自編版新增（2026-11-14）：換資料夾後，正在播的那首播完要接它原本的下一首。
//
// Album list 換資料夾會把「目前選取資料夾」清空重建，正在播的那首因此脫離清單（「孤立」：核心不再知道它是第幾項）。
// 「播放跟隨游標」開著時，核心播完會接焦點那一列：切回原資料夾沒點任何歌＝跳回第一首；
// 單擊正在播的那首＝同一首再播一次（使用者回報）。SDK 沒有「把正在播的歌重新掛回清單某一項」的 API。
//
// 做法：清單重建、單擊時都不動，等孤立中的那首剩最後幾秒（核心決定下一首之前），在播放中那份清單找到它，
// 把焦點移到它的下一列，核心就照跟隨游標接下一首。
// - 清單重建後使用者刻意把焦點放到別首（跟隨游標本來的用法：「下一首播這個」）就尊重它；點的是正在播那首不算
// - 歌尾已經處理過，使用者又點回正在播那首：再處理一次
// - 只在依序與重複清單兩種播放順序、而且跟隨游標開著時處理
// 副作用：最後幾秒焦點框與標頭封面（跟著焦點）會先換成下一首。
// 2026-11-14 在 219 比較過五種做法：重建時就移焦點（單擊正在播那首還是會重播）、單擊時移焦點（一點封面就跳成下一首的）、
// 重新開始播放再跳回原時間（每次切回都會斷一下、重觸發換曲事件）、塞進佇列（會跟佇列清單功能互相干擾），這個副作用最小

namespace cui::orphan_playback {

namespace {

// 剩幾秒時動手。核心在解碼讀到檔尾時就決定下一首（無縫播放），輸出緩衝 1 秒，所以留 5 秒
constexpr double boundary_seconds = 5.0;

enum class OrderKind { sequential, repeat_playlist, other };

// 跟 next_track_toast.cpp 的 get_order_kind 同一套認法；隨機、單曲循環本來就不是「下一列」
OrderKind get_order_kind()
{
    const auto api = playlist_manager::get();
    const auto index = api->playback_order_get_active();
    const std::string_view name = api->playback_order_get_name(index);

    if (name.find("Repeat (playlist)") != std::string_view::npos)
        return OrderKind::repeat_playlist;
    if (name == "Default")
        return OrderKind::sequential;
    if (name.find("Repeat") != std::string_view::npos || name.find("Random") != std::string_view::npos
        || name.find("Shuffle") != std::string_view::npos)
        return OrderKind::other;

    switch (index) {
    case 0:
        return OrderKind::sequential;
    case 1:
        return OrderKind::repeat_playlist;
    default:
        return OrderKind::other;
    }
}

// 正在播放、但核心記的那一項已經不在任何清單裡
bool is_orphaned(metadb_handle_ptr& now_playing)
{
    const auto playback = play_control::get();
    if (!playback->is_playing() || !playback->get_now_playing(now_playing))
        return false;
    size_t playlist{};
    size_t item{};
    return !playlist_manager::get()->get_playing_item_location(&playlist, &item);
}

struct State {
    // 清單重建後使用者有沒有刻意把焦點放到別首
    bool focus_choice{};
    // 這一首的歌尾已經處理過
    bool boundary_done{};
    // 自己在移焦點（焦點回呼不要當成使用者的選擇）
    bool adjusting_focus{};
    // 播放中那份清單剛加了歌、重建還沒整理完（這段期間核心動的焦點不算）
    bool rebuild_pending{};
};

State& state()
{
    static State instance;
    return instance;
}

void check_boundary(double position)
{
    auto& st = state();
    if (st.boundary_done)
        return;

    metadb_handle_ptr now_playing;
    if (!is_orphaned(now_playing))
        return;

    const double length = now_playing->get_length();
    if (length <= 0 || length - position > boundary_seconds)
        return;

    st.boundary_done = true;
    if (st.focus_choice)
        return;

    if (!config_object::g_get_data_bool_simple(standard_config_objects::bool_playback_follows_cursor, false))
        return;

    const auto order = get_order_kind();
    if (order == OrderKind::other)
        return;

    const auto api = playlist_manager::get();
    const auto playlist = api->get_playing_playlist();
    if (playlist == pfc_infinite)
        return;

    pfc::list_t<metadb_handle_ptr> items;
    api->playlist_get_all_items(playlist, items);
    const auto item = items.find_item(now_playing);
    if (item == pfc_infinite)
        return;

    const auto count = items.get_count();
    size_t next = item + 1;
    if (next >= count) {
        if (order != OrderKind::repeat_playlist)
            return;
        next = 0;
    }

    st.adjusting_focus = true;
    api->playlist_set_focus_item(playlist, next);
    st.adjusting_focus = false;
}

// 記下使用者在播放中那份清單把焦點放到哪
void on_focus_moved(size_t playlist, size_t item)
{
    auto& st = state();
    const auto api = playlist_manager::get();
    if (playlist != api->get_playing_playlist())
        return;

    metadb_handle_ptr now_playing;
    if (!is_orphaned(now_playing))
        return;

    metadb_handle_ptr handle;
    if (!api->playlist_get_item_handle(handle, playlist, item))
        return;

    // 點的是正在播那首：不是「下一首要播這個」的意思
    st.focus_choice = handle != now_playing;
    if (st.boundary_done && !st.focus_choice) {
        st.boundary_done = false;
        check_boundary(play_control::get()->playback_get_position());
    }
}

class PlaylistCallback : public playlist_callback_impl_base {
public:
    PlaylistCallback() : playlist_callback_impl_base(flag_on_items_added | flag_on_item_focus_change) {}

    void on_items_added(size_t playlist, size_t, metadb_handle_list_cref, const bit_array&) override
    {
        auto& st = state();
        if (playlist != playlist_manager::get()->get_playing_playlist() || st.rebuild_pending)
            return;

        // Album list 是先清空再加入、核心接著動焦點，都在同一段裡；等整段做完才把焦點的狀態歸零
        st.rebuild_pending = true;
        fb2k::inMainThread([] {
            auto& st = state();
            st.rebuild_pending = false;
            st.focus_choice = false;
            st.boundary_done = false;
        });
    }

    void on_item_focus_change(size_t playlist, size_t, size_t to) override
    {
        const auto& st = state();
        if (st.adjusting_focus || st.rebuild_pending || to == pfc_infinite)
            return;
        fb2k::inMainThread([playlist, to] { on_focus_moved(playlist, to); });
    }
};

class PlayCallback : public play_callback_impl_base {
public:
    PlayCallback()
        : play_callback_impl_base(flag_on_playback_new_track | flag_on_playback_time | flag_on_playback_seek)
    {
    }

    void on_playback_new_track(metadb_handle_ptr) override { state().boundary_done = false; }
    void on_playback_time(double time) override { check_boundary(time); }
    void on_playback_seek(double time) override { check_boundary(time); }
};

std::unique_ptr<PlaylistCallback> g_playlist_callback;
std::unique_ptr<PlayCallback> g_play_callback;

class OrphanPlaybackInitQuit : public initquit {
public:
    void on_init() override
    {
        g_playlist_callback = std::make_unique<PlaylistCallback>();
        g_play_callback = std::make_unique<PlayCallback>();
    }
    void on_quit() override
    {
        g_playlist_callback.reset();
        g_play_callback.reset();
    }
};

initquit_factory_t<OrphanPlaybackInitQuit> orphan_playback_initquit;

} // namespace

} // namespace cui::orphan_playback
