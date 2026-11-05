#include "pch.h"

#include <filesystem>
#include <fstream>
#include <span>

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

// 依某個字型項目目前的字級找最接近的整體大小，對不上就用「標準」
size_t get_current_size_option_index(const ThemeFontItem& item)
{
    const auto entry = g_font_manager_data.find_by_id(item.id);
    const auto scale = gsl::narrow_cast<float>(g_font_manager_data.resolve_font_description(entry).point_size_tenths)
        / (item.default_point_size * 10.0f);

    const auto iter = std::ranges::min_element(
        size_options, {}, [scale](const SizeOption& option) { return std::abs(option.scale - scale); });

    if (std::abs(iter->scale - scale) > 0.05f)
        return default_size_option_index;

    return gsl::narrow<size_t>(std::distance(size_options.begin(), iter));
}

// 主題對話框用：照「Playlist view: Items」目前的字級判斷整體大小
size_t get_current_size_option_index()
{
    return get_current_size_option_index(theme_font_items[size_reference_item_index]);
}

// 把字型套到 items 這幾個 Columns UI 字型項目（整體大小＝預設字級 × scale）
void apply_columns_ui_fonts(std::span<const ThemeFontItem* const> items, const uih::direct_write::FontFamily& family,
    const uih::direct_write::Font& font, float scale)
{
    const auto context = uih::direct_write::Context::s_create();

    for (const auto* item_ptr : items) {
        const auto& item = *item_ptr;
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

        std::array<const ThemeFontItem*, theme_font_items.size()> all_items{};
        std::ranges::transform(theme_font_items, all_items.begin(), [](const ThemeFontItem& item) { return &item; });
        apply_columns_ui_fonts(all_items, family, *font, size_option.scale);
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

// ---------------------------------------------------------------------------------------------------------------
// 分區塊換字型（字型分頁用，見 theme_font.h）
// ---------------------------------------------------------------------------------------------------------------
namespace {

constexpr size_t no_index = static_cast<size_t>(-1);

// 一個區塊對應的 Columns UI 字型項目（theme_font_items 的索引，最多兩個）與主題檔（theme_files 的索引）
struct Region {
    std::array<size_t, 2> item_indexes;
    size_t file_index;
};

constexpr std::array regions{
    Region{{3, no_index}, no_index}, // 播放清單：Playlist view: Items
    Region{{2, no_index}, no_index}, // 分頁標籤：Playlist tabs
    Region{{4, no_index}, no_index}, // 資料夾樹：Album list
    Region{{0, 1}, no_index}, // 其餘介面文字：Common (list items)、Common (labels)
    Region{{no_index, no_index}, 0}, // 播放列與展開頁：main.xml 的 txt 樣式
    Region{{no_index, no_index}, 1}, // 專輯標頭：index.html 的 --font
};

// 系統的字族清單。有意不釋放：裡面是 DirectWrite 的 COM 物件，在 DLL 卸載時才解構可能已經來不及
const std::vector<uih::direct_write::FontFamily>& cached_families()
{
    static const auto* families = [] {
        auto* result = new std::vector<uih::direct_write::FontFamily>();
        try {
            *result = uih::direct_write::Context::s_create()->get_font_families();
        }
        CATCH_LOG()
        std::erase_if(*result, [](const auto& family) { return family.is_symbol_font; });
        return result;
    }();

    return *families;
}

// 每個字族的英文名（寫進主題檔用的），跟 cached_families 同一個順序；取不到的是空字串
const std::vector<std::string>& cached_english_names()
{
    static const auto* names = [] {
        auto* result = new std::vector<std::string>();
        for (const auto& family : cached_families()) {
            try {
                result->push_back(mmh::to_utf8(get_english_family_name(family)));
            } catch (...) {
                result->emplace_back();
            }
        }
        return result;
    }();

    return *names;
}

std::filesystem::path get_profile_path()
{
    return mmh::to_utf16(::filesystem::g_get_native_path(core_api::get_profile_path()).c_str());
}

// 讀主題檔裡目前的字型名（跟 update_theme_file 用同一個樣式），並還原 escape_family_name 的跳脫
std::optional<std::string> read_theme_file_family(const ThemeFile& file)
{
    std::ifstream stream(get_profile_path() / file.relative_path, std::ios::binary);
    if (!stream)
        return {};

    const std::string content((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());

    std::smatch match;
    if (!std::regex_search(content, match, std::regex(file.pattern)))
        return {};

    std::string name = match[1].str();
    if (file.is_css) {
        std::string result;
        for (size_t i = 0; i < name.size(); ++i) {
            if (name[i] == '\\' && i + 1 < name.size())
                ++i;
            result += name[i];
        }
        return result;
    }

    for (const auto& [from, to] : {std::pair<std::string_view, char>{"&quot;", '"'}, {"&lt;", '<'}, {"&amp;", '&'}}) {
        for (size_t position = name.find(from); position != std::string::npos; position = name.find(from, position + 1))
            name.replace(position, from.size(), 1, to);
    }
    return name;
}

} // namespace

size_t font_family_count()
{
    return cached_families().size();
}

const std::wstring& font_family_name(size_t index)
{
    return cached_families().at(index).name;
}

size_t size_option_count()
{
    return size_options.size();
}

const wchar_t* size_option_label(size_t index)
{
    return size_options.at(index).label;
}

size_t default_size_option()
{
    return default_size_option_index;
}

size_t font_region_count()
{
    return regions.size();
}

bool font_region_has_size(size_t region)
{
    return regions.at(region).item_indexes[0] != no_index;
}

std::optional<size_t> current_family_index(std::optional<size_t> region)
{
    const auto& families = cached_families();

    try {
        // 主題檔區塊：讀檔裡的英文名，跟每個字族的英文名比對
        if (region && regions.at(*region).item_indexes[0] == no_index) {
            const auto current = read_theme_file_family(theme_files[regions[*region].file_index]);
            if (!current)
                return {};

            const auto& english_names = cached_english_names();
            for (size_t index{}; index < families.size(); ++index) {
                if (english_names[index] == *current)
                    return index;
            }
            return {};
        }

        // Columns UI 字型項目：「全部」看第一個項目（Common list items），跟主題對話框一樣
        const auto item_index = region ? regions.at(*region).item_indexes[0] : size_t{0};
        const auto description = g_font_manager_data.resolve_font_description(
            g_font_manager_data.find_by_id(theme_font_items[item_index].id));
        const std::wstring name = !description.typographic_family_name.empty()
            ? description.typographic_family_name
            : (description.wss ? description.wss->family_name : std::wstring{});

        for (size_t index{}; index < families.size(); ++index) {
            if (families[index].name == name)
                return index;
        }
    }
    CATCH_LOG()

    return {};
}

size_t current_size_index(std::optional<size_t> region)
{
    if (region && regions.at(*region).item_indexes[0] == no_index)
        return default_size_option_index;

    const auto item_index = region ? regions[*region].item_indexes[0] : size_reference_item_index;
    try {
        return get_current_size_option_index(theme_font_items[item_index]);
    }
    CATCH_LOG()

    return default_size_option_index;
}

FontApplyResult apply_font(std::optional<size_t> region, size_t family_index, size_t size_index)
{
    FontApplyResult result;
    const auto& families = cached_families();

    if (family_index >= families.size() || size_index >= size_options.size()) {
        result.error = nocturne_text::error_no_usable_style;
        return result;
    }

    const auto& family = families[family_index];

    try {
        const auto family_fonts = family.fonts();
        const auto font = find_regular_font(family_fonts);

        if (!font) {
            result.error = nocturne_text::error_no_usable_style;
            return result;
        }

        std::vector<const ThemeFontItem*> items;
        std::vector<const ThemeFile*> files;

        if (!region) {
            for (const auto& item : theme_font_items)
                items.push_back(&item);
            for (const auto& file : theme_files)
                files.push_back(&file);
        } else {
            const auto& definition = regions.at(*region);
            for (const auto item_index : definition.item_indexes) {
                if (item_index != no_index)
                    items.push_back(&theme_font_items[item_index]);
            }
            if (definition.file_index != no_index)
                files.push_back(&theme_files[definition.file_index]);
        }

        if (!items.empty())
            apply_columns_ui_fonts(items, family, *font, size_options[size_index].scale);

        if (!files.empty()) {
            const auto english_name = mmh::to_utf8(get_english_family_name(family));
            const auto profile_path = get_profile_path();

            for (const auto* file : files) {
                const auto path = profile_path / file->relative_path;
                const auto update = update_theme_file(path, *file, english_name);

                result.theme_file_changed = result.theme_file_changed || update.changed;

                if (update.error)
                    result.failures += fmt::format(nocturne_text::failure_item, mmh::to_utf8(path.native()), *update.error);
            }
        }
    } catch (const std::exception& ex) {
        result.error = fmt::format(nocturne_text::error_change_failed, ex.what());
    }

    return result;
}

} // namespace cui::theme_font
