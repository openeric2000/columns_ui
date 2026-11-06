#pragma once

namespace cui::theme_font {

// 自編版新增：View/Layout/主題字型…
void open_theme_font_dialog(HWND parent_wnd);

// ---------------------------------------------------------------------------------------------------------------
// 字型分頁（Preferences → Tools → Nocturne →「字型」）用：分區塊換字型（2026-11-05）
// 區塊編號 0～5：播放清單、分頁標籤、資料夾樹、其餘介面文字（以上四個是 Columns UI 字型項目，字族與大小都能換、馬上生效）、
// 播放列與展開頁（main.xml）、專輯標頭（index.html）（以上兩個是主題檔，只能換字族、要重開 fb2k 才生效）。
// 傳 std::nullopt 代表「全部區塊」，等於 View → Layout → 主題字型…（一次換掉 5 個項目與 2 個主題檔）。
// ---------------------------------------------------------------------------------------------------------------

// 系統裡可選的字族（去掉符號字型；第一次呼叫時列舉，之後沿用）
size_t font_family_count();
const std::wstring& font_family_name(size_t index);

// 整體大小的選項（小、標準、大、特大）
size_t size_option_count();
const wchar_t* size_option_label(size_t index);
size_t default_size_option();

size_t font_region_count();
// 該區塊能不能調大小（兩個主題檔區塊不能）
bool font_region_has_size(size_t region);

// 目前字族在 font_family_name 清單裡的位置；找不到（字型被移除、主題檔讀不到）傳空
std::optional<size_t> current_family_index(std::optional<size_t> region);
// 目前的大小選項；對不上任何一個就傳「標準」。主題檔區塊沒有大小，傳「標準」
size_t current_size_index(std::optional<size_t> region);

struct FontApplyResult {
    // 主題檔（播放列、專輯標頭）有被改：要重開 fb2k 才生效
    bool theme_file_changed{};
    // 沒寫成功的主題檔（要請使用者自己用記事本改），已排好版的文字；空的就是都成功
    std::string failures;
    // 整個套用失敗的原因（例如字型沒有可用樣式）；沒有就是成功
    std::optional<std::string> error;
};

// 把 family_index 這個字族（與 size_index 這個大小，主題檔區塊忽略）套到 region
FontApplyResult apply_font(std::optional<size_t> region, size_t family_index, size_t size_index);

} // namespace cui::theme_font
