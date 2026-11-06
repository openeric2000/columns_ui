#pragma once

// 自編版新增：播放列的「音量圖示」（2026-11-06，使用者要求）
// 音量條平常收成一個喇叭圖示，滑鼠移上去才在圖示正上方往上彈出一個小滑桿（跟輸出裝置的滑過面板、裝置清單同一套做法）。
// ESLyric 的版面檔碰不到「滑過才顯示」，也彈不出播放列的範圍，所以跟輸出裝置顯示一樣由 Columns UI 自己畫：
// - 圖示視窗：分割面板的分層子視窗，蓋在 main.xml 留給靜音鈕的位置上（48×48 DIP），自己畫喇叭圖示、滑過的反白框與靜音色；
//   點一下＝靜音切換，滾輪＝調音量
// - 彈出滑桿：頂層分層視窗（不搶焦點），滑過約 0.15 秒彈出、離開約 0.3 秒收起；拖曳、點軌道、滾輪都能調；
//   調整時圓點左邊短暫冒出 dB 數字（樣式 2，使用者 2026-11-06 看預覽選的：小膠囊、不常駐 dB）
// - 音量刻度與 Columns UI 內建的音量條相同：位置＝2^(dB/10)（−27 dB 約在 15%）
// - 設定頁（Preferences → Tools → Nocturne →「播放」）有開關，預設關：開了以後 main.xml 要拿掉 <volume> 與 dB 標籤、
//   把靜音鈕換成同寬的空白，輸出裝置顯示才會把右邊多出來的空間用掉（見 time_col_right_dip）。沒開時這個視窗整個藏起來，
//   各環境的行為跟以前一樣
// - 視窗由 FlatSplitterPanel 在有播放列時建立，排版時放到播放列上並放最上層（跟輸出裝置顯示同一批）

namespace cui::volume_chip {

// 設定頁：音量條收成圖示（預設關）。一寫入（勾選、Reset page 都算）就重排圖示與輸出裝置顯示
extern fbh::ConfigBool enabled;

// 播放列右邊那串（停止、靜音、音量條、dB）的寬度＝時間欄右緣離播放列右緣幾 DIP；輸出裝置顯示照它排位置。
// 沒開：433（main.xml 原本的音量條 140＋dB 148 都在）；開了：144（只剩停止、靜音與右留白）
int time_col_right_dip();

// 重排所有圖示視窗（設定改了、GDI+ 字型暖機做完時用）
void refresh_layouts();

// 在 parent（分割面板視窗）底下建立圖示視窗（先不顯示）；失敗傳回 nullptr
HWND create(HWND parent);

// 依 bar_rect（播放列在分割面板裡的位置）排位置；沒開或放不下就藏起來
void layout(HWND wnd, const RECT& bar_rect);

// 同 layout，但加進呼叫端的 DeferWindowPos 批次（要放在把播放列容器放到最上層的那一筆「之後」）
HDWP defer_layout(HDWP dwp, HWND wnd, const RECT& bar_rect);

} // namespace cui::volume_chip
