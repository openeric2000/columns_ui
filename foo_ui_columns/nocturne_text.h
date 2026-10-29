#pragma once

// 自編版新增：主題（Nocturne for foobar2000）加上去的介面文字，中英兩套
// - 編譯時定義 NOCTURNE_LANG_EN 就是英文版，沒定義是中文版（個人版照舊）
// - 選單名稱是 ESLyric 佈局 main.xml 用 mm://View/Layout/<名稱> 呼叫的路徑，改名時兩邊要一起改
// - ESLyric 新裝時是英文介面（歌詞右鍵選單是 Panel Options...），可以在它的設定裡切成中文（面板參數選項），兩種都寫

namespace cui::nocturne_text {

#ifdef NOCTURNE_LANG_EN

// 選單（View/Layout）
inline constexpr auto toggle_now_playing_name = "Toggle Now Playing";
inline constexpr auto toggle_now_playing_description
    = "Lets the panel with the custom title \"Now Playing Bar\" cover the panel titled \"Main Content\" at the same "
      "level, or puts it back. Does not expand while stopped.";
inline constexpr auto show_cover_name = "Show Large Cover";
inline constexpr auto show_cover_description = "Opens the cover of the playing track in the foobar2000 picture viewer.";
inline constexpr auto theme_font_name = "Theme Font…";
inline constexpr auto theme_font_description
    = "Changes the font of the lists, tabs, folder tree, now playing bar and album header at once, and sets the "
      "overall list size.";
inline constexpr auto style_centered_name = "Now Playing: Centered";
inline constexpr auto style_centered_description = "Centers the cover and lyrics together in the now playing view.";
inline constexpr auto style_spectrum_name = "Now Playing: Left + VU Meter";
inline constexpr auto style_spectrum_description
    = "Puts the cover on the left and the VU meter on the right (the panel with the custom title \"Now Playing "
      "Side\" in the layout).";
inline constexpr auto style_info_name = "Now Playing: Left + Info Card";
inline constexpr auto style_info_description = "Puts the cover on the left and track information on the right.";
inline constexpr auto cycle_style_name = "Toggle Now Playing Style";
inline constexpr auto cycle_style_description
    = "Switches the now playing view between \"Centered\" and \"Left + Info Card\".";

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
    = "讓自訂標題為「播放列」的面板蓋住同一層自訂標題為「主內容」的面板，或縮回原位。停止播放時不展開。";
inline constexpr auto show_cover_name = "顯示封面大圖";
inline constexpr auto show_cover_description = "在 foobar2000 picture viewer 開啟正在播放曲目的封面。";
inline constexpr auto theme_font_name = "主題字型…";
inline constexpr auto theme_font_description
    = "一次換掉清單、分頁、資料夾樹、播放列與專輯標頭的字型，並調整清單的整體大小。";
inline constexpr auto style_centered_name = "展開頁：置中";
inline constexpr auto style_centered_description = "展開頁的封面與歌詞整組置中。";
inline constexpr auto style_spectrum_name = "展開頁：靠左＋VU 表";
inline constexpr auto style_spectrum_description
    = "展開頁的封面靠左，右邊顯示 VU 表（版面裡自訂標題「展開頁右側」的面板）。";
inline constexpr auto style_info_name = "展開頁：靠左＋資訊卡";
inline constexpr auto style_info_description = "展開頁的封面靠左，右邊顯示曲目資訊。";
inline constexpr auto cycle_style_name = "切換展開頁樣式";
inline constexpr auto cycle_style_description = "在展開頁樣式「置中」與「靠左＋資訊卡」之間切換。";

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
