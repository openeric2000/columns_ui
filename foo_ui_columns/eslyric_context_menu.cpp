#include "pch.h"

#include "nocturne_text.h"

// 自編版新增（2026-11-18）：ESLyric 歌詞面板的右鍵選單只留常用的，其他收進「更多」。
//
// 使用者要的樣子（2026-11-18 定）：
//   儲存歌詞（＝原本的「儲存歌詞至 → 設定檔目錄」）、調整位移、開啟歌詞所在目錄、編輯歌詞、重載歌詞、搜尋歌詞...
//   ────
//   更多 >（其他全部，照原本順序、保留分隔線；偏好設定也在這裡）
//   正在播放 >
// - 「儲存歌詞」換成「設定檔目錄」那一項：原本的「儲存歌詞」走 ESLyric 內建的 <profile>\lyrics，還會用搜尋關鍵字多開一層
//   子資料夾（2026-11-18 實測：涼宮ハルヒ (C.V.平野綾) 存成 lyrics\涼宮ハルヒ\…），「設定檔目錄」才照設定頁的位置與檔名存。
//   原本那一項就不放了，免得「更多」裡又有一個同名但存到別處的「儲存歌詞」
// - ESLyric 沒有開關可以改選單，所以接手它匯入的 TrackPopupMenu（見檔尾）：選單顯示之前就地重排。
//   只搬動項目、不改 ID，按下去的還是 ESLyric 自己的功能
// - 用選單文字認項目（ESLyric 設成繁中）：認不出來的項目照原本的位置留著；連「搜尋歌詞」「儲存歌詞至」都認不出來
//   （ESLyric 換了語言或改版）就整個不動

namespace cui::eslyric_context_menu {

namespace {

// 標在「更多」這一項上，同一個選單再拿來顯示時就知道排過了
constexpr ULONG_PTR more_item_marker = 0x4e6f4d6f;

struct Item {
    MENUITEMINFOW info{};
    std::wstring text;
    std::wstring key;
};

// 比對用的文字：拿掉快速鍵（& 與 Tab 後面）、結尾的 ... 或 …
std::wstring normalise(std::wstring text)
{
    if (const auto tab = text.find(L'\t'); tab != std::wstring::npos)
        text.resize(tab);
    std::erase(text, L'&');
    while (!text.empty() && (text.back() == L'.' || text.back() == L'…' || text.back() == L' '))
        text.pop_back();
    return text;
}

std::vector<Item> read_items(HMENU menu)
{
    std::vector<Item> items;
    const auto count = GetMenuItemCount(menu);
    for (int index = 0; index < count; ++index) {
        Item item;
        wchar_t buffer[256]{};
        item.info.cbSize = sizeof(item.info);
        item.info.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_SUBMENU | MIIM_STRING | MIIM_DATA | MIIM_BITMAP
            | MIIM_CHECKMARKS;
        item.info.dwTypeData = buffer;
        item.info.cch = static_cast<UINT>(std::size(buffer));
        if (!GetMenuItemInfoW(menu, index, TRUE, &item.info))
            return {};
        item.text = buffer;
        item.key = normalise(item.text);
        items.emplace_back(std::move(item));
    }
    return items;
}

bool is_separator(const Item& item)
{
    return (item.info.fType & MFT_SEPARATOR) != 0;
}

void insert(HMENU menu, Item& item)
{
    auto info = item.info;
    info.dwTypeData = item.text.data();
    info.cch = static_cast<UINT>(item.text.size());
    if (is_separator(item))
        info.fMask &= ~MIIM_STRING;
    InsertMenuItemW(menu, GetMenuItemCount(menu), TRUE, &info);
}

// 「儲存歌詞至」子選單裡「設定檔目錄」那一項的 ID
std::optional<UINT> find_configured_directory_id(const Item& save_to)
{
    if (!save_to.info.hSubMenu)
        return {};
    for (auto& item : read_items(save_to.info.hSubMenu))
        if (!is_separator(item) && !item.info.hSubMenu && item.key == L"設定檔目錄")
            return item.info.wID;
    return {};
}

void rearrange(HMENU menu)
{
    // 用位置通知（MNS_NOTIFYBYPOS）的選單搬了位置就會錯，不動
    MENUINFO menu_info{sizeof(menu_info), MIM_STYLE};
    if (GetMenuInfo(menu, &menu_info) && (menu_info.dwStyle & MNS_NOTIFYBYPOS))
        return;

    auto items = read_items(menu);
    if (items.empty())
        return;

    const auto find = [&items](std::wstring_view key) -> Item* {
        for (auto& item : items)
            if (!is_separator(item) && item.key == key)
                return &item;
        return nullptr;
    };

    for (auto& item : items)
        if (item.info.dwItemData == more_item_marker)
            return;

    auto* search = find(L"搜尋歌詞");
    auto* save_to = find(L"儲存歌詞至");
    if (!search || !save_to)
        return;

    auto* save = find(L"儲存歌詞");
    const auto configured_directory_id = find_configured_directory_id(*save_to);

    constexpr std::wstring_view kept_keys[]{L"調整位移", L"開啟歌詞所在目錄", L"編輯歌詞", L"重載歌詞", L"搜尋歌詞"};
    auto* now_playing = find(L"正在播放");

    std::vector<Item*> top;
    std::vector<Item*> more;

    // 第一項：儲存歌詞。找得到「設定檔目錄」就借它的 ID；找不到就留原本那一項
    std::optional<Item> save_replacement;
    if (save && configured_directory_id) {
        save_replacement = *save;
        save_replacement->info.wID = *configured_directory_id;
        save_replacement->info.hSubMenu = nullptr;
        top.push_back(&*save_replacement);
    } else if (save) {
        top.push_back(save);
    }

    for (const auto key : kept_keys)
        if (auto* item = find(key))
            top.push_back(item);

    for (auto& item : items) {
        if (&item == save || &item == now_playing)
            continue;
        if (std::ranges::find(top, &item) != top.end())
            continue;
        // 分隔線：開頭、連續兩條都不要
        if (is_separator(item) && (more.empty() || is_separator(*more.back())))
            continue;
        more.push_back(&item);
    }
    while (!more.empty() && is_separator(*more.back()))
        more.pop_back();

    if (more.empty())
        return;

    const auto more_menu = CreatePopupMenu();
    if (!more_menu)
        return;

    // 先把原本的項目全部拆下來（RemoveMenu 不會毀掉子選單），再照新的順序放回去
    for (int index = GetMenuItemCount(menu) - 1; index >= 0; --index)
        RemoveMenu(menu, index, MF_BYPOSITION);

    for (auto* item : top)
        insert(menu, *item);

    Item separator;
    separator.info.cbSize = sizeof(separator.info);
    separator.info.fMask = MIIM_FTYPE;
    separator.info.fType = MFT_SEPARATOR;
    insert(menu, separator);

    for (auto* item : more)
        insert(more_menu, *item);

    Item more_item;
    more_item.text = nocturne_text::eslyric_menu_more;
    more_item.info.cbSize = sizeof(more_item.info);
    more_item.info.fMask = MIIM_STRING | MIIM_SUBMENU | MIIM_DATA;
    more_item.info.hSubMenu = more_menu;
    more_item.info.dwItemData = more_item_marker;
    insert(menu, more_item);

    if (now_playing)
        insert(menu, *now_playing);

    // 原本的「儲存歌詞」（沒放回選單）是單純的指令項目，沒有子選單要收
}

// ESLyric 叫 TrackPopupMenu 時帶了 TPM_NONOTIFY（2026-11-18 在 219 實測：選單開了，掛在主執行緒的 WH_CALLWNDPROC
// 完全收不到 WM_INITMENUPOPUP），所以改接 ESLyric 匯入表裡的 USER32!TrackPopupMenu：選單交給系統顯示之前先重排
using TrackPopupMenuFn = BOOL(WINAPI*)(HMENU, UINT, int, int, int, HWND, const RECT*);
TrackPopupMenuFn g_original{};
TrackPopupMenuFn* g_patched_slot{};

BOOL WINAPI track_popup_menu(HMENU menu, UINT flags, int x, int y, int reserved, HWND wnd, const RECT* rect)
{
    // 實測 flags 有 TPM_RETURNCMD：系統回傳的是被點那一項的 ID，搬到「更多」裡的項目照樣對得上
    rearrange(menu);
    return g_original(menu, flags, x, y, reserved, wnd, rect);
}

// 在模組的匯入表找 dll!function 那一格（找不到回 nullptr）
void** find_import_slot(HMODULE module, const char* dll, const char* function)
{
    auto* base = reinterpret_cast<BYTE*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return nullptr;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return nullptr;
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress)
        return nullptr;

    for (auto* descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
        descriptor->Name; ++descriptor) {
        if (_stricmp(reinterpret_cast<const char*>(base + descriptor->Name), dll) != 0)
            continue;
        if (!descriptor->OriginalFirstThunk)
            return nullptr;
        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                continue;
            const auto* by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(by_name->Name), function) == 0)
                return reinterpret_cast<void**>(&slots->u1.Function);
        }
    }
    return nullptr;
}

bool write_slot(void** slot, void* value)
{
    DWORD old_protect{};
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_protect))
        return false;
    *slot = value;
    VirtualProtect(slot, sizeof(void*), old_protect, &old_protect);
    return true;
}

class EslyricContextMenuInitQuit : public initquit {
public:
    void on_init() override
    {
        // ESLyric 是元件，fb2k 啟動時已經載入；沒裝就什麼都不做
        const auto module = GetModuleHandleW(L"foo_uie_eslyric.dll");
        if (!module)
            return;
        auto** slot = find_import_slot(module, "USER32.dll", "TrackPopupMenu");
        if (!slot)
            return;
        auto* original = reinterpret_cast<TrackPopupMenuFn>(*slot);
        if (write_slot(slot, reinterpret_cast<void*>(&track_popup_menu))) {
            g_original = original;
            g_patched_slot = reinterpret_cast<TrackPopupMenuFn*>(slot);
        }
    }

    void on_quit() override
    {
        // 結束時換回原本的函式（ESLyric 可能比我們晚卸載）
        if (g_patched_slot && g_original)
            write_slot(reinterpret_cast<void**>(g_patched_slot), reinterpret_cast<void*>(g_original));
        g_patched_slot = nullptr;
    }
};

initquit_factory_t<EslyricContextMenuInitQuit> eslyric_context_menu_initquit;

} // namespace

} // namespace cui::eslyric_context_menu
