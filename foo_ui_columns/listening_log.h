#pragma once

// 聆聽紀錄模組對外提供的「新的播放次數」（工作區 docs\計畫書\聆聽紀錄計畫書.md 的 Step 8）。
// 新次數＝切換那一刻 foo_playcount 的次數（快照，存在 profile\nocturne-stats\baseline.jsonl）
//        ＋切換之後照新門檻（聽一半或滿 4 分鐘）算到的次數。
// 還沒載入完（fb2k 剛啟動那一下）傳回 nullopt，呼叫端退回 foo_playcount 的舊欄位，畫面不會閃成空白。
namespace cui::listening_log {

// 新的播放次數；0 次也是有值（傳回 0）
std::optional<uint32_t> play_count(metadb_handle* handle);

// 新的最後播放時間，格式同 foo_playcount 的 %last_played%（YYYY-MM-DD HH:MM:SS，本地時間）；沒播過傳回空字串
std::optional<std::string> last_played(metadb_handle* handle);

// 新的累計聆聽秒數：起始次數×歌長＋切換之後實際聽的秒數
std::optional<double> listened_seconds(metadb_handle* handle);

// 算不算一次播放（新門檻）：歌長 30 秒以上，實際聽了一半或滿 4 分鐘；不到 30 秒要播完（差 1 秒內算播完）
bool is_counted(double length, uint32_t listened);

} // namespace cui::listening_log
