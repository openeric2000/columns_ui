#include "pch.h"

#include "popup_dismiss.h"

// 卡片型彈出視窗共用的自動關閉規則，說明見 popup_dismiss.h

namespace cui::popup_dismiss {

namespace {

// 跟卡片自己的計時器（1、2…）錯開
constexpr UINT_PTR watch_timer_id = 0x7d15;
constexpr UINT watch_interval_ms = 250;

struct Entry {
    HWND wnd{};
    std::function<bool()> still_open;
    ULONGLONG* outside_close_tick{};
};

// 同時開著的卡片（通常只有一張）；有卡片時掛一個這個執行緒的滑鼠 hook，全部關掉就拿掉
std::vector<Entry>& entries()
{
    static std::vector<Entry> list;
    return list;
}

HHOOK& mouse_hook()
{
    static HHOOK hook{};
    return hook;
}

LRESULT CALLBACK mouse_hook_proc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION) {
        switch (wp) {
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_NCLBUTTONDOWN:
        case WM_NCRBUTTONDOWN:
        case WM_NCMBUTTONDOWN: {
            const auto* info = reinterpret_cast<const MOUSEHOOKSTRUCT*>(lp);
            for (const auto& entry : entries()) {
                if (info->hwnd == entry.wnd)
                    continue;
                if (entry.outside_close_tick)
                    *entry.outside_close_tick = GetTickCount64();
                PostMessage(entry.wnd, WM_CLOSE, 0, 0);
            }
            break;
        }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

Entry* find(HWND wnd)
{
    auto& list = entries();
    const auto iter = std::ranges::find(list, wnd, &Entry::wnd);
    return iter == list.end() ? nullptr : &*iter;
}

void detach(HWND wnd)
{
    KillTimer(wnd, watch_timer_id);
    std::erase_if(entries(), [wnd](const Entry& entry) { return entry.wnd == wnd; });
    if (entries().empty() && mouse_hook()) {
        UnhookWindowsHookEx(mouse_hook());
        mouse_hook() = nullptr;
    }
}

} // namespace

void attach(HWND wnd, std::function<bool()> still_open, ULONGLONG* outside_close_tick)
{
    detach(wnd);
    entries().push_back({wnd, std::move(still_open), outside_close_tick});
    if (!mouse_hook())
        mouse_hook() = SetWindowsHookEx(WH_MOUSE, mouse_hook_proc, nullptr, GetCurrentThreadId());
    SetTimer(wnd, watch_timer_id, watch_interval_ms, nullptr);
}

bool handle_message(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ACTIVATE:
        // 原本兩張卡都是自己處理完就 return 0（不交給 DefWindowProc），這裡照舊
        if (!find(wnd))
            return false;
        if (LOWORD(wp) == WA_INACTIVE)
            PostMessage(wnd, WM_CLOSE, 0, 0);
        return true;
    case WM_TIMER: {
        if (wp != watch_timer_id)
            return false;
        const auto* entry = find(wnd);
        if (!entry)
            return true;
        DWORD process{};
        GetWindowThreadProcessId(GetForegroundWindow(), &process);
        if (process != GetCurrentProcessId() || (entry->still_open && !entry->still_open()))
            PostMessage(wnd, WM_CLOSE, 0, 0);
        return true;
    }
    case WM_NCDESTROY:
        detach(wnd);
        return false;
    }
    return false;
}

} // namespace cui::popup_dismiss
