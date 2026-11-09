#pragma once

// 自編版新增：展開頁大封面右下角的「看大圖」放大鈕（2026-11-09，v1.5 待做清單第 2 個；使用者選方案 1）
// 點封面已經改成「切到資訊卡」，看大圖的入口改成封面右下角一顆小放大鈕。ESLyric 的 albumart 一定畫在同一塊所有元素上面，
// 放在 main.xml 裡的按鈕會被封面蓋住（點得到、看不到），所以跟音量圖示一樣由 Columns UI 開一個分層子視窗蓋上去：
// - 外觀照原本 main.xml 試做的那顆：44 DIP 半透明深色圓底＋Segoe MDL2「全螢幕」圖示，離封面右緣、下緣各 14 DIP；滑過變亮
// - 點一下開 foobar2000 picture viewer（跟主選單「顯示封面大圖」相同）
// - 封面位置由 main.xml 裡的一行標記註解告訴這裡（Columns UI 不知道 ESLyric 把封面排在哪），例如
//     <!-- nocturne-cover-zoom: left=472 cover=600 gap=24 info=115 -->
//   left＝展開頁左緣到封面左緣、cover＝封面邊長、gap＝封面下緣到歌名、info＝封面下面那塊資訊在 ESLyric 排版時佔的高度；
//   數字是 DIP，加 % 是展開頁寬度的百分比。封面＋gap＋info 這一整組在展開頁裡垂直置中（main.xml 的 disc 容器 justify=middle）。
//   info 是實測反推的（2026-11-09：ESLyric 排版時這塊的高度跟實際顯示幾行無關，兩行、四行時封面上緣位置相同）。
//   沒有這行標記的版面（例如公開版、169 舊版面）不顯示按鈕
// - 只在展開頁完全展開（動畫、截圖都結束）而且是封面模式（不是資訊卡）時顯示
// - 視窗由 FlatSplitterPanel 在有播放列時建立，排版時跟進度條、音量圖示同一批放到最上層

namespace cui::cover_zoom {

// 在 parent（分割面板視窗）底下建立按鈕視窗（先不顯示）；失敗傳回 nullptr
HWND create(HWND parent);

// 依 bar_rect（播放列在分割面板裡的位置；展開頁＝它上面那一段）排位置；不該顯示時藏起來
void layout(HWND wnd, const RECT& bar_rect);

// 同 layout，但加進呼叫端的 DeferWindowPos 批次（要放在把播放列容器放到最上層的那一筆「之後」）
HDWP defer_layout(HDWP dwp, HWND wnd, const RECT& bar_rect);

// 用上一次的 bar_rect 重排所有按鈕（展開動畫結束、切換展開頁樣式、GDI+ 字型暖機做完時用）
void refresh_layouts();

} // namespace cui::cover_zoom
