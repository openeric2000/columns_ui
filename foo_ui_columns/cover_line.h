#pragma once

// 自編版新增（2026-11-14）：展開頁封面下方資訊選「精簡」時，封面下面那一排顯示什麼，右鍵跳挑選卡切換
// （使用者在預覽頁 https://claude.ai/artifact/NWLAgXqzcKWaAjYaxinRXR 留下 2、3、4，要「像視覺化那樣右鍵選」）
// - 三種：下一首（沒有下一首時改顯示等級＋格式）、等級＋格式（2026-11-15 起預設，使用者要求；原本預設下一首）、播放統計
// - main.xml 讀 %cui_cover_line%（1 下一首、2 等級＋格式、3 播放統計）決定顯示哪一排；
//   下一首的歌名、藝人是 %cui_next_title%、%cui_next_artist%（沒有下一首時不存在）
// - 那一排是 ESLyric 畫的，右鍵會開 ESLyric 自己的選單，所以上面蓋一層看不見的子視窗（同歌詞防誤拖的作法）：
//   只接右鍵、開挑選卡，其他滑鼠訊息照常轉給底下的 ESLyric
// - 視窗由 FlatSplitterPanel 在有播放列時建立，跟放大鈕、歌詞防誤拖同一批排版

namespace cui::cover_line {

enum class Style : uint32_t { next = 1, format = 2, stats = 3 };
inline constexpr uint32_t style_count = 3;
// 預設只影響沒存過這個設定的環境（存過的照舊）
inline constexpr uint32_t style_default = static_cast<uint32_t>(Style::format);

extern fbh::ConfigUint32 style;

// 標題格式欄位用（任何執行緒都可以呼叫）：目前的樣式（1～3）、下一首的歌名或藝人（沒有下一首時傳回 false）
uint32_t style_for_fields();
bool next_text_for_fields(bool artist, std::string& out);

HWND create(HWND parent);
void layout(HWND wnd, const RECT& bar_rect);
HDWP defer_layout(HDWP dwp, HWND wnd, const RECT& bar_rect);
// 用上一次的播放列位置重排所有熱區（設定、模式改了時用）
void refresh_layouts();

} // namespace cui::cover_line
