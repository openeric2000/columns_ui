#pragma once

// 自編版新增：展開頁右上角的「下一首」提示（2026-11-02）
// - 歌曲最後 N 秒出現，左邊的圈逆時針倒數；停止且佇列還有歌時改顯示「已停止 · 接下來」
// - 下一首的決定順序：佇列第一首 → 隨機／單曲循環不顯示 → 播放跟隨游標 → 播放中那首的下一列（清單循環時最後一首接第一首）
// - 提示是一個浮在主視窗上的分層視窗（不擋滑鼠、不搶焦點），自己用 GDI+ 畫，所以才畫得出倒數圈與淡入淡出
// - 設定（以下五個）存在 Columns UI 設定裡，改了立刻生效

namespace cui::next_track_toast {

inline constexpr uint32_t min_seconds = 3;
inline constexpr uint32_t max_seconds = 30;
// 有些歌結尾會有好幾秒無聲，5 秒太短，預設改 10 秒（2026-11-02）
inline constexpr uint32_t default_seconds = 10;

// 啟用提示（關閉＝完全不出現，包含已停止的狀態）
extern fbh::ConfigBool enabled;
// 歌曲最後幾秒開始倒數
extern fbh::ConfigUint32 seconds;
// 圈內顯示剩餘秒數
extern fbh::ConfigBool show_seconds_in_ring;
// 顯示下一首的專輯封面
extern fbh::ConfigBool show_cover;
// 顯示來源標籤（佇列／清單）
extern fbh::ConfigBool show_source_tag;

} // namespace cui::next_track_toast
