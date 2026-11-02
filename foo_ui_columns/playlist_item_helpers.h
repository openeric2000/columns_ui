#pragma once

namespace cui::playlist_item_helpers {

// 自編版新增（2026-11-03）：播放播放清單裡的某一首，但不清掉播放佇列。
// 核心的預設動作（雙擊、Enter、右鍵 Play）會把佇列清空，SDK 沒有「不清」的選項，所以先記下佇列、呼叫核心、再補回；
// 佇列會短暫變空一下。設定頁在 tab_next_track_toast.cpp，預設開
extern fbh::ConfigBool keep_queue_on_play;

void play_item_keeping_queue(size_t playlist, size_t item);
void play_active_item_keeping_queue(size_t item);

// 補回佇列的過程中（包含核心清佇列的通知）為 true，別的元件用來略過這一瞬間的空佇列
bool is_restoring_queue();

using MiddleClickFunction = void (*)(bool, size_t);

struct MiddleLickAction {
    const char* name;
    unsigned id;
    MiddleClickFunction p_run;
};

class MiddleClickActionManager {
public:
    static MiddleLickAction g_pma_actions[];
    static size_t id_to_idx(unsigned id);

    static bool run(unsigned id, bool on_item, size_t idx)
    {
        g_pma_actions[id_to_idx(id)].p_run(on_item, idx);
        return true;
    }
    static size_t get_count();
};
} // namespace cui::playlist_item_helpers
