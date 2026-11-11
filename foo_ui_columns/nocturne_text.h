#pragma once

// 自編版新增：主題（Nocturne for foobar2000）加上去的介面文字，中英兩套
// - 編譯時定義 NOCTURNE_LANG_EN 就是英文版，沒定義是中文版（個人版照舊）
// - 選單名稱是 ESLyric 佈局 main.xml 用 mm://View/Layout/<名稱> 呼叫的路徑，改名時兩邊要一起改
// - ESLyric 新裝時是英文介面（歌詞右鍵選單是 Panel Options...），可以在它的設定裡切成中文（面板參數選項），兩種都寫

namespace cui::nocturne_text {

// 產品名稱與版本號（設定頁「關於」分頁顯示；中英文版共用，發布新版時只改這一行版本號）
inline constexpr wchar_t about_product_name[] = L"Nocturne for foobar2000";
inline constexpr wchar_t nocturne_version[] = L"1.40";

#ifdef NOCTURNE_LANG_EN

// 選單（View/Layout）
inline constexpr auto toggle_now_playing_name = "Toggle Now Playing";
inline constexpr auto toggle_now_playing_description
    = "Lets the panel with the custom title \"Now Playing Bar\" cover the panel titled \"Main Content\" at the same "
      "level, or puts it back. Does not expand while stopped with an empty queue.";
inline constexpr auto stop_or_collapse_name = "Stop or Collapse Now Playing";
inline constexpr auto stop_or_collapse_description
    = "Stops playback. If playback is already stopped and the now playing view is still open (it stays open while the "
      "queue has tracks), closes the view and leaves the queue as it is.";
inline constexpr auto next_track_toggle_name = "Up Next Notice";
inline constexpr auto next_track_toggle_description
    = "Shows the next track in a corner of the now playing view during the last seconds of a track. The corner can be "
      "chosen in Preferences > Tools > Nocturne.";
inline constexpr wchar_t next_caption[] = L"Up next";
inline constexpr wchar_t next_caption_stopped[] = L"Stopped · Up next";
inline constexpr wchar_t stopped_label[] = L"Stopped";
inline constexpr wchar_t next_source_queue[] = L"Queue";
inline constexpr wchar_t next_source_list[] = L"Playlist";
// 播放列的輸出裝置顯示（output_chip.cpp）
inline constexpr wchar_t chip_kind_default[] = L"Default";
inline constexpr wchar_t chip_kind_exclusive[] = L"Exclusive";
inline constexpr wchar_t chip_kind_mute[] = L"Muted";
inline constexpr wchar_t chip_mode_exclusive[] = L"Exclusive mode";
inline constexpr wchar_t chip_mode_asio[] = L"ASIO mode";
inline constexpr wchar_t chip_menu_title[] = L"Output mode";
// 設定頁「輸出裝置」區塊與裝置名稱檔的範本
inline constexpr wchar_t chip_settings_title[] = L"Output device";
inline constexpr wchar_t chip_show[] = L"Show the current output device in the playback bar";
inline constexpr wchar_t chip_click[] = L"Click it to switch device (the hover panel also shows the hint)";
inline constexpr wchar_t chip_edit_names[] = L"Edit device names...";
inline constexpr char chip_alias_template[]
    = "# Output device names: one per line, \"text the device name contains=name to show\".\r\n"
      "# Lines starting with # are comments. Save the file, then hover over or click the output device display to apply.\r\n"
      "# Example: Holo Audio=DAC (Holo Audio)\r\n";
inline constexpr wchar_t chip_mode_default[] = L"Default (shared)";
inline constexpr wchar_t chip_mode_mute[] = L"Muted output";
inline constexpr wchar_t chip_hint[] = L"Click to switch device";
// 音量圖示（volume_chip.cpp）：設定頁的開關與靜音時提示泡的字
inline constexpr wchar_t volume_title[] = L"Volume";
inline constexpr wchar_t volume_icon_only[] = L"Collapse the volume bar to an icon (recommended at 125%+ Windows scaling)";
inline constexpr wchar_t volume_muted[] = L"Muted";
inline constexpr wchar_t chip_unknown[] = L"Unknown device";
// 設定頁（Preferences → Tools → Nocturne，2026-11-05 起；原本在 Columns UI 的分頁裡）
// 「下一首提示」與「播放佇列」兩個只有少數選項的分頁合併成一頁（2026-11-08 使用者要求）
inline constexpr auto next_toast_tab_name = "Up Next & Queue";
inline constexpr wchar_t next_toast_title[] = L"Up Next Notice";
inline constexpr wchar_t next_toast_enabled[] = L"Show the next track near the end of the playing track";
inline constexpr wchar_t next_toast_seconds_label[] = L"Start counting down this many seconds before the track ends (3-30)";
inline constexpr wchar_t next_toast_show_seconds[] = L"Show the remaining seconds inside the ring";
inline constexpr wchar_t next_toast_show_cover[] = L"Show the cover of the next track";
inline constexpr wchar_t next_toast_show_tag[] = L"Show where the next track comes from (Queue / Playlist)";
inline constexpr wchar_t next_toast_position_label[] = L"Position";
inline constexpr wchar_t next_toast_position_names[4][16] = {L"Top right", L"Bottom right", L"Top left", L"Bottom left"};
inline constexpr wchar_t next_toast_size_label[] = L"Size";
inline constexpr wchar_t next_toast_size_names[4][24] = {L"Small (85%)", L"Standard", L"Large (120%)", L"Extra large (140%)"};
inline constexpr wchar_t keep_queue_title[] = L"Playback queue";
inline constexpr wchar_t keep_queue_on_play[] = L"Keep the queue when playing another track (double-click, Enter, Play)";
// 設定頁的分頁：播放、下一首與佇列、字型、介面、關於
inline constexpr auto playback_tab_name = "Playback";
inline constexpr wchar_t progress_title[] = L"Progress bar";
inline constexpr wchar_t wheel_seek_enabled[] = L"Seek with the mouse wheel over the progress bar";
inline constexpr wchar_t wheel_step_label[] = L"Seconds per notch (1-60)";
inline constexpr wchar_t wheel_shift_step_label[] = L"Seconds per notch while holding Shift (5-300)";
inline constexpr auto font_tab_name = "Fonts";
inline constexpr wchar_t font_tab_title[] = L"Fonts";
// 「關於」分頁：只放產品名稱與版本號（2026-11-06，使用者要求「不要寫太厚、主要就寫版本號」）
inline constexpr auto about_tab_name = "About";
inline constexpr wchar_t about_tab_title[] = L"About";
inline constexpr wchar_t about_version_prefix[] = L"Version ";
// 個人建置在版本號後面加上版本種類（見 nocturne_build_info.h，由 build_columns_ui.ps1 產生）
inline constexpr wchar_t about_edition_dev[] = L" Development build";
inline constexpr wchar_t about_edition_personal[] = L" Personal build";
// 「介面」分頁（2026-11-08）：原本的「外觀」「展開頁」「啟動」三個各只有一個設定的分頁合併成一頁，各自一個小標題
inline constexpr auto interface_tab_name = "Interface";
// 捲軸：細捲軸開關
inline constexpr wchar_t appearance_tab_title[] = L"Scroll bars and lists";
inline constexpr wchar_t thin_scrollbar[] = L"Use thin scroll bars";
inline constexpr wchar_t thin_scrollbar_note[]
    = L"A slim scroll bar that widens when the pointer is near. Turn off for the standard Windows scroll bars.";
inline constexpr wchar_t thin_autohide_vertical[] = L"Folder tree: auto-hide the vertical scroll bar";
inline constexpr wchar_t thin_autohide_horizontal[] = L"Folder tree: auto-hide the horizontal scroll bar";
// 資料夾樹的搜尋框放上面還是下面（2026-11-09）
inline constexpr wchar_t folder_search_position_label[] = L"Folder tree: search box position";
inline constexpr wchar_t folder_search_position_names[2][16] = {L"Top", L"Bottom (default)"};
// 播放清單框選方框（2026-11-09）
inline constexpr wchar_t playlist_marquee[] = L"Playlist: drag to box-select";
// 暫時隱藏伴奏版（2026-11-11）：只影響「目前選取資料夾」，邏輯在自編 Album list 的 playlist_utils.cpp
inline constexpr wchar_t hide_instrumental_title[] = L"Current folder";
inline constexpr wchar_t hide_instrumental[] = L"Hide instrumental versions";
inline constexpr wchar_t hide_instrumental_note[]
    = L"Tracks whose file name or title contains Instrumental, Inst., Off Vocal, Karaoke and similar words are left out "
      L"of the current folder playlist. Takes effect right away.";
// 展開頁：封面虛化背景
inline constexpr wchar_t expanded_tab_title[] = L"Now playing view";
inline constexpr wchar_t expanded_bg[] = L"Blurred cover background";
inline constexpr wchar_t expanded_bg_cap_label[] = L"Brightness limit";
inline constexpr wchar_t expanded_bg_cap_names[6][24]
    = {L"10% (very dark)", L"15%", L"20% (default)", L"25%", L"30%", L"35% (lighter)"};
// 啟動（開機優化 B）
inline constexpr wchar_t startup_tab_title[] = L"Startup";
inline constexpr wchar_t startup_hold[] = L"Show the window only when the web panel is ready";
inline constexpr wchar_t startup_hold_note[]
    = L"Keeps the window hidden until the album header is ready (3 seconds at most). Takes effect the next time "
      L"foobar2000 starts.";
inline constexpr wchar_t font_tab_note[]
    = L"Change the font of each part separately, or all parts at once with the first row. The lists, tabs, folder tree "
      L"and other interface text also take a size. These apply at once.";
inline constexpr wchar_t font_row_all[] = L"All parts";
inline constexpr wchar_t font_row_playlist[] = L"Playlist";
inline constexpr wchar_t font_row_tabs[] = L"Playlist tabs";
inline constexpr wchar_t font_row_tree[] = L"Folder tree";
inline constexpr wchar_t font_row_ui[] = L"Other interface text";
inline constexpr wchar_t font_row_bar[] = L"Now playing bar and view";
inline constexpr wchar_t font_row_header[] = L"Album header";
inline constexpr wchar_t font_tab_foot[]
    = L"The now playing bar, the now playing view and the album header only take a font (no size) and need a restart "
      L"of foobar2000 to show it.";
inline constexpr wchar_t font_restart_note[] = L"Changed fonts will show after foobar2000 restarts.";
inline constexpr wchar_t font_restart_button[] = L"Restart now";
inline constexpr auto show_cover_name = "Show Large Cover";
// 展開頁封面右下角的放大鈕（2026-11-09，now_playing_cover_zoom.cpp）
inline constexpr wchar_t cover_zoom_tooltip[] = L"View the large cover";
inline constexpr auto show_cover_description = "Opens the cover of the playing track in the foobar2000 picture viewer.";
inline constexpr auto theme_font_name = "Theme Font…";
inline constexpr auto theme_font_description
    = "Changes the font of the lists, tabs, folder tree, now playing bar and album header at once, and sets the "
      "overall list size.";
inline constexpr auto style_centered_name = "Now Playing: Centered";
inline constexpr auto style_centered_description = "Centers the cover and lyrics together in the now playing view.";
inline constexpr auto style_info_name = "Now Playing: Left + Info Card";
inline constexpr auto style_info_description = "Puts the cover on the left and track information on the right.";
inline constexpr auto cycle_style_name = "Toggle Now Playing Style";
inline constexpr auto cycle_style_description
    = "Switches the now playing view between \"Centered\" and \"Left + Info Card\".";
// 展開頁顯示內容（2026-11-11，v1.5 待做清單第 4 個）：歌詞／純封面／VU 表，左上角按鈕切換
inline constexpr auto mode_lyrics_name = "Now Playing: Lyrics";
inline constexpr auto mode_lyrics_description = "Shows the cover and the lyrics in the now playing view.";
inline constexpr auto mode_cover_name = "Now Playing: Cover Only";
inline constexpr auto mode_cover_description = "Shows only a large cover in the middle of the now playing view.";
inline constexpr auto mode_vu_name = "Now Playing: VU Meter";
inline constexpr auto mode_vu_description
    = "Shows the VU meter in place of the lyrics.";
inline constexpr auto mode_button_name = "Switch Now Playing Content";
inline constexpr auto mode_button_description
    = "Switches what the now playing view shows. Clicking moves to the next one or opens a menu, as set in Preferences.";
// 彈出選單與切換時的提示（第 4 個是版面沒標記歌詞欄位置、不能用 VU 表時的灰色項目）
inline constexpr wchar_t mode_short_names[4][32] = {L"Lyrics", L"Cover only", L"VU meter", L"VU meter (layout not supported)"};
inline constexpr const char* mode_toast_names[3] = {"Lyrics", "Cover only", "VU meter"};
// 設定頁「介面」分頁、展開頁那一段
inline constexpr wchar_t mode_button_label[] = L"Top-left button";
inline constexpr wchar_t mode_button_names[2][32] = {L"Next on click (default)", L"Menu on click"};
// 「展開頁」分頁（2026-11-11）：背景、顯示內容、歌詞三段
inline constexpr auto expanded_page_tab_name = "Now Playing View";
inline constexpr wchar_t expanded_section_background[] = L"Background";
inline constexpr wchar_t expanded_section_content[] = L"Content";
inline constexpr wchar_t expanded_section_lyrics[] = L"Lyrics";
inline constexpr wchar_t mode_note[]
    = L"Only the ticked ones take part in the top-left button.";
// 「展開頁要有」勾選下拉鈕（2026-11-11）
inline constexpr wchar_t modes_label[] = L"Show";
inline constexpr wchar_t modes_separator[] = L", ";
inline constexpr wchar_t modes_dropdown_arrow[] = L"  \x25BE";
inline constexpr wchar_t lyrics_lock[] = L"Lock the lyrics against dragging";
inline constexpr wchar_t lyrics_lock_note[]
    = L"Left-clicking or dragging the lyrics no longer jumps to another time. The mouse wheel and right-click still "
      L"work.";

// 主選單按鈕面板
inline constexpr auto menu_button_panel_name = "Main Menu Button";
inline constexpr wchar_t collapse_text[] = L"Collapse All";
inline constexpr wchar_t menu_tooltip[] = L"Menu";

// 主題字型對話框
inline constexpr wchar_t theme_font_title_w[] = L"Theme Font";
inline constexpr auto theme_font_title = "Theme Font";
inline constexpr wchar_t theme_font_intro[]
    = L"Pick a font to change the lists, tabs, folder tree, now playing bar, now playing view and album header at "
      L"once. The weight is always Regular.";
inline constexpr wchar_t theme_font_family_label[] = L"Font";
inline constexpr wchar_t theme_font_size_label[]
    = L"Overall size (lists, tabs and folder tree scale together; the now playing bar and header stay the same)";
inline constexpr wchar_t theme_font_lyrics_note[]
    = L"The lyrics font is set separately: right-click the lyrics → Panel Options...";
inline constexpr wchar_t ok_text[] = L"OK";
inline constexpr wchar_t cancel_text[] = L"Cancel";
inline constexpr wchar_t size_small[] = L"Small (90%)";
inline constexpr wchar_t size_standard[] = L"Standard";
inline constexpr wchar_t size_large[] = L"Large (115%)";
inline constexpr wchar_t size_extra_large[] = L"Extra Large (130%)";

// 主題字型的結果訊息（{} 依序是字型名、大小）
inline constexpr auto error_file_not_found = "file not found";
inline constexpr auto error_font_setting_not_found = "no font setting found in the file";
inline constexpr auto error_write_failed = "could not write the file";
inline constexpr auto error_no_font_list = "Could not read the list of system fonts.";
inline constexpr auto error_no_usable_style = "This font has no usable style.";
inline constexpr char error_change_failed[] = "Could not change the font: {}";
inline constexpr char failure_item[] = "\n{} ({})";
inline constexpr char applied_message[]
    = "Changed to \"{}\", size \"{}\". Everything has been applied.\n\n"
      "The lyrics font is set separately: right-click the lyrics → Panel Options...";
inline constexpr char restart_message[]
    = "Changed to \"{}\", size \"{}\".\n\n"
      "The lists, tabs and folder tree are already updated. The now playing bar, now playing view and album header "
      "switch fonts after foobar2000 restarts (their size stays the same).\n"
      "The lyrics font is set separately: right-click the lyrics → Panel Options...";
inline constexpr char failures_message[] = "\n\nThese files were not updated. Please edit them in Notepad:{}";
inline constexpr auto restart_question = "\n\nRestart foobar2000 now?";

#else

// 選單（View/Layout）
inline constexpr auto toggle_now_playing_name = "切換播放頁";
inline constexpr auto toggle_now_playing_description
    = "讓自訂標題為「播放列」的面板蓋住同一層自訂標題為「主內容」的面板，或縮回原位。停止播放而且佇列沒有歌時不展開。";
inline constexpr auto stop_or_collapse_name = "停止或收起播放頁";
inline constexpr auto stop_or_collapse_description
    = "停止播放；已經停止而展開頁還開著（佇列有歌時停止不會自動收）時，收起展開頁，佇列保留不動。";
inline constexpr auto next_track_toggle_name = "下一首提示";
inline constexpr auto next_track_toggle_description
    = "在展開頁的角落，歌曲最後幾秒顯示下一首；角落可在 Preferences > Tools > Nocturne 選。";
inline constexpr wchar_t next_caption[] = L"下一首";
inline constexpr wchar_t next_caption_stopped[] = L"已停止 · 接下來";
inline constexpr wchar_t stopped_label[] = L"已停止";
inline constexpr wchar_t next_source_queue[] = L"佇列";
inline constexpr wchar_t next_source_list[] = L"清單";
// 播放列的輸出裝置顯示（output_chip.cpp）
inline constexpr wchar_t chip_kind_default[] = L"預設";
inline constexpr wchar_t chip_kind_exclusive[] = L"獨占";
inline constexpr wchar_t chip_kind_mute[] = L"靜音";
inline constexpr wchar_t chip_mode_exclusive[] = L"獨占模式";
inline constexpr wchar_t chip_mode_asio[] = L"ASIO 模式";
inline constexpr wchar_t chip_menu_title[] = L"輸出模式";
// 設定頁「輸出裝置」區塊與裝置名稱檔的範本
inline constexpr wchar_t chip_settings_title[] = L"輸出裝置";
inline constexpr wchar_t chip_show[] = L"在播放列顯示目前的輸出裝置";
inline constexpr wchar_t chip_click[] = L"點一下可切換裝置（滑過面板的提示也一起顯示）";
inline constexpr wchar_t chip_edit_names[] = L"編輯裝置顯示名稱…";
inline constexpr char chip_alias_template[]
    = "# 輸出裝置顯示名稱：每行「裝置名稱裡要有的字=要顯示的名稱」，# 開頭是註解。\r\n"
      "# 存檔後，到播放列的輸出裝置顯示上滑過或點一下就會套用，不用重開 foobar2000。\r\n"
      "# 例：Holo Audio=DAC (Holo Audio)\r\n";
inline constexpr wchar_t chip_mode_default[] = L"預設（共用）";
inline constexpr wchar_t chip_mode_mute[] = L"靜音輸出";
inline constexpr wchar_t chip_hint[] = L"點一下可切換裝置";
// 音量圖示（volume_chip.cpp）：設定頁的開關與靜音時提示泡的字
inline constexpr wchar_t volume_title[] = L"音量";
inline constexpr wchar_t volume_icon_only[] = L"音量條收成圖示（Windows 顯示縮放 125% 以上建議開啟）";
inline constexpr wchar_t volume_muted[] = L"靜音";
inline constexpr wchar_t chip_unknown[] = L"未知裝置";
// 設定頁（Preferences → Tools → Nocturne，2026-11-05 起；原本在 Columns UI 的分頁裡）
inline constexpr auto next_toast_tab_name = "下一首與佇列";
inline constexpr wchar_t next_toast_title[] = L"下一首提示";
inline constexpr wchar_t next_toast_enabled[] = L"歌曲快結束時，在展開頁顯示下一首";
inline constexpr wchar_t next_toast_seconds_label[] = L"歌曲最後幾秒開始倒數（3～30 秒）";
inline constexpr wchar_t next_toast_show_seconds[] = L"圈內顯示剩餘秒數";
inline constexpr wchar_t next_toast_show_cover[] = L"顯示下一首的專輯封面";
inline constexpr wchar_t next_toast_show_tag[] = L"顯示下一首的來源標籤（佇列／清單）";
inline constexpr wchar_t next_toast_position_label[] = L"位置";
inline constexpr wchar_t next_toast_position_names[4][16] = {L"右上角", L"右下角", L"左上角", L"左下角"};
inline constexpr wchar_t next_toast_size_label[] = L"大小";
inline constexpr wchar_t next_toast_size_names[4][24] = {L"小（85%）", L"標準", L"大（120%）", L"特大（140%）"};
inline constexpr wchar_t keep_queue_title[] = L"播放佇列";
inline constexpr wchar_t keep_queue_on_play[] = L"雙擊、按 Enter 或右鍵選 Play 播放別首歌時，保留播放佇列（不清掉）";
// 設定頁的分頁：播放、下一首與佇列、字型、介面、關於
inline constexpr auto playback_tab_name = "播放";
inline constexpr wchar_t progress_title[] = L"進度條";
inline constexpr wchar_t wheel_seek_enabled[] = L"在進度條上用滑鼠滾輪前後跳轉";
inline constexpr wchar_t wheel_step_label[] = L"每滾一格跳幾秒（1～60）";
inline constexpr wchar_t wheel_shift_step_label[] = L"按住 Shift 每滾一格跳幾秒（5～300）";
inline constexpr auto font_tab_name = "字型";
inline constexpr wchar_t font_tab_title[] = L"字型";
// 「關於」分頁：只放產品名稱與版本號（2026-11-06，使用者要求「不要寫太厚、主要就寫版本號」）
inline constexpr auto about_tab_name = "關於";
inline constexpr wchar_t about_tab_title[] = L"關於";
inline constexpr wchar_t about_version_prefix[] = L"版本 ";
// 個人建置在版本號後面加上版本種類（見 nocturne_build_info.h，由 build_columns_ui.ps1 產生）
inline constexpr wchar_t about_edition_dev[] = L" 開發版";
inline constexpr wchar_t about_edition_personal[] = L" 個人版";
// 「介面」分頁（2026-11-08）：原本的「外觀」「展開頁」「啟動」三個各只有一個設定的分頁合併成一頁，各自一個小標題
inline constexpr auto interface_tab_name = "介面";
// 捲軸：細捲軸開關
inline constexpr wchar_t appearance_tab_title[] = L"捲軸與清單";
inline constexpr wchar_t thin_scrollbar[] = L"使用細捲軸";
inline constexpr wchar_t thin_scrollbar_note[]
    = L"很細的捲軸，滑鼠靠近才變寬。關掉就用 Windows 原本的捲軸。";
inline constexpr wchar_t thin_autohide_vertical[] = L"資料夾樹：直向捲軸自動隱藏";
inline constexpr wchar_t thin_autohide_horizontal[] = L"資料夾樹：橫向捲軸自動隱藏";
// 資料夾樹的搜尋框放上面還是下面（2026-11-09）
inline constexpr wchar_t folder_search_position_label[] = L"資料夾樹：搜尋框位置";
inline constexpr wchar_t folder_search_position_names[2][16] = {L"上方", L"下方（預設）"};
// 播放清單框選方框（2026-11-09）
inline constexpr wchar_t playlist_marquee[] = L"播放清單：按住左鍵拖曳可框選";
// 暫時隱藏伴奏版（2026-11-11）：只影響「目前選取資料夾」，邏輯在自編 Album list 的 playlist_utils.cpp
inline constexpr wchar_t hide_instrumental_title[] = L"目前選取資料夾";
inline constexpr wchar_t hide_instrumental[] = L"隱藏伴奏版（Instrumental）";
inline constexpr wchar_t hide_instrumental_note[]
    = L"檔名或歌名有 Instrumental、Inst.、Off Vocal、Karaoke、カラオケ、伴奏 這些字的歌不會放進「目前選取資料夾」。改了馬上生效。";
// 展開頁：封面虛化背景
inline constexpr wchar_t expanded_tab_title[] = L"展開頁";
inline constexpr wchar_t expanded_bg[] = L"封面虛化背景";
inline constexpr wchar_t expanded_bg_cap_label[] = L"亮度上限";
inline constexpr wchar_t expanded_bg_cap_names[6][24]
    = {L"10%（很暗）", L"15%", L"20%（預設）", L"25%", L"30%", L"35%（偏亮）"};
// 啟動（開機優化 B）
inline constexpr wchar_t startup_tab_title[] = L"啟動";
inline constexpr wchar_t startup_hold[] = L"等網頁面板載好再顯示視窗";
inline constexpr wchar_t startup_hold_note[]
    = L"等專輯標頭載好才顯示視窗（最多等 3 秒）。下次啟動才生效。";
inline constexpr wchar_t font_tab_note[]
    = L"每個區塊可以各自換字型，也可以用第一列一次全換。清單、分頁、資料夾樹與其餘介面文字還能調大小，改了馬上生效。";
inline constexpr wchar_t font_row_all[] = L"全部區塊";
inline constexpr wchar_t font_row_playlist[] = L"播放清單";
inline constexpr wchar_t font_row_tabs[] = L"分頁標籤";
inline constexpr wchar_t font_row_tree[] = L"資料夾樹";
inline constexpr wchar_t font_row_ui[] = L"其餘介面文字";
inline constexpr wchar_t font_row_bar[] = L"播放列與展開頁";
inline constexpr wchar_t font_row_header[] = L"專輯標頭";
inline constexpr wchar_t font_tab_foot[]
    = L"播放列、展開頁與專輯標頭只能換字型、不能調大小，要重開 foobar2000 才會換。";
inline constexpr wchar_t font_restart_note[] = L"換過字型的部分要重開 foobar2000 才會生效。";
inline constexpr wchar_t font_restart_button[] = L"立即重開";
inline constexpr auto show_cover_name = "顯示封面大圖";
// 展開頁封面右下角的放大鈕（2026-11-09，now_playing_cover_zoom.cpp）
inline constexpr wchar_t cover_zoom_tooltip[] = L"看封面大圖";
inline constexpr auto show_cover_description = "在 foobar2000 picture viewer 開啟正在播放曲目的封面。";
inline constexpr auto theme_font_name = "主題字型…";
inline constexpr auto theme_font_description
    = "一次換掉清單、分頁、資料夾樹、播放列與專輯標頭的字型，並調整清單的整體大小。";
inline constexpr auto style_centered_name = "展開頁：置中";
inline constexpr auto style_centered_description = "展開頁的封面與歌詞整組置中。";
inline constexpr auto style_info_name = "展開頁：靠左＋資訊卡";
inline constexpr auto style_info_description = "展開頁的封面靠左，右邊顯示曲目資訊。";
inline constexpr auto cycle_style_name = "切換展開頁樣式";
inline constexpr auto cycle_style_description = "在展開頁樣式「置中」與「靠左＋資訊卡」之間切換。";
// 展開頁顯示內容（2026-11-11，v1.5 待做清單第 4 個）：歌詞／純封面／VU 表，左上角按鈕切換
inline constexpr auto mode_lyrics_name = "展開頁：歌詞";
inline constexpr auto mode_lyrics_description = "展開頁顯示封面與歌詞。";
inline constexpr auto mode_cover_name = "展開頁：純封面";
inline constexpr auto mode_cover_description = "展開頁只在中間顯示放大的封面。";
inline constexpr auto mode_vu_name = "展開頁：VU 表";
inline constexpr auto mode_vu_description = "展開頁歌詞的位置換成 VU 表。";
inline constexpr auto mode_button_name = "切換展開頁顯示內容";
inline constexpr auto mode_button_description = "切換展開頁要顯示什麼；點一下換下一種或跳出選單，依設定頁的選擇。";
// 彈出選單與切換時的提示（第 4 個是版面沒標記歌詞欄位置、不能用 VU 表時的灰色項目）
inline constexpr wchar_t mode_short_names[4][32] = {L"歌詞", L"純封面", L"VU 表", L"VU 表（這個版面不支援）"};
inline constexpr const char* mode_toast_names[3] = {"歌詞", "純封面", "VU 表"};
// 設定頁「介面」分頁、展開頁那一段
inline constexpr wchar_t mode_button_label[] = L"左上角按鈕";
inline constexpr wchar_t mode_button_names[2][32] = {L"點一下換下一種（預設）", L"點一下跳出選單"};
// 「展開頁」分頁（2026-11-11）：背景、顯示內容、歌詞三段
inline constexpr auto expanded_page_tab_name = "展開頁";
inline constexpr wchar_t expanded_section_background[] = L"背景";
inline constexpr wchar_t expanded_section_content[] = L"顯示內容";
inline constexpr wchar_t expanded_section_lyrics[] = L"歌詞";
inline constexpr wchar_t mode_note[] = L"勾選的才會出現在左上角按鈕的輪流與選單裡。";
// 「展開頁要有」勾選下拉鈕（2026-11-11）
inline constexpr wchar_t modes_label[] = L"展開頁要有";
inline constexpr wchar_t modes_separator[] = L"、";
inline constexpr wchar_t modes_dropdown_arrow[] = L"  \x25BE";
inline constexpr wchar_t lyrics_lock[] = L"歌詞防誤拖";
inline constexpr wchar_t lyrics_lock_note[] = L"開著時，在歌詞上按左鍵或拖曳不會跳到別的時間；滾輪和右鍵照常。";

// 主選單按鈕面板
inline constexpr auto menu_button_panel_name = "主選單按鈕";
// 2026-10-28 使用者覺得只寫「收合」看不出要收合什麼，改「全部收合」（「收合清單」的「清單」容易被當成播放清單）
inline constexpr wchar_t collapse_text[] = L"全部收合";
inline constexpr wchar_t menu_tooltip[] = L"選單";

// 主題字型對話框
inline constexpr wchar_t theme_font_title_w[] = L"主題字型";
inline constexpr auto theme_font_title = "主題字型";
inline constexpr wchar_t theme_font_intro[]
    = L"選一個字型，一次換掉清單、分頁、資料夾樹、播放列、展開頁與上方專輯標頭的字型。粗細固定用一般粗細。";
inline constexpr wchar_t theme_font_family_label[] = L"字型";
inline constexpr wchar_t theme_font_size_label[] = L"整體大小（清單、分頁與資料夾樹一起放大縮小；播放列與標頭固定不變）";
inline constexpr wchar_t theme_font_lyrics_note[] = L"歌詞字型不在這裡，請在歌詞上按右鍵 → Panel Options...（ESLyric 切成中文時是「面板參數選項」）另外設定。";
inline constexpr wchar_t ok_text[] = L"確定";
inline constexpr wchar_t cancel_text[] = L"取消";
inline constexpr wchar_t size_small[] = L"小（90%）";
inline constexpr wchar_t size_standard[] = L"標準";
inline constexpr wchar_t size_large[] = L"大（115%）";
inline constexpr wchar_t size_extra_large[] = L"特大（130%）";

// 主題字型的結果訊息（{} 依序是字型名、大小）
inline constexpr auto error_file_not_found = "找不到檔案";
inline constexpr auto error_font_setting_not_found = "檔案裡找不到字型設定";
inline constexpr auto error_write_failed = "寫入失敗";
inline constexpr auto error_no_font_list = "讀不到系統字型清單。";
inline constexpr auto error_no_usable_style = "這個字型沒有可以用的樣式。";
inline constexpr char error_change_failed[] = "換字型失敗：{}";
inline constexpr char failure_item[] = "\n{}（{}）";
inline constexpr char applied_message[]
    = "已經換成「{}」，大小「{}」，都已經套用好了。\n\n"
      "歌詞字型不在這裡，請在歌詞上按右鍵 → Panel Options...（ESLyric 切成中文時是「面板參數選項」）另外設定。";
inline constexpr char restart_message[]
    = "已經換成「{}」，大小「{}」。\n\n"
      "清單、分頁與資料夾樹已經換好；播放列、展開頁與上方專輯標頭要重開 foobar2000 才會換字型（大小固定不變）。\n"
      "歌詞字型不在這裡，請在歌詞上按右鍵 → Panel Options...（ESLyric 切成中文時是「面板參數選項」）另外設定。";
inline constexpr char failures_message[] = "\n\n下面的檔案沒有換到，要自己用記事本改：{}";
inline constexpr auto restart_question = "\n\n要現在重開 foobar2000 嗎？";

#endif

} // namespace cui::nocturne_text
