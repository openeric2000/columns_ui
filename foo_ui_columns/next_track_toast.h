#pragma once

// 自編版新增：展開頁角落的「下一首」提示（2026-11-02），預設放右下，可在設定頁改成其他三個角落
// - 歌曲最後 N 秒出現，左邊的圈逆時針倒數；停止且佇列還有歌時改顯示「已停止 · 接下來」
// - 下一首的決定順序：佇列第一首 → 隨機／單曲循環不顯示 → 播放跟隨游標 → 播放中那首的下一列（清單循環時最後一首接第一首）
// - 提示是一個浮在主視窗上的分層視窗（不擋滑鼠、不搶焦點），自己用 GDI+ 畫，所以才畫得出倒數圈與淡入淡出
// - 設定（以下六個）存在 Columns UI 設定裡，改了立刻生效；設定頁在 tab_next_track_toast.cpp

namespace cui::next_track_toast {

inline constexpr uint32_t min_seconds = 3;
inline constexpr uint32_t max_seconds = 30;
// 有些歌結尾會有好幾秒無聲，5 秒太短，預設改 10 秒（2026-11-02）
inline constexpr uint32_t default_seconds = 10;

// 提示放在展開頁的哪個角落（存的是這四個值之一，數字不能改，已存進設定）；預設 position_bottom_right
inline constexpr uint32_t position_top_right = 0;
inline constexpr uint32_t position_bottom_right = 1;
inline constexpr uint32_t position_top_left = 2;
inline constexpr uint32_t position_bottom_left = 3;
inline constexpr uint32_t position_count = 4;

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
// 放在哪個角落（position_*）
extern fbh::ConfigUint32 position;

// 卡片的大小（2026-11-05 起可在設定頁選）：存的是選項編號 0～3，對應小 85%、標準 100%、大 120%、特大 140%，預設標準。
// 整張卡片（尺寸、邊距、字級、圈、封面）一起照比例放大縮小
inline constexpr uint32_t size_count = 4;
inline constexpr uint32_t size_default = 1;
inline constexpr int size_percents[size_count] = {85, 100, 120, 140};
extern fbh::ConfigUint32 size_index;

} // namespace cui::next_track_toast
