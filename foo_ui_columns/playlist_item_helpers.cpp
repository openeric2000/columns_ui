#include "pch.h"
#include "playlist_item_helpers.h"

namespace cui::playlist_item_helpers {

fbh::ConfigBool keep_queue_on_play(
    GUID{0x4b2e8a17, 0x6d03, 0x4e95, {0xa8, 0x1c, 0x72, 0xf0, 0x3d, 0x95, 0xbe, 0x26}}, true);

fbh::ConfigBool allow_duplicate_queue(
    GUID{0x34bd1e22, 0x8b0e, 0x433b, {0x9e, 0xc5, 0x79, 0x05, 0x89, 0x99, 0x22, 0x9b}}, false);

namespace {

template <typename RunDefaultAction>
void run_keeping_queue(RunDefaultAction&& run_default_action)
{
    const auto api = playlist_manager::get();

    if (!keep_queue_on_play.get() || api->queue_get_count() == 0) {
        run_default_action();
        return;
    }

    pfc::list_t<t_playback_queue_item> saved;
    api->queue_get_contents(saved);

    run_default_action();

    // 核心把佇列清空了才補回；佇列沒變空就不動，免得加成兩份
    if (api->queue_get_count() != 0)
        return;

    for (size_t index = 0; index < saved.get_count(); index++) {
        const auto& entry = saved[index];

        if (entry.m_playlist != pfc_infinite)
            api->queue_add_item_playlist(entry.m_playlist, entry.m_item);
        else
            api->queue_add_item(entry.m_handle);
    }
}

} // namespace

void play_item_keeping_queue(size_t playlist, size_t item)
{
    run_keeping_queue([&] { playlist_manager::get()->playlist_execute_default_action(playlist, item); });
}

void play_active_item_keeping_queue(size_t item)
{
    run_keeping_queue([&] { playlist_manager::get()->activeplaylist_execute_default_action(item); });
}

void action_remove_track(bool on_item, size_t idx)
{
    if (on_item) {
        const auto api = playlist_manager::get();
        api->activeplaylist_undo_backup();
        api->activeplaylist_remove_items(bit_array_one(idx));
    }
}

void action_add_to_queue(bool on_item, size_t idx)
{
    if (on_item) {
        const auto api = playlist_manager::get();
        const auto active = api->get_active_playlist();
        if (active == -1)
            return;

        // 沒允許重複時，同一首已經在佇列裡（不管從哪份清單加的）就不再加（2026-11-12）
        if (!allow_duplicate_queue.get()) {
            metadb_handle_ptr handle;
            pfc::list_t<t_playback_queue_item> queue;
            api->queue_get_contents(queue);
            if (api->playlist_get_item_handle(handle, active, idx)) {
                for (size_t index = 0; index < queue.get_count(); index++) {
                    if (queue[index].m_handle == handle)
                        return;
                }
            }
        }

        api->queue_add_item_playlist(active, idx);
    }
}

void action_none(bool on_on_item, size_t idx) {}

MiddleLickAction MiddleClickActionManager::g_pma_actions[] = {
    {"Autoscroll", 0, action_none},
    {"Remove track from playlist", 1, action_remove_track},
    {"Add track to playback queue", 2, action_add_to_queue},
};

size_t MiddleClickActionManager::get_count()
{
    return std::size(g_pma_actions);
}

size_t MiddleClickActionManager::id_to_idx(unsigned id)
{
    constexpr auto count = std::size(g_pma_actions);
    for (size_t n = 0; n < count; n++) {
        if (g_pma_actions[n].id == id)
            return n;
    }
    return 0;
}
} // namespace cui::playlist_item_helpers
