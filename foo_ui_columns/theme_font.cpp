#include "pch.h"

#include <filesystem>
#include <fstream>

#include "theme_font.h"

#include "config_appearance.h"
#include "dark_mode_dialog.h"
#include "nocturne_text.h"

// 自編版新增：View/Layout/主題字型…
// 選一個字型，一次換掉主題用到的 5 個 Columns UI 字型項目，以及兩個主題檔（ESLyric 佈局 main.xml、標頭 index.html）
// - 字級＝主題預設字級 × 選的整體大小，所以重複使用不會越來越大，選「標準」就回到原本的設計
// - 整體大小只影響這 5 項（Columns UI 會自己調列高）；播放列與標頭是固定高度，主題檔裡的字級不動
// - 粗細固定用最接近一般（400）的樣式：偏好設定字型頁換字族時，新字族沒有同名樣式就會選清單第一個（常是 Light）
// - 直接改字型項目並寫回 wss，不經過 FCL 匯入（Entry::import 不會寫回 wss）
// - 歌詞字型存在 ESLyric 面板自己的設定裡（格式不公開），這裡不動，請使用者在歌詞右鍵選單另外設定

namespace cui::theme_font {

namespace {

struct ThemeFontItem {
    GUID id;
    float default_point_size;
};

// 主題的 5 個字型項目與預設字級
constexpr std::array theme_font_items{
    ThemeFontItem{fonts::items_font_id, 10.5f}, // Common (list items)
    ThemeFontItem{fonts::labels_font_id, 10.5f}, // Common (labels)
    ThemeFontItem{{0x942c36a4, 0x4e28, 0x4cea, {0x96, 0x44, 0xf2, 0x23, 0xc9, 0xa8, 0x38, 0xec}}, 11.0f}, // Playlist tabs
    ThemeFontItem{{0x19f8e0b3, 0xe822, 0x4f07, {0xb2, 0x00, 0xd4, 0xa6, 0x7e, 0x48, 0x72, 0xf9}},
        13.5f}, // Playlist view: Items
    ThemeFontItem{{0x06b856cc, 0x86e7, 0x4459, {0xa7, 0x5c, 0x2d, 0xab, 0x5b, 0x33, 0xb8, 0xbb}}, 12.0f}, // Album list
};

// 用來判斷目前是哪個整體大小的項目（Playlist view: Items）
constexpr size_t size_reference_item_index = 3;

struct SizeOption {
    const wchar_t* label;
    float scale;
};

constexpr size_t default_size_option_index = 1;

constexpr std::array size_options{
    SizeOption{nocturne_text::size_small, 0.9f},
    SizeOption{nocturne_text::size_standard, 1.0f},
    SizeOption{nocturne_text::size_large, 1.15f},
    SizeOption{nocturne_text::size_extra_large, 1.3f},
};

struct Selection {
    size_t family_index{};
    size_t size_option_index{default_size_option_index};
};

struct ThemeFile {
    const wchar_t* relative_path;
    const char* pattern; // 第 1 個群組是要換掉的字型名稱
    bool is_css;
};

// main.xml 換 txt 樣式的字型；index.html 只換 --font 的第一個字型，後面的備用字型不動
constexpr std::array theme_files{
    ThemeFile{LR"(eslyric-data\layout\fb2k-modern\main.xml)", R"re(<props id="txt" font-family="([^"]*)")re", false},
    ThemeFile{LR"(webview\default\index.html)", R"re(--font:\s*"([^"]*)")re", true},
};

const uih::direct_write::Font* find_regular_font(const std::vector<uih::direct_write::Font>& fonts)
{
    const auto distance = [](const uih::direct_write::Font& font) {
        return (font.style == DWRITE_FONT_STYLE_NORMAL ? 0 : 100'000)
            + std::abs(static_cast<int>(font.stretch) - static_cast<int>(DWRITE_FONT_STRETCH_NORMAL)) * 1'000
            + std::abs(static_cast<int>(font.weight) - static_cast<int>(DWRITE_FONT_WEIGHT_REGULAR));
    };

    const auto iter = std::ranges::min_element(fonts, {}, distance);
    return iter == fonts.end() ? nullptr : &*iter;
}

// 寫進主題檔的用英文名，換到別的語系也找得到
std::wstring get_english_family_name(const uih::direct_write::FontFamily& family)
{
    wil::com_ptr<IDWriteLocalizedStrings> names;
    THROW_IF_FAILED(family.family->GetFamilyNames(&names));

    uint32_t index{};
    BOOL exists{};
    THROW_IF_FAILED(names->FindLocaleName(L"en-us", &index, &exists));
    if (!exists)
        index = 0;

    uint32_t length{};
    THROW_IF_FAILED(names->GetStringLength(index, &length));

    std::wstring name(length + 1, L'\0');
    THROW_IF_FAILED(names->GetString(index, name.data(), length + 1));
    name.resize(length);
    return name;
}

std::wstring get_current_family_name()
{
    const auto description = g_font_manager_data.resolve_font_description(g_font_manager_data.m_common_items_entry);

    if (!description.typographic_family_name.empty())
        return description.typographic_family_name;

    return description.wss ? description.wss->family_name : std::wstring{};
}

// 依目前的字級找最接近的整體大小，對不上就用「標準」
size_t get_current_size_option_index()
{
    const auto& item = theme_font_items[size_reference_item_index];
    const auto entry = g_font_manager_data.find_by_id(item.id);
    const auto scale = gsl::narrow_cast<float>(g_font_manager_data.resolve_font_description(entry).point_size_tenths)
        / (item.default_point_size * 10.0f);

    const auto iter = std::ranges::min_element(
        size_options, {}, [scale](const SizeOption& option) { return std::abs(option.scale - scale); });

    if (std::abs(iter->scale - scale) > 0.05f)
        return default_size_option_index;

    return gsl::narrow<size_t>(std::distance(size_options.begin(), iter));
}

void apply_columns_ui_fonts(const uih::direct_write::FontFamily& family, const uih::direct_write::Font& font, float scale)
{
    const auto context = uih::direct_write::Context::s_create();

    for (const auto& item : theme_font_items) {
        const auto entry = g_font_manager_data.find_by_id(item.id);

        entry->font_mode = fonts::FontMode::Custom;

        auto& description = entry->font_description;
        description.wss
            = uih::direct_write::WeightStretchStyle{family.wss_name(font), font.weight, font.stretch, font.style};
        description.typographic_family_name = family.typographic_name();
        description.axis_values = font.axis_values;
        description.log_font = context->create_log_font(font.font);
        description.set_point_size(item.default_point_size * scale);
    }

    g_font_manager_data.dispatch_all_fonts_changed();
}

std::string escape_family_name(std::string_view name, bool is_css)
{
    std::string result;

    for (const auto ch : name) {
        if (is_css) {
            if (ch == '"' || ch == '\\')
                result += '\\';
            result += ch;
            continue;
        }

        switch (ch) {
        case '&':
            result += "&amp;";
            break;
        case '"':
            result += "&quot;";
            break;
        case '<':
            result += "&lt;";
            break;
        default:
            result += ch;
        }
    }

    return result;
}

struct FileUpdateResult {
    bool changed{};
    std::optional<std::string> error;
};

// 字型名稱本來就一樣時不寫檔（changed 為 false），失敗時 error 是原因
FileUpdateResult update_theme_file(
    const std::filesystem::path& path, const ThemeFile& file, std::string_view family_name)
{
    std::string content;
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
            return {.error = nocturne_text::error_file_not_found};

        content.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    }

    std::smatch match;
    if (!std::regex_search(content, match, std::regex(file.pattern)))
        return {.error = nocturne_text::error_font_setting_not_found};

    const auto position = gsl::narrow<size_t>(match.position(1));
    const auto length = gsl::narrow<size_t>(match.length(1));
    const auto escaped_name = escape_family_name(family_name, file.is_css);

    if (content.compare(position, length, escaped_name) == 0)
        return {};

    content.replace(position, length, escaped_name);

    // 先寫暫存檔再取代，寫到一半失敗時原檔不會壞掉
    auto temp_path = path;
    temp_path += L".tmp";
    {
        std::ofstream stream(temp_path, std::ios::binary | std::ios::trunc);
        stream.write(content.data(), gsl::narrow<std::streamsize>(content.size()));
        stream.close();

        if (!stream)
            return {.error = nocturne_text::error_write_failed};
    }

    if (!MoveFileExW(temp_path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code error_code;
        std::filesystem::remove(temp_path, error_code);
        return {.error = nocturne_text::error_write_failed};
    }

    return {.changed = true};
}

std::optional<Selection> run_dialog(HWND parent_wnd, const std::vector<uih::direct_write::FontFamily>& families)
{
    std::optional<Selection> selection;

    dark::modal_dialog_box(IDD_THEME_FONT,
        {.button_ids = {IDOK, IDCANCEL},
            .combo_box_ids = {IDC_THEME_FONT_FAMILY, IDC_THEME_FONT_SIZE},
            .last_button_id = IDCANCEL},
        parent_wnd, [&](HWND wnd, UINT msg, WPARAM wp, LPARAM lp) -> INT_PTR {
            switch (msg) {
            case WM_INITDIALOG: {
                // 對話框的文字依語言填入（.rc 裡只放版面）
                SetWindowText(wnd, nocturne_text::theme_font_title_w);
                SetDlgItemText(wnd, IDC_THEME_FONT_INTRO, nocturne_text::theme_font_intro);
                SetDlgItemText(wnd, IDC_THEME_FONT_FAMILY_LABEL, nocturne_text::theme_font_family_label);
                SetDlgItemText(wnd, IDC_THEME_FONT_SIZE_LABEL, nocturne_text::theme_font_size_label);
                SetDlgItemText(wnd, IDC_THEME_FONT_LYRICS_NOTE, nocturne_text::theme_font_lyrics_note);
                SetDlgItemText(wnd, IDOK, nocturne_text::ok_text);
                SetDlgItemText(wnd, IDCANCEL, nocturne_text::cancel_text);

                const auto family_combo = GetDlgItem(wnd, IDC_THEME_FONT_FAMILY);
                const auto current_name = get_current_family_name();

                for (size_t index{}; index < families.size(); ++index) {
                    ComboBox_AddString(family_combo, families[index].name.c_str());

                    if (families[index].name == current_name)
                        ComboBox_SetCurSel(family_combo, gsl::narrow<int>(index));
                }

                const auto size_combo = GetDlgItem(wnd, IDC_THEME_FONT_SIZE);
                for (const auto& option : size_options)
                    ComboBox_AddString(size_combo, option.label);

                ComboBox_SetCurSel(size_combo, gsl::narrow<int>(get_current_size_option_index()));
                return TRUE;
            }
            case WM_COMMAND:
                switch (LOWORD(wp)) {
                case IDOK: {
                    const auto family_index = ComboBox_GetCurSel(GetDlgItem(wnd, IDC_THEME_FONT_FAMILY));
                    const auto size_index = ComboBox_GetCurSel(GetDlgItem(wnd, IDC_THEME_FONT_SIZE));
                    if (family_index == CB_ERR || size_index == CB_ERR)
                        return TRUE;

                    selection = Selection{gsl::narrow<size_t>(family_index), gsl::narrow<size_t>(size_index)};
                    EndDialog(wnd, IDOK);
                    return TRUE;
                }
                case IDCANCEL:
                    EndDialog(wnd, IDCANCEL);
                    return TRUE;
                }
                break;
            }
            return FALSE;
        });

    return selection;
}

} // namespace

void open_theme_font_dialog(HWND parent_wnd)
{
    constexpr auto title = nocturne_text::theme_font_title;

    std::vector<uih::direct_write::FontFamily> families;
    try {
        families = uih::direct_write::Context::s_create()->get_font_families();
    }
    CATCH_LOG()

    std::erase_if(families, [](const auto& family) { return family.is_symbol_font; });

    if (families.empty()) {
        dark::modal_info_box(
            parent_wnd, title, nocturne_text::error_no_font_list, uih::InfoBoxType::Error, uih::InfoBoxModalType::OK);
        return;
    }

    const auto selection = run_dialog(parent_wnd, families);
    if (!selection)
        return;

    const auto& family = families[selection->family_index];
    const auto& size_option = size_options[selection->size_option_index];
    std::string file_family_name;

    try {
        const auto family_fonts = family.fonts();
        const auto font = find_regular_font(family_fonts);

        if (!font) {
            dark::modal_info_box(parent_wnd, title, nocturne_text::error_no_usable_style, uih::InfoBoxType::Error,
                uih::InfoBoxModalType::OK);
            return;
        }

        apply_columns_ui_fonts(family, *font, size_option.scale);
        file_family_name = mmh::to_utf8(get_english_family_name(family));
    } catch (const std::exception& ex) {
        const auto message = fmt::format(nocturne_text::error_change_failed, ex.what());
        dark::modal_info_box(parent_wnd, title, message.c_str(), uih::InfoBoxType::Error, uih::InfoBoxModalType::OK);
        return;
    }

    const std::filesystem::path profile_path
        = mmh::to_utf16(::filesystem::g_get_native_path(core_api::get_profile_path()).c_str());

    std::string failures;
    bool is_any_file_changed{};
    for (const auto& file : theme_files) {
        const auto path = profile_path / file.relative_path;
        const auto result = update_theme_file(path, file, file_family_name);

        is_any_file_changed = is_any_file_changed || result.changed;

        if (result.error)
            failures += fmt::format(nocturne_text::failure_item, mmh::to_utf8(path.native()), *result.error);
    }

    // 主題檔沒有變（只改了大小）時，Columns UI 的項目已經即時套用，不用重開
    if (!is_any_file_changed && failures.empty()) {
        const auto message = fmt::format(
            nocturne_text::applied_message, mmh::to_utf8(family.name), mmh::to_utf8(size_option.label));
        dark::modal_info_box(
            parent_wnd, title, message.c_str(), uih::InfoBoxType::Information, uih::InfoBoxModalType::OK);
        return;
    }

    auto message
        = fmt::format(nocturne_text::restart_message, mmh::to_utf8(family.name), mmh::to_utf8(size_option.label));

    if (!failures.empty())
        message += fmt::format(nocturne_text::failures_message, failures);

    message += nocturne_text::restart_question;

    if (dark::modal_info_box(parent_wnd, title, message.c_str(),
            failures.empty() ? uih::InfoBoxType::Information : uih::InfoBoxType::Warning,
            uih::InfoBoxModalType::YesNo))
        standard_commands::main_restart();
}

} // namespace cui::theme_font
