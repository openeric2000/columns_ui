#include "pch.h"

#include "nocturne_default_stub.h"

// 公開版 v1.40：沒封面時自動顯示主題附的唱片圖（2026-11-09）
//
// 為什麼由 DLL 做：fb2k 的「沒封面佔位圖」（Preferences → Display → Album art 的 stub image）存在核心設定（config.sqlite），
// 不能放進 ZIP，所以公開版只能在啟動時代為設定。用 SDK 的 fb2k::configStore 讀寫，不碰 config.sqlite 檔案本身。
//
// 套用規則（依序，任一不成立就什麼都不做）：
// 1. 公開版建置（NOCTURNE_PUBLIC_BUILD）。個人環境（219／169／主環境／朋友版）各自設好了圖，不能被改。
// 2. profile 根目錄有 no-cover.png（標頭網頁的空清單也讀這個位置，兩邊一致）。沒有就不設，避免指到不存在的檔。
// 3. 佔位圖設定是空的，而且從沒套用過：寫入，並記下「已套用過」與「我們寫入的值」。
// 4. 設定值等於我們上次寫入的值、但跟現在該寫的值不同（例如安裝版改了 profile 位置）：更新成新值。
// 5. 其他（使用者自己指別張、使用者清空過）：不動。清空是明確的選擇，有「已套用過」記錄就不再套回去。
//
// 記錄值存在核心設定、不存在 Columns UI 的 .cfg：.cfg 會進公開版 ZIP，記在裡面的話乾淨設定檔會帶著「已套用過」出去，
// 使用者全新安裝時這個功能就整個失效。
//
// 路徑格式：攜帶版（profile 就是 fb2k 程式資料夾裡的 profile）寫相對路徑 .\profile\no-cover.png——fb2k 相對程式資料夾解析，
// 資料夾整個搬家也照樣有效（2026-11-07 實測）；安裝版（profile 在 %APPDATA%）寫完整路徑。

#ifdef NOCTURNE_PUBLIC_BUILD

namespace {

// fb2k 核心「沒封面佔位圖」設定的鍵（config.sqlite 的 configStrings 表，2026-11-07 查到）
constexpr auto stub_key = "albumArt.F1E66F4E-FE09-4B94-91A3-67C23ED1445E.stub";
// 我們自己的記錄值（跟細捲軸開關 nocturne.thinScrollbar 一樣放核心設定）
constexpr auto applied_key = "nocturne.defaultStub.applied";
constexpr auto written_key = "nocturne.defaultStub.value";
constexpr auto relative_value = ".\\profile\\no-cover.png";

std::wstring trim_slash(std::wstring path)
{
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/'))
        path.pop_back();
    return path;
}

std::wstring to_wide(const char* text)
{
    return std::wstring(pfc::stringcvt::string_wide_from_utf8(text).get_ptr());
}

std::string to_utf8(const std::wstring& text)
{
    return std::string(pfc::stringcvt::string_utf8_from_wide(text.c_str()).get_ptr());
}

// 這次該寫進佔位圖設定的值；profile 沒有 no-cover.png 時回傳空字串
std::string expected_stub_value()
{
    const pfc::string8 native_profile = ::filesystem::g_get_native_path(core_api::get_profile_path());
    const auto profile = trim_slash(to_wide(native_profile.c_str()));
    if (profile.empty())
        return {};

    const auto image = profile + L"\\no-cover.png";
    const auto attributes = GetFileAttributesW(image.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
        return {};

    // fb2k 程式資料夾（foobar2000.exe 所在）＋ \profile 就是 profile 的話＝攜帶版
    wchar_t exe_path[MAX_PATH * 2]{};
    const auto length = GetModuleFileNameW(nullptr, exe_path, static_cast<DWORD>(std::size(exe_path)));
    if (length > 0 && length < std::size(exe_path)) {
        std::wstring exe_dir(exe_path, length);
        const auto slash = exe_dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            exe_dir.resize(slash);
            if (CompareStringOrdinal(
                    (exe_dir + L"\\profile").c_str(), -1, profile.c_str(), -1, TRUE)
                == CSTR_EQUAL)
                return relative_value;
        }
    }

    return to_utf8(image);
}

} // namespace

void cui::nocturne::apply_default_stub_image()
{
    try {
        const auto expected = expected_stub_value();
        if (expected.empty())
            return;

        const auto store = fb2k::configStore::get();
        const std::string current = store->getConfigString(stub_key, "")->c_str();
        const bool applied = store->getConfigInt(applied_key, 0) != 0;
        const std::string written = store->getConfigString(written_key, "")->c_str();

        if (current.empty() && !applied) {
            store->setConfigString(stub_key, expected.c_str());
            store->setConfigInt(applied_key, 1);
            store->setConfigString(written_key, expected.c_str());
            console::print(
                fmt::format("Nocturne – no-cover image set to the bundled vinyl picture ({}).", expected).c_str());
        } else if (applied && !current.empty() && current == written && current != expected) {
            store->setConfigString(stub_key, expected.c_str());
            store->setConfigString(written_key, expected.c_str());
            console::print(fmt::format("Nocturne – no-cover image path updated ({}).", expected).c_str());
        }
    } catch (const std::exception& ex) {
        // 設不起來只是沒有唱片圖，不能影響啟動
        console::print(fmt::format("Nocturne – could not set the no-cover image: {}", ex.what()).c_str());
    }
}

#else

void cui::nocturne::apply_default_stub_image() {}

#endif
