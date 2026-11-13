#pragma once

namespace cui::playlist_item_helpers {

// 自編版新增（2026-11-03）：播放播放清單裡的某一首，但不清掉播放佇列。
// 核心的預設動作（雙擊、Enter、右鍵 Play）會把佇列清空，SDK 沒有「不清」的選項，所以先記下佇列、呼叫核心、再補回；
// 佇列會短暫變空一下。設定頁在 tab_next_track_toast.cpp，預設開
extern fbh::ConfigBool keep_queue_on_play;

// 自編版新增（2026-11-12）：同一首可不可以重複加入佇列，預設不行。
// 管得到的是標頭「加入佇列」按鈕（讀 %cui_queue_allow_duplicates%，見 menu_items.cpp）與播放清單的中鍵「加入佇列」；
// fb2k 自己的右鍵選單管不到，重複時佇列號碼欄位顯示「1+」
extern fbh::ConfigBool allow_duplicate_queue;

void play_item_keeping_queue(size_t playlist, size_t item);
void play_active_item_keeping_queue(size_t item);

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
