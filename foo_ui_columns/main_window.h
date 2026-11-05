#pragma once

#include <thread>

#include "config_utils.h"
#include "fallback_menu.h"
#include "fb2k_keyboard.h"
#include "icons.h"

/** Main window UI control IDs */
constexpr auto ID_REBAR = 2100u;
constexpr auto ID_STATUS = 2002u;

/** Main window custom message numbers */
enum {
    MSG_SET_AOT = WM_USER + 3,
    MSG_UPDATE_TITLE,
    MSG_RUN_INITIAL_SETUP,
    MSG_SYSTEM_TRAY_ICON,
    MSG_CREATE_TASKBAR_BUTTONS,
    MSG_UPDATE_TASKBAR_BUTTONS,
    MSG_STARTUP_HOLD_TIMEOUT, // 開機優化 B：等太久，監看執行緒已經把視窗顯示出來了，通知主執行緒收尾
};

void on_show_status_change();
void on_show_status_pane_change();
void on_show_toolbars_change();

namespace taskbar_buttons {
enum {
    ID_FIRST = 667,
    ID_STOP = ID_FIRST,
    ID_PREV,
    ID_PLAY_OR_PAUSE,
    ID_NEXT,
    ID_RAND
};
} // namespace taskbar_buttons

namespace cui {

extern fbh::ConfigBool lock_main_window_size;

// 自編版新增（開機優化 B，2026-11-06）：啟動時先不顯示主視窗，等網頁面板回報「好了」再顯示，免得一打開看到空殼畫面
// （視窗先出現、專輯樹是空的、網頁面板只有 WebView2 字樣）。預設開；關掉就跟原本一樣，視窗建好立刻顯示
extern fbh::ConfigBool startup_hold_until_ready;
// 等太久（沒有網頁面板、網頁壞掉、機器很慢）就直接顯示：從行程啟動起算（使用者按下去之後最多等這麼久），
// 視窗建立時已經剩不到 startup_hold_min_ms 就不等了（機器慢到這個地步，等也只是讓空殼晚幾秒出現）
inline constexpr DWORD startup_hold_timeout_ms = 3000;
inline constexpr DWORD startup_hold_min_ms = 400;

class MainWindow {
public:
    HWND initialise(user_interface::HookProc_t hook, bool is_hidden = false);

    // 結束「先不顯示」的狀態，讓視窗的畫面出現。
    // 網頁回報「好了」、逾時、第二次啟動轉送時都走這裡；沒在延後的時候什麼都不做，可以放心重複呼叫
    void end_startup_hold(const char* reason = "other");
    void shutdown();
    void on_query_capability();
    void update_title();
    void reset_title();
    void set_dark_mode_attributes(bool is_update = false) const;
    std::optional<config::WindowPlacementAndDpi> get_window_placement() const;
    void save_window_placement();
    bool is_window_placement_overridden() const;
    void override_window_placement(const config::WindowPlacementAndDpi& placement_and_dpi);
    void restore_window_placement();
    void mark_window_placement_overridden();
    void on_window_placement_imported();
    void update_window() const;
    void check_focus() const;
    void set_focus() const;

    /*
     * ITaskbarList3::ThumbBarUpdateButtons calls SendMessageTimeout without the SMTO_BLOCK flag.
     * So we postpone updates, to avoid weird bugs (other function calls when executing a callback
     * function that aren't legal, calls to ITaskbarList3::ThumbBarUpdateButtons() during another
     * ITaskbarList3::ThumbBarUpdateButtons() call etc.)
     */
    void queue_taskbar_button_update(bool update = true);
    void resize_child_windows();

    HWND get_wnd() const { return m_wnd; }

private:
    static constexpr std::array taskbar_icon_configs{icons::built_in::stop, icons::built_in::previous,
        icons::built_in::pause, icons::built_in::play, icons::built_in::next, icons::built_in::random};

    static LRESULT CALLBACK s_on_message(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) noexcept;
    static void warn_if_ui_hacks_installed();

    LRESULT on_message(HWND wnd, UINT msg, WPARAM wp, LPARAM lp);
    void on_create();
    void on_destroy();
    void create_child_windows();
    void set_title(const char* ptr);
    bool update_taskbar_button_images() const;
    void update_taskbar_buttons(bool update) const;
    void save_focus_state();
    void set_or_restore_focus() const;
    void set_window_placement(WINDOWPLACEMENT placement, bool is_initial = false, bool is_hidden = false);

    pfc::string8 m_window_title;
    wil::com_ptr<ITaskbarList3> m_taskbar_list;
    HWND m_wnd{};
    HWND m_last_focused_wnd{};
    HMONITOR m_monitor{};
    user_interface::HookProc_t m_hook_proc{};
    bool m_should_handle_multimedia_keys{true};
    bool m_shell_hook_registered{};
    bool m_gdiplus_initialised{};
    bool m_is_destroying{};
    ui_status_text_override::ptr m_status_text_override;
    bool m_last_systray_r_down{};
    bool m_last_systray_x1_down{};
    bool m_last_systray_x2_down{};
    fb2k_utils::SimpleKeyboardShortcutProcessor m_keyboard_shortcut_processor;
    FallbackMenu::Ptr m_fallback_menu;
    ULONG_PTR m_gdiplus_instance{};
    UINT m_wm_taskbarcreated{};
    UINT m_wm_taskbarbuttoncreated{};
    UINT m_wm_shellhookmessage{};
    bool m_is_window_placement_overridden{};

    // 開機優化 B：延後顯示的狀態。視窗照常顯示（位置、大小、最大化都照舊，所有面板也照常在「可見」的視窗裡載入），
    // 只是用 DWM 的「隱形」（cloak）先不讓畫面被畫出來，等網頁回報「好了」或逾時再解除。
    // 不用真的隱藏視窗：實測網頁面板（foo_webview2）在隱藏的視窗裡建立後，顯示出來也不會渲染（標頭一直是空殼的「WebView2」字樣）
    static bool should_hold_startup_show(const WINDOWPLACEMENT& placement, bool is_hidden);
    bool begin_startup_hold();
    // 逾時由另一條執行緒負責：主執行緒開機時常忙到訊息佇列排不完，WM_TIMER 會被拖延（慢機器實測預期 6 秒結果拖到 12 秒），
    // 解除 DWM 隱形可以從任何執行緒做，所以到時間就直接做，再通知主執行緒收尾
    void start_startup_hold_watchdog(DWORD wait_ms);
    void stop_startup_hold_watchdog();
    bool m_startup_hold_active{};
    ULONGLONG m_startup_hold_started_ms{};
    wil::unique_handle m_startup_hold_cancel;
    std::thread m_startup_hold_watchdog;

    wil::unique_himagelist m_taskbar_button_images;
    std::optional<uih::BufferedPaintInitialiser> m_buffered_paint_initialiser;
};

extern MainWindow main_window;
} // namespace cui
