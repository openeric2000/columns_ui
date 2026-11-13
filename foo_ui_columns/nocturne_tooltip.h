#pragma once

// 自編版新增（2026-11-12）：自己畫的滑鼠提示（外觀見 nocturne_surface.h）。
// ESLyric 的原生提示在 nocturne_tooltip.cpp 裡自動換掉；網頁面板（專輯標頭）的提示由網頁用 Console 暗號
// 「[Nocturne] tip show …／tip hide」叫這裡畫（見 tab_next_track_toast.cpp 的 WebKeyFromConsole），
// 這樣提示可以畫在網頁面板外面（按鈕正下方），跟 ESLyric 的提示是同一個視窗、同一個樣子

namespace cui::nocturne_tooltip {

// anchor：要提示的元件在螢幕上的範圍（像素）。提示放在它正下方、水平置中；游標離開 anchor 就自己收
// 只能在主執行緒呼叫
void show_below(const RECT& anchor, const std::wstring& text, UINT dpi);
void hide_below();

} // namespace cui::nocturne_tooltip
