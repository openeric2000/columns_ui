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
// 自編版新增：View/Layout/展開頁樣式（置中、靠左＋資訊卡）與切換展開頁樣式
// （原本還有第三種「靠左＋VU 表」，2026-11-07 拿掉；它的選單項目 GUID 是 4eb74dc8-f7c3-4277-8f4b-73dc65a06c7c，以後要復原可以沿用）
constexpr GUID now_playing_style_centered_id{
    0x8a9efc3a, 0x7b5d, 0x4cff, {0xae, 0x4c, 0x06, 0x46, 0xf2, 0xb6, 0xa2, 0xfb}};
constexpr GUID now_playing_style_info_id{
    0x66712042, 0x47c3, 0x4379, {0xae, 0x31, 0xfb, 0x58, 0xd6, 0x68, 0xd0, 0xe3}};
constexpr GUID cycle_now_playing_style_id{
    0x32bc0b81, 0x6f5d, 0x40f0, {0x99, 0x76, 0x1d, 0xbb, 0x7f, 0x5e, 0x13, 0x6b}};
// 自編版新增（2026-11-11）：View/Layout/展開頁顯示內容（歌詞、純封面、VU 表）與左上角按鈕用的「切換展開頁顯示內容」
constexpr GUID now_playing_mode_lyrics_id{
    0xb4ae76c9, 0xc724, 0x478c, {0xac, 0x37, 0xb6, 0x48, 0xae, 0xe3, 0xf5, 0xf3}};
constexpr GUID now_playing_mode_cover_id{
    0xcf0b57e1, 0x32df, 0x44f8, {0xaa, 0xc0, 0x73, 0x97, 0xc3, 0x4c, 0x05, 0x38}};
constexpr GUID now_playing_mode_vu_id{
    0xd0539e89, 0xa2ee, 0x4314, {0xaa, 0x53, 0x55, 0x42, 0xaf, 0xe9, 0x90, 0xc7}};
// 2026-11-12：展開頁：頻譜
constexpr GUID now_playing_mode_spectrum_id{
    0x3a983b1b, 0x4f95, 0x4718, {0x89, 0xf0, 0x0e, 0x88, 0xb6, 0x7c, 0xc9, 0x74}};
// 2026-11-12：展開頁：歌詞＋VU 表、歌詞＋頻譜
constexpr GUID now_playing_mode_lyrics_vu_id{
    0x2f29d27e, 0x459e, 0x4d2f, {0x96, 0x4e, 0xb6, 0x2f, 0x62, 0x74, 0x71, 0x0e}};
constexpr GUID now_playing_mode_lyrics_spectrum_id{
    0xa7f6ef88, 0xe197, 0x4a61, {0xb6, 0xf8, 0xc1, 0x38, 0xfc, 0xb7, 0xde, 0x36}};
constexpr GUID now_playing_mode_button_id{
    0xeb119c41, 0x706f, 0x4e51, {0x87, 0x5d, 0xeb, 0xc2, 0x5a, 0x2f, 0x91, 0x8d}};
// 自編版新增：View/Layout/下一首提示（開關）
constexpr GUID next_track_toast_toggle_id{
    0xe2b7a914, 0x35c8, 0x4d61, {0x9f, 0x0a, 0x7c, 0x48, 0xd1, 0x2e, 0xb6, 0x93}};
// 自編版新增：View/Layout/主題字型…
constexpr GUID theme_font_id{0x7493810c, 0x2bf4, 0x49a6, {0x8b, 0x8f, 0xa3, 0x7d, 0xae, 0x34, 0x9a, 0x60}};
// 自編版新增（2026-11-22）：View/Layout/切換剩餘時間（播放列點時間文字用）
constexpr GUID toggle_remaining_time_id{
    0x5c8e2f47, 0x1a3b, 0x4d96, {0x8e, 0x71, 0x2b, 0xc4, 0x9d, 0x05, 0x6f, 0xa8}};

void on_transparency_changed();
void on_lock_window_size_changed();

} // namespace commands

// 自編版新增：暫停時按下暫停（或暫停中跳轉）當下記下的位置（秒）；沒在暫停或沒記到時為空
// 有些輸出方式暫停中回報的位置會前後跳，要顯示暫停中的位置請用這個，不要讀 playback_get_position
std::optional<double> get_paused_position();

// 自編版新增（2026-11-05）：拖曳進度條、滾輪累積、等 fb2k 跳完的這段時間，進度條預覽的位置（秒）；沒有預覽時傳空
// 設定後標題格式欄位 %cui_time_text% 會顯示這個位置（暫停中沒有預覽時顯示暫停位置），底下的時間才會跟著動；
// 只有顯示的整秒變了才通知 ESLyric 重算（拖曳時最多約每 30 ms 一次）。要在主執行緒呼叫
void set_seek_preview(std::optional<double> seconds);

// 自編版新增（2026-11-22）：播放列時間的後半段顯示剩餘時間（「0:05 / -1:51」，跟 Spotify 一樣）還是曲長。
// 點播放列的時間文字（mm://View/Layout/切換剩餘時間）或設定頁「播放」切換，存在設定裡，重開 fb2k 照舊。
// 一寫入就通知 ESLyric 重算（版面讀 %cui_time_remaining%、%cui_time_left%）
extern fbh::ConfigBool show_remaining_time;

// 自編版新增（2026-11-06）：自訂標題格式欄位（例如 %cui_volume_icon%）的值變了，通知 ESLyric 重算標題格式與 visibility。
// ESLyric 的 visibility 不會隨時重算，要對曲目送一次 dispatch_refresh：正在播放就送播放中的曲目，沒在播放就送播放清單反白的那首。
// 要在主執行緒呼叫
void refresh_layout_fields();

// 自編版新增（2026-11-09）：目前生效的展開頁樣式（1 封面、3 資訊卡；已拿掉的 2 當成 1），給展開頁放大鈕判斷要不要顯示
uint32_t get_now_playing_style();

// 自編版新增（2026-11-09）：展開頁收合完成時呼叫：是資訊卡就直接切回封面（不做切換動畫，這時展開頁已經看不到），
// 下次打開展開頁一定是封面
void reset_now_playing_style_to_cover();

// 自編版新增（2026-11-11，v1.5 待做清單第 4 個）：展開頁要顯示什麼，給 ESLyric 佈局的 %cui_now_playing_mode% 與 Columns UI 自己用
// 1 歌詞（預設）、2 純封面、3 VU 表、4 頻譜、5 歌詞＋VU 表、6 歌詞＋頻譜（4～6 是 2026-11-12 加的）。
// 3～6 左邊一樣有封面；它們只有 main.xml 標記有歌詞欄位置時才算數，沒有就當成 1。
// 數字是存在設定裡的值，不能改；畫面上的順序見 now_playing_mode_order
enum class NowPlayingMode : uint32_t { lyrics = 1, cover = 2, vu = 3, spectrum = 4, lyrics_vu = 5, lyrics_spectrum = 6 };
inline constexpr uint32_t now_playing_mode_count = 6;
// 設定頁勾選、左上角按鈕的輪流與選單都照這個順序（2026-11-12 使用者指定：純封面、封面＋歌詞、VU、頻譜、歌詞＋VU、歌詞＋頻譜）
inline constexpr NowPlayingMode now_playing_mode_order[now_playing_mode_count] = {NowPlayingMode::cover,
    NowPlayingMode::lyrics, NowPlayingMode::vu, NowPlayingMode::spectrum, NowPlayingMode::lyrics_vu,
    NowPlayingMode::lyrics_spectrum};

// 模式在 nocturne_text::mode_short_names 裡的位置；版面不支援時的灰色字在下一格（3～6 才有）
inline constexpr int mode_name_index(NowPlayingMode mode, bool available = true)
{
    switch (mode) {
    case NowPlayingMode::cover:
        return 1;
    case NowPlayingMode::vu:
        return available ? 2 : 3;
    case NowPlayingMode::spectrum:
        return available ? 4 : 5;
    case NowPlayingMode::lyrics_vu:
        return available ? 6 : 7;
    case NowPlayingMode::lyrics_spectrum:
        return available ? 8 : 9;
    default:
        return 0;
    }
}

// 要放到歌詞欄位置、要 main.xml 標記的模式
inline constexpr bool mode_needs_lyrics_column(NowPlayingMode mode)
{
    return static_cast<uint32_t>(mode) >= 3 && static_cast<uint32_t>(mode) <= now_playing_mode_count;
}

// 這個模式畫 VU 表／頻譜嗎；歌詞要不要跟著顯示（歌詞＋VU、歌詞＋頻譜時歌詞在歌詞欄上半）
inline constexpr bool mode_shows_vu(NowPlayingMode mode)
{
    return mode == NowPlayingMode::vu || mode == NowPlayingMode::lyrics_vu;
}
inline constexpr bool mode_shows_spectrum(NowPlayingMode mode)
{
    return mode == NowPlayingMode::spectrum || mode == NowPlayingMode::lyrics_spectrum;
}
inline constexpr bool mode_shows_lyrics(NowPlayingMode mode)
{
    return mode == NowPlayingMode::lyrics || mode == NowPlayingMode::lyrics_vu || mode == NowPlayingMode::lyrics_spectrum;
}
NowPlayingMode get_now_playing_mode();

// 展開頁要有哪幾種（2026-11-11 使用者要求：設定頁用下拉鈕勾選）：bit0 歌詞、bit1 純封面、bit2 VU 表、bit3 頻譜、
// bit4 歌詞＋VU 表、bit5 歌詞＋頻譜，預設全勾。
// 左上角按鈕的「換下一種」與「跳出選單」都只出現勾選的；至少留一種（設定頁擋掉全部取消）。存在 fb2k 共用設定
namespace now_playing_modes {
inline constexpr char config_key[] = "nocturne.nowPlaying.modes";
inline constexpr uint32_t all_modes = 63;
// 2026-11-12 加頻譜與兩種「歌詞＋」之前存的勾選沒有 bit3～5：第一次讀到時補勾（新功能預設出現），之後照使用者的勾選
inline constexpr char spectrum_added_key[] = "nocturne.nowPlaying.modesSpectrumAdded";

inline uint32_t get_mask()
{
    const auto store = fb2k::configStore::get();
    auto mask = static_cast<uint32_t>(store->getConfigInt(config_key, all_modes)) & all_modes;
    if (!store->getConfigBool(spectrum_added_key, false)) {
        mask |= 8u | 16u | 32u;
        store->setConfigInt(config_key, mask);
        store->setConfigBool(spectrum_added_key, true);
    }
    return mask ? mask : 1u;
}

inline void set_mask(uint32_t mask)
{
    fb2k::configStore::get()->setConfigInt(config_key, mask & all_modes ? mask & all_modes : 1u);
}

inline constexpr uint32_t bit_of(NowPlayingMode mode)
{
    return 1u << (static_cast<uint32_t>(mode) - 1);
}
} // namespace now_playing_modes

// 勾選改了之後呼叫（主執行緒）：目前的模式被取消勾選就換成第一個還勾著的，並重排展開頁
void on_now_playing_modes_changed();

// 版面裡有沒有 VU 表可以用（「展開頁右側」面板＋main.xml 標記的歌詞欄位置）。主執行緒
bool is_vu_mode_available();

// 左上角按鈕按右鍵：跳出版面縮圖卡（2026-11-12 起左鍵固定換下一種、右鍵固定跳卡片，設定頁不再有這個選項）。主執行緒
void show_now_playing_mode_picker();

} // namespace cui::main_menu
