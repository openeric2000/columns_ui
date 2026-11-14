#pragma once

// 自編版新增：展開頁大封面右下角的「看大圖」放大鈕（2026-11-09，v1.5 待做清單第 2 個；使用者選方案 1）
// 點封面已經改成「切到資訊卡」，看大圖的入口改成封面右下角一顆小放大鈕。ESLyric 的 albumart 一定畫在同一塊所有元素上面，
// 放在 main.xml 裡的按鈕會被封面蓋住（點得到、看不到），所以跟音量圖示一樣由 Columns UI 開一個分層子視窗蓋上去：
// - 外觀：Segoe MDL2「全螢幕」雙箭頭，離封面右緣、下緣各 14 DIP（可點範圍 44 DIP）；平常只有很淡的箭頭（不畫圓底，
//   使用者看了幾版淡圓底都說太明顯），滑過才出現深色圓底、箭頭變全白
// - 點一下開 foobar2000 picture viewer（跟主選單「顯示封面大圖」相同）
// - 封面位置由 main.xml 裡的一行標記註解告訴這裡（Columns UI 不知道 ESLyric 把封面排在哪），例如
//     <!-- nocturne-cover-zoom: left=472 cover=600 gap=24 info=115 -->
//   left＝展開頁左緣到封面左緣、cover＝封面邊長、gap＝封面下緣到歌名、info＝封面下面那塊資訊在 ESLyric 排版時佔的高度；
//   數字是 DIP，加 % 是展開頁寬度的百分比。封面＋gap＋info 這一整組在展開頁裡垂直置中（main.xml 的 disc 容器 justify=middle）。
//   info 是實測反推的（2026-11-09：ESLyric 排版時這塊的高度跟實際顯示幾行無關，兩行、四行時封面上緣位置相同）。
//   沒有這行標記的版面（例如公開版、169 舊版面）不顯示按鈕
// - 只在展開頁完全展開（動畫、截圖都結束）而且是封面模式（不是資訊卡）時顯示
// - 視窗由 FlatSplitterPanel 在有播放列時建立，排版時跟進度條、音量圖示同一批放到最上層

namespace cui::cover_zoom {

// 自編版新增（2026-11-14）：展開頁封面下方資訊「詳細／精簡」（設定頁「展開頁」分頁，預設詳細，使用者指定）。
// 詳細＝歌名、藝人・年份、專輯、規格列四行；精簡＝封面放大、下面只留一排（下一首／等級＋格式／播放統計，右鍵挑選，見 cover_line.h）。
// main.xml 讀 %cui_expanded_compact%（精簡時為 1）換左欄；這裡的封面位置改照標記的 compact-cover／compact-gap／compact-info
inline constexpr bool compact_info_default = false;
extern fbh::ConfigBool compact_info;

// 在 parent（分割面板視窗）底下建立按鈕視窗（先不顯示）；失敗傳回 nullptr
HWND create(HWND parent);

// 依 bar_rect（播放列在分割面板裡的位置；展開頁＝它上面那一段）排位置；不該顯示時藏起來
void layout(HWND wnd, const RECT& bar_rect);

// 同 layout，但加進呼叫端的 DeferWindowPos 批次（要放在把播放列容器放到最上層的那一筆「之後」）
HDWP defer_layout(HDWP dwp, HWND wnd, const RECT& bar_rect);

// 用上一次的 bar_rect 重排所有按鈕（展開動畫結束、切換展開頁樣式、GDI+ 字型暖機做完時用）
void refresh_layouts();

// 封面方塊在展開頁裡的位置（展開頁左上角為原點、實際像素），照 main.xml 的 nocturne-cover-zoom 標記算；
// 版面沒有標記、展開頁太矮或封面放不下時傳回空的。不看展開頁樣式（資訊卡也在同一塊），給封面⇄資訊卡的切換動畫用
std::optional<RECT> cover_rect_in_stage(int stage_width, int stage_height);

// 純封面模式的大封面位置（2026-11-12）：照標記的 solo-cover／solo-info 算，水平置中、封面＋gap＋solo-info 垂直置中；沒寫時傳回空的
std::optional<RECT> solo_cover_rect_in_stage(int stage_width, int stage_height);

// 歌詞欄在展開頁裡的位置（展開頁左上角為原點、實際像素，整個展開頁高），照標記的 lyrics-left／lyrics-right 算（2026-11-11）；
// 例如 <!-- nocturne-cover-zoom: left=472 cover=600 gap=24 info=119 lyrics-left=1312 lyrics-right=100 vu-width=640 -->。
// 標記沒有這兩個值、或展開頁太矮時傳回空的
std::optional<RECT> lyrics_column_in_stage(int stage_width, int stage_height);

// VU 表模式時「展開頁右側」面板（VU 表）的位置：歌詞欄左緣、寬度＝歌詞欄寬（標記有 vu-width 時不超過它）、
// 高度照 VU 外觀比例 830×199、在展開頁垂直置中。沒有歌詞欄位置時傳回空的（＝這個版面沒有 VU 表模式）。
// 標記另外寫了 vu-left／vu-right（2026-11-12）時改用那一欄，不跟歌詞欄連動
std::optional<RECT> vu_rect_in_stage(int stage_width, int stage_height);

// 標記的 vu-left／vu-right 那一欄（2026-11-12，整個展開頁高）；沒寫時傳回空的
std::optional<RECT> vu_column_in_stage(int stage_width, int stage_height);

// 「歌詞＋VU 表」「歌詞＋頻譜」時歌詞在歌詞欄上半、下半放 VU 表／頻譜（2026-11-12）：歌詞那塊的下緣（展開頁左上角為原點、實際像素）。
// 照標記的 lyrics-split（展開頁高的百分比，main.xml 裡上半那塊的高度要一致），沒寫時是 56%
int lyrics_split_y(int stage_height);

// 封面下面那組資訊（歌名、藝人、專輯、技術資訊）在排版時的下緣（展開頁左上角為原點、實際像素）＝封面＋gap＋info 那一整組的下緣。
// 歌詞＋VU 表／頻譜時 VU 表／頻譜的錶框底部對齊這裡（2026-11-12 使用者要求）；沒有封面標記時傳回空的
std::optional<int> info_bottom_in_stage(int stage_width, int stage_height);
// 同上那組資訊的上緣（＝封面下緣＋gap，歌名那一行的頂端）
std::optional<int> info_top_in_stage(int stage_width, int stage_height);
// 歌詞＋VU 表／頻譜時錶框要對齊的那一段（上緣、下緣，展開頁座標、實際像素；2026-11-14）：
// 詳細時＝上面那組資訊；精簡時那組只剩一行，錶會被壓成一條，所以改成「詳細時的資訊高度」、下緣對齊精簡那一行的下緣
std::optional<std::pair<int, int>> meter_band_in_stage(int stage_width, int stage_height);

} // namespace cui::cover_zoom

// 自編版新增：歌詞防誤拖（2026-11-11 使用者要求）
// ESLyric 的歌詞可以用滑鼠左鍵按住拖曳（拖了就跳到別的時間），使用者覺得太容易不小心拉到。
// 歌詞欄上面蓋一個看不見的子視窗（透明度 1/255 的分層視窗）：左鍵（拖曳、點擊、雙擊）全部吃掉；滾輪、右鍵、中鍵轉給底下的 ESLyric。
// 只在展開頁完全展開、歌詞模式、設定頁勾著時蓋上；位置照 main.xml 標記的歌詞欄（lyrics-left／lyrics-right），沒有標記的版面不蓋。
// 視窗由 FlatSplitterPanel 在有播放列時建立，跟放大鈕同一批排版
namespace cui::lyrics_lock {

// 開關存在 fb2k 共用設定；預設開（使用者要的就是擋掉）
inline constexpr char config_key[] = "nocturne.nowPlaying.lyricsLock";
inline constexpr bool enabled_default = true;

bool is_enabled();
// 改了馬上重排（蓋上或拿掉）
void set_enabled(bool enabled);

HWND create(HWND parent);
void layout(HWND wnd, const RECT& bar_rect);
HDWP defer_layout(HDWP dwp, HWND wnd, const RECT& bar_rect);
// 用上一次的播放列位置重排所有蓋層（展開動畫結束、切換展開頁模式、設定改了時用）
void refresh_layouts();

} // namespace cui::lyrics_lock
