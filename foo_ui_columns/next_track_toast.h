#pragma once

#include <functional>

// 自編版新增：展開頁角落的「下一首」提示（2026-11-02），預設放右下，可在設定頁改成其他三個角落
// - 歌曲最後 N 秒出現，左邊的圈逆時針倒數；停止中不出現（停止一律收起展開頁，2026-11-12）
// - 下一首的決定順序：佇列第一首 → 隨機／單曲循環不顯示 → 播放跟隨游標 → 播放中那首的下一列（清單循環時最後一首接第一首）
// - 提示是一個浮在主視窗上的分層視窗（不擋滑鼠、不搶焦點），自己用 GDI+ 畫，所以才畫得出倒數圈與淡入淡出
// - 設定（以下六個）存在 Columns UI 設定裡，改了立刻生效；設定頁在 tab_next_track_toast.cpp

namespace cui::next_track_toast {

// 預測播放完 now_playing 之後會接哪一首（佇列第一首 → 播放跟隨游標 → 下一列；隨機、單曲循環猜不準就傳空）。
// 給「提前準備」用：展開頁封面虛化背景（expanded_cover_bg.cpp）趁現在先替下一首做好背景圖，換歌時就不用在主執行緒上等。
// 只能在主執行緒呼叫
metadb_handle_ptr predict_next_track(const metadb_handle_ptr& now_playing);

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

// 啟用提示（關閉＝完全不出現）
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

// GDI+ 字型表的暖機（啟動時在背景執行緒做，見 next_track_toast.cpp 的 font_warm_up）有沒有做完。
// 進程裡第一次依名稱建立 GDI+ 字型家族，GDI+ 要列舉整台電腦的字型（這台 755 個字族，219 要 0.3～1.4 秒），
// 在主執行緒上做會把整個介面卡住，連帶網頁面板、專輯樹都晚出現（2026-11-05 啟動分析）。
// 會在主執行緒用名稱建字型的地方（輸出裝置顯示）要等這個做完再排版。暖機沒跑成（取不到字型名稱、GDI+ 啟動失敗）也算做完，不會永遠等
bool gdiplus_fonts_ready();
// 暖機做完時在主執行緒呼叫 callback（已經做完就馬上呼叫）。只能在主執行緒呼叫
void when_gdiplus_fonts_ready(std::function<void()> callback);

} // namespace cui::next_track_toast
