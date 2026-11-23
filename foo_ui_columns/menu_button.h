#pragma once

// 自編版新增（2026-11-14）：側欄頂排（☰／全部收合，menu_button.cpp）底部的細線。
// 資料夾樹（自編版 Album list）可以捲動時，頂排底部畫 1 px、比底色亮 12 的細線（跟側欄右緣那條同色），把頂排跟樹隔開；
// 樹不用捲（內容不到一頁）時不畫。只在深色模式畫（側欄右緣的細線也只在深色模式）。
// Album list 在「可不可以捲動」變了的時候送 album_list_scrollable_message 給主視窗，主視窗呼叫 refresh_dividers()；
// 頂排畫的時候自己讀樹的捲動範圍，所以就算漏了通知，下一次重畫也會對

namespace cui::panels::menu_button {

// 自編版 Album list 送的視窗訊息名稱（兩邊要一致，Album list 那邊在 main_thin_scrollbar.cpp）
inline constexpr wchar_t album_list_scrollable_message[] = L"Nocturne.AlbumListScrollableChanged";

// 所有頂排面板重畫（重新判斷要不要畫底部細線）
void refresh_dividers();

} // namespace cui::panels::menu_button
