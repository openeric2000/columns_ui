#pragma once

// 自編版新增（2026-11-12）：卡片型彈出視窗共用的「什麼時候自動關」規則。
// 從展開頁的版面縮圖卡（now_playing_mode_picker.cpp）與 VU 表／頻譜的樣式卡（now_playing_vu_meter.cpp）抽出來，
// 兩邊原本各寫一份一樣的邏輯。輸出裝置清單（output_chip.cpp）沒有用這個：它用滑鼠捕捉把「點外面」那一下吞掉，
// 才不會穿過去點到底下的播放列，行為跟這裡不同。
//
// 會關的情況（都用 PostMessage(WM_CLOSE)，視窗自己決定怎麼收）：
// - 這個執行緒的滑鼠在卡片外面按下任何一個鍵（點 fb2k 其他地方、標題列都算；卡片沒拿到焦點時也有效）
// - 卡片失去啟用（WA_INACTIVE）
// - 每 0.25 秒檢查：前景換成別的程式，或 still_open 傳回 false（例如展開頁收合、VU 表藏起來）
//
// 用法：建好視窗後呼叫 attach；window_proc 一開頭先交給 handle_message，傳回 true 就直接 return 0
// （WM_ACTIVATE 與這裡的檢查計時器會傳回 true）。
// WM_NCDESTROY 時 handle_message 會收掉 hook 與計時器，但傳回 false，呼叫端照常做自己的清理。

#include <functional>

namespace cui::popup_dismiss {

// outside_close_tick：有給的話，點卡片外面而關掉時寫入當下的 GetTickCount64()（給「剛關掉不重開」用）
void attach(HWND wnd, std::function<bool()> still_open = {}, ULONGLONG* outside_close_tick = nullptr);

bool handle_message(HWND wnd, UINT msg, WPARAM wp, LPARAM lp);

} // namespace cui::popup_dismiss
