#pragma once

// 自編版新增：細捲軸開關（2026-11-08，使用者要求「設定頁可以開關」）
// 覆蓋在清單右邊緣的細捲軸本身做在清單程式庫裡（ui_helpers\list_view，見 list_view_scroll.cpp 下半部），
// 這裡只管「開或關」：
// - 開關存在 fb2k 共用設定（fb2k::configStore）的 nocturne.thinScrollbar，而不是 Columns UI 自己的設定檔——
//   因為左邊資料夾樹是另一個元件（自編版 Album list），兩邊要讀同一個開關，也要在設定頁改了之後當場一起切換
//   （configStore 有變更通知，不用重開 foobar2000）
// - 沒有這個條目就是開（預設值個人版與公開版都一樣）；設定頁的「Reset page」會還原成開

namespace cui::thin_scrollbar {

inline constexpr char config_key[] = "nocturne.thinScrollbar";
inline constexpr bool enabled_default = true;

// 資料夾樹（自編版 Album list）的細捲軸「平常隱藏、滑鼠移進軌道才出現」，直、橫各一個開關（2026-11-09 使用者要求）。
// 只有資料夾樹會讀；鍵名要跟 Album list 的 main_thin_scrollbar.cpp 一致。播放清單的細捲軸不受影響
inline constexpr char autohide_vertical_key[] = "nocturne.thinScrollbar.autoHideVertical";
inline constexpr char autohide_horizontal_key[] = "nocturne.thinScrollbar.autoHideHorizontal";
inline constexpr bool autohide_default = true;

bool is_enabled();

bool is_autohide(bool vertical);
void set_autohide(bool vertical, bool value);

// 設定頁勾選、Reset page 用：寫入設定並馬上套用到所有清單
void set_enabled(bool enabled);

// 把目前的設定值套用到清單程式庫（也會在第一次呼叫時登記設定變更通知，之後別的程式改了開關也會跟著切換）。
// 每次建立清單視窗前呼叫，不用擔心呼叫太多次
void sync_to_list_views();

} // namespace cui::thin_scrollbar
