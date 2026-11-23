#include "pch.h"

// 自編版新增（2026-11-18）：ESLyric 的「歌詞搜尋」視窗只蓋在 fb2k 上面，不再蓋住其他程式。
//
// ESLyric 開這個視窗時把它設成系統置頂（WS_EX_TOPMOST），擁有者是 fb2k 核心的隱藏視窗，所以切到別的程式時它還浮在最上面。
// 2026-11-18 在 219 實測：從外部把擁有者改成 fb2k 主視窗、拿掉置頂，它就跟 fb2k 設定視窗一樣只蓋在 fb2k 上面，
// 之後再搜尋 ESLyric 也不會把置頂加回去。ESLyric 沒有開關可以關，所以在 fb2k 行程裡攔：視窗一顯示就照這樣改。
// - 只認搜尋視窗：ESLyric 模組的對話框、裡面有結果清單（1018）和進度條（1017）。歌詞編輯器有自己的「窗口置頂」選項，不能動
// - 主視窗縮到最小或隱藏時（例如從桌面歌詞開搜尋）不改：掛在縮小的主視窗底下，搜尋視窗會跟著看不見
// - 保險：之後 ESLyric 若又要把它設成置頂，改成只排到最前面

namespace cui::eslyric_search_owner {

namespace {

constexpr wchar_t eslyric_module_name[] = L"foo_uie_eslyric.dll";
constexpr wchar_t dialog_class_name[] = L"#32770";
constexpr int result_list_id = 1018;
constexpr int progress_bar_id = 1017;
constexpr UINT_PTR subclass_id = 0x4e7d;

HWINEVENTHOOK g_hook{};
bool g_alive{};

bool has_class(HWND wnd, const wchar_t* name)
{
    wchar_t buffer[64]{};
    return GetClassNameW(wnd, buffer, static_cast<int>(std::size(buffer))) > 0 && wcscmp(buffer, name) == 0;
}

bool is_search_dialog(HWND wnd)
{
    if (!has_class(wnd, dialog_class_name))
        return false;
    // 沒載入 ESLyric 就不會有它的視窗
    const auto module = GetModuleHandleW(eslyric_module_name);
    if (!module || reinterpret_cast<HMODULE>(GetWindowLongPtrW(wnd, GWLP_HINSTANCE)) != module)
        return false;
    const auto list = GetDlgItem(wnd, result_list_id);
    const auto progress = GetDlgItem(wnd, progress_bar_id);
    return list && progress && has_class(progress, PROGRESS_CLASSW);
}

LRESULT CALLBACK dialog_subclass(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR)
{
    switch (msg) {
    case WM_WINDOWPOSCHANGING: {
        auto* pos = reinterpret_cast<WINDOWPOS*>(lp);
        if (!(pos->flags & SWP_NOZORDER) && pos->hwndInsertAfter == HWND_TOPMOST)
            pos->hwndInsertAfter = HWND_TOP;
        break;
    }
    case WM_NCDESTROY:
        RemoveWindowSubclass(wnd, dialog_subclass, id);
        break;
    }
    return DefSubclassProc(wnd, msg, wp, lp);
}

void adopt(HWND wnd)
{
    if (!is_search_dialog(wnd))
        return;
    const auto main_window = core_api::get_main_window();
    if (!main_window || !IsWindowVisible(main_window) || IsIconic(main_window))
        return;
    // 子類別只能在建立那個視窗的執行緒上掛（ESLyric 在主執行緒開，跟這個事件的執行緒相同）
    if (GetWindowThreadProcessId(wnd, nullptr) != GetCurrentThreadId())
        return;

    if (reinterpret_cast<HWND>(GetWindowLongPtrW(wnd, GWLP_HWNDPARENT)) != main_window)
        SetWindowLongPtrW(wnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(main_window));
    if (GetWindowLongPtrW(wnd, GWL_EXSTYLE) & WS_EX_TOPMOST)
        SetWindowPos(wnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    DWORD_PTR existing{};
    if (!GetWindowSubclass(wnd, dialog_subclass, subclass_id, &existing))
        SetWindowSubclass(wnd, dialog_subclass, subclass_id, 0);
}

void CALLBACK on_win_event(HWINEVENTHOOK, DWORD, HWND wnd, LONG object_id, LONG child_id, DWORD, DWORD)
{
    if (!g_alive || !wnd || object_id != OBJID_WINDOW || child_id != CHILDID_SELF)
        return;
    adopt(wnd);
}

class EslyricSearchOwnerInitQuit : public initquit {
public:
    void on_init() override
    {
        g_alive = true;
        g_hook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, on_win_event, GetCurrentProcessId(), 0,
            WINEVENT_OUTOFCONTEXT);
    }

    void on_quit() override
    {
        g_alive = false;
        if (g_hook) {
            UnhookWinEvent(g_hook);
            g_hook = nullptr;
        }
    }
};

initquit_factory_t<EslyricSearchOwnerInitQuit> eslyric_search_owner_initquit;

} // namespace

} // namespace cui::eslyric_search_owner
