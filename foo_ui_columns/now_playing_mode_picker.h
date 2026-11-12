#pragma once

// 自編版新增（2026-11-12）：展開頁左上角按鈕設成「點一下跳出選單」時，選單改成版面縮圖卡（使用者看過預覽頁選 B）。
// 原本是 Windows 內建選單（方角、系統配色、打勾），跟展開頁其他自繪的東西（VU 挑選卡、模式小膠囊、下一首提示）不搭。
// - 每種模式一張版面縮圖（封面用正在播的這首、頻譜顏色取封面色），目前的模式紅框；每排最多三張
// - 卡片是霧面玻璃：開的時候截下展開頁那一塊、模糊後壓暗當底；截不到時用實心深色
// - 貼在左上角按鈕正下方展開（約 0.16 秒淡入＋往下滑 6 DIP）；不在展開頁時（例如從主選單叫出來）在游標位置開
// - 點縮圖切換並關閉；點外面、切到別的程式、展開頁收合、Esc 都會關；方向鍵移動、Enter／空白鍵選
// - 這個版面不支援的模式縮圖壓暗、寫「版面不支援」，點了沒反應
// - 卡片開著時再點一次左上角按鈕＝關掉（不會關了又馬上開）

#include "menu_items.h"

namespace cui::mode_picker {

struct Item {
    main_menu::NowPlayingMode mode;
    const wchar_t* name;
    bool available;
};

// 開卡片（主執行緒）；傳回 false 表示開不起來（GDI+ 字型還沒暖機好等），呼叫端改用系統選單。
// 卡片已經開著、或剛被「點外面」關掉時，這次當成「關掉」並傳回 true
bool open(std::vector<Item> items, main_menu::NowPlayingMode current,
    std::function<void(main_menu::NowPlayingMode)> on_choose);

void close();

} // namespace cui::mode_picker
