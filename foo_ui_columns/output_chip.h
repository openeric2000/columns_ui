#pragma once

// 自編版新增：播放列的「目前輸出裝置」顯示（2026-11-05）
// 位置：播放列、時間欄左邊的空白（循環鍵與時間之間）。ESLyric 的標籤碰不到「滑過往上展開面板」，
// 所以跟自繪進度條一樣，由 Columns UI 自己畫一個蓋在 ESLyric 上面的小視窗：
// - 外面顯示「類型標記＋裝置名」：ASIO、獨占用琥珀色，預設、靜音用灰色；
//   裝置名拿掉「Default : 」前綴、結尾的 [exclusive] 與括號裡的廠牌（例如「螢幕喇叭 (AMD High Definition Audio Device)」→「螢幕喇叭」）
// - 滑過去（約 0.35 秒）在正上方浮出圓角面板：輸出類型、完整裝置名（同樣拿掉括號）、「點一下可切換裝置」
// - 點下去往上彈出裝置清單（目前裝置打勾），選了就切換
// - 輸出設定一變（Preferences、Playback→Device、命令列都算）就即時更新
// - 位置照 main.xml 的 side-col（寬 700）推算；播放列太窄放不下就整個藏起來
// - 視窗是分割面板的子視窗，由 FlatSplitterPanel 在有播放列時建立、排版時放到播放列上並放最上層

namespace cui::output_chip {

// 在 parent（分割面板視窗）底下建立裝置顯示視窗（先不顯示）；失敗傳回 nullptr
HWND create(HWND parent);

// 依 bar_rect（播放列在分割面板裡的位置）排位置；放不下就藏起來。位置沒變而且已在最上層時什麼都不做
void layout(HWND wnd, const RECT& bar_rect);

// 同 layout，但加進呼叫端的 DeferWindowPos 批次（要放在把播放列容器放到最上層的那一筆「之後」）
HDWP defer_layout(HDWP dwp, HWND wnd, const RECT& bar_rect);

} // namespace cui::output_chip
