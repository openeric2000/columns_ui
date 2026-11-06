#pragma once

namespace cui::main_menu {

struct MainMenuCommand {
    GUID guid;
    const char* name;
    const char* description;
    std::function<void()> execute_callback;
    std::function<bool()> is_ticked_callback{};
    std::function<bool()> is_radio_selected_callback{};
    bool hide_without_shift_key{};
    std::function<bool()> is_available_callback{};
};

namespace commands {
constexpr GUID show_groups_id{0x8fa126be, 0xda26, 0x43de, {0xa5, 0x25, 0xb6, 0x4d, 0x17, 0x79, 0x3c, 0xf0}};
constexpr GUID show_artwork_id{0x1fba5c30, 0xc6ed, 0x4649, {0x84, 0xa0, 0x8f, 0xf6, 0xab, 0x71, 0x85, 0x97}};
constexpr GUID activate_now_playing_id{0x450c17b0, 0x70e0, 0x42cc, {0xaa, 0x7d, 0xdc, 0x5b, 0x7a, 0xdf, 0xf5, 0xa8}};
constexpr GUID toggle_live_editing_id{0xf2c92eb9, 0xb5f1, 0x4cec, {0xb2, 0x4e, 0xc1, 0x40, 0x83, 0x1f, 0x7b, 0x9}};
// 自編版新增：View/Layout/切換播放頁
constexpr GUID toggle_now_playing_view_id{
    0x95dae386, 0xd80a, 0x4ef7, {0xb5, 0x48, 0xd3, 0x6d, 0x0e, 0x4c, 0xc1, 0x63}};
// 自編版新增：View/Layout/停止或收起播放頁（播放列的停止鈕用）
constexpr GUID stop_or_collapse_id{
    0x4d1f7b36, 0x9ae2, 0x4b58, {0x83, 0xc0, 0x2e, 0x15, 0xf9, 0x6a, 0xd7, 0x04}};
// 自編版新增：View/Layout/顯示封面大圖
constexpr GUID show_now_playing_cover_id{
    0x2cc23246, 0xc90d, 0x4787, {0x84, 0x16, 0xdf, 0x32, 0x6d, 0x69, 0x6d, 0xbf}};
// 自編版新增：View/Layout/展開頁樣式（三種）與切換展開頁樣式
constexpr GUID now_playing_style_centered_id{
    0x8a9efc3a, 0x7b5d, 0x4cff, {0xae, 0x4c, 0x06, 0x46, 0xf2, 0xb6, 0xa2, 0xfb}};
constexpr GUID now_playing_style_spectrum_id{
    0x4eb74dc8, 0xf7c3, 0x4277, {0x8f, 0x4b, 0x73, 0xdc, 0x65, 0xa0, 0x6c, 0x7c}};
constexpr GUID now_playing_style_info_id{
    0x66712042, 0x47c3, 0x4379, {0xae, 0x31, 0xfb, 0x58, 0xd6, 0x68, 0xd0, 0xe3}};
constexpr GUID cycle_now_playing_style_id{
    0x32bc0b81, 0x6f5d, 0x40f0, {0x99, 0x76, 0x1d, 0xbb, 0x7f, 0x5e, 0x13, 0x6b}};
// 自編版新增：View/Layout/下一首提示（開關）
constexpr GUID next_track_toast_toggle_id{
    0xe2b7a914, 0x35c8, 0x4d61, {0x9f, 0x0a, 0x7c, 0x48, 0xd1, 0x2e, 0xb6, 0x93}};
// 自編版新增：View/Layout/主題字型…
constexpr GUID theme_font_id{0x7493810c, 0x2bf4, 0x49a6, {0x8b, 0x8f, 0xa3, 0x7d, 0xae, 0x34, 0x9a, 0x60}};

void on_transparency_changed();
void on_lock_window_size_changed();

} // namespace commands

// 自編版新增：目前的展開頁樣式（1 置中、2 靠左＋VU 表、3 靠左＋資訊卡）
uint32_t get_now_playing_style();

// 自編版新增：暫停時按下暫停（或暫停中跳轉）當下記下的位置（秒）；沒在暫停或沒記到時為空
// 有些輸出方式暫停中回報的位置會前後跳，要顯示暫停中的位置請用這個，不要讀 playback_get_position
std::optional<double> get_paused_position();

// 自編版新增（2026-11-05）：拖曳進度條、滾輪累積、等 fb2k 跳完的這段時間，進度條預覽的位置（秒）；沒有預覽時傳空
// 設定後標題格式欄位 %cui_time_text% 會顯示這個位置（暫停中沒有預覽時顯示暫停位置），底下的時間才會跟著動；
// 只有顯示的整秒變了才通知 ESLyric 重算（拖曳時最多約每 30 ms 一次）。要在主執行緒呼叫
void set_seek_preview(std::optional<double> seconds);

// 自編版新增（2026-11-06）：自訂標題格式欄位（例如 %cui_volume_icon%）的值變了，通知 ESLyric 重算標題格式與 visibility。
// ESLyric 的 visibility 不會隨時重算，要對曲目送一次 dispatch_refresh：正在播放就送播放中的曲目，沒在播放就送播放清單反白的那首。
// 要在主執行緒呼叫
void refresh_layout_fields();

} // namespace cui::main_menu
