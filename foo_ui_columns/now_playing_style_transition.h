#pragma once

// 自編版新增：展開頁封面⇄資訊卡切換時的過渡動畫（2026-11-09，v1.5 待做清單第 3 個）
// 「淡出＋微縮放」：舊的一面縮到 96% 淡出、新的一面從 104% 縮回 100% 淡入，各約 0.29 秒。
// 2026-11-12 使用者決定先不做 3D 翻牌／翻書，只把這個弄順：改用 DirectComposition，縮放與透明度交給 DWM 照螢幕更新跑
// （原本每格用 GDI+ 合成，219 實測一格 16～33 ms、整段只有約 14 格）。
// ESLyric 沒有透明度動畫，所以由 Columns UI 做：
// 1. 切換前截下封面方塊（含外圍一圈）的畫面，上傳成 DirectComposition 的內容，用一個不擋滑鼠的視窗蓋在原位
// 2. 底下真的切換樣式；舊的一面馬上開始縮小淡出（DWM 跑，主執行緒被 ESLyric 或截圖佔住也不停）
// 3. 每隔一下再截一次，跟切換前不一樣而且畫面穩定了（ESLyric 畫好了）就當作「切換後」，上傳後新的一面從這時開始放大淡入
// 4. 動畫結束後拿掉蓋層，露出真的畫面
// 封面位置沿用放大鈕讀的 main.xml 標記（now_playing_cover_zoom.h）；展開頁沒有完全展開、找不到封面位置時直接切換、不做動畫
// 換歌時整個展開頁也交叉淡化（2026-11-14）：換歌前截圖蓋上，換歌後等 ESLyric 畫好再淡出，播放回呼在 .cpp 裡自己登記

#include <functional>

namespace cui::style_transition {

// 套用 apply（真正切換樣式的動作）；可以的話包一層過渡動畫。要在主執行緒呼叫
void run(std::function<void()> apply);

// 切換展開頁顯示內容（歌詞／純封面／VU 表，2026-11-11）：先截圖蓋住，等 ESLyric 畫好後整個蓋層淡出約 0.29 秒、
// 透出底下真的新畫面（淡出也由 DWM 跑）；展開頁沒有完全展開時直接切換
void run_stage(std::function<void()> apply);

} // namespace cui::style_transition
