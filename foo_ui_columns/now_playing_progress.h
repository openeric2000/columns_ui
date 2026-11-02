#pragma once

// 自編版新增：播放列最上緣的進度條（2026-11-02）
// ESLyric 的進度條平常播放時每約 0.3 秒才更新一次（紅線每次跳 2 px），暫停→繼續時又會跟著 fb2k 回報的位置往前跳，
// 佈局碰不到這個更新頻率，所以由 Columns UI 自己畫一條蓋在 ESLyric 上面：
// - 外觀與 main.xml 的 seekbar 相同：高 12 DIP、軌道 3 DIP（灰 61／已播放紅 247,11,11）、左右各縮進圓點半徑 6 DIP，
//   滑過去才出現紅色圓點
// - 播放中約 60 Hz 更新；位置用跟下一首提示相同的濾波（吸收暫停→繼續時往前跳的小誤差）
// - 點軌道立刻跳轉；按住拖曳時圓點與紅線跟著滑鼠，放開才真正跳轉（邊拖邊跳在 ASIO+DSD 輸出可能有雜音）
// - 停止、沒有歌、曲長未知時只畫灰軌道，點了沒反應
// - 視窗是分割面板的子視窗，由 FlatSplitterPanel 在有播放列時建立、排版時移到播放列上緣並放最上層

namespace cui::now_playing_progress {

// 在 parent（分割面板視窗）底下建立進度條視窗（先不顯示）；失敗傳回 nullptr
HWND create(HWND parent);

// 移到 bar_rect（播放列在分割面板裡的位置）的上緣，寬度同播放列，並放到子視窗的最上層
// 位置、大小都沒變而且已經在最上層時什麼都不做
void layout(HWND wnd, const RECT& bar_rect);

// 同 layout，但加進呼叫端的 DeferWindowPos 批次（要放在把播放列容器放到最上層的那一筆「之後」）：
// 容器被放到最上層的同一個動作裡，進度條也回到它上面，中間不會有一格露出底下 ESLyric 的條
// （2026-11-02 錄影：先放容器、再另外放進度條，展開／收合 6 次有 3～4 次整條閃 1 幀）
HDWP defer_layout(HDWP dwp, HWND wnd, const RECT& bar_rect);

} // namespace cui::now_playing_progress
