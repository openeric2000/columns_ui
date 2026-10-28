#pragma once

namespace cui::dark {

void handle_tab_control_paint(HWND wnd);

// 自編版新增：底線式分頁（只給播放清單分頁用，Tab stack 面板照舊用 handle_tab_control_paint）
// 不畫方塊與邊框；選中的分頁用文字色＋下方強調色底線，未選的用灰字，滑過時灰字變亮
void handle_underline_tab_control_paint(
    HWND wnd, COLORREF background_colour, COLORREF text_colour, COLORREF accent_colour);

} // namespace cui::dark
