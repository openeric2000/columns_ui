#include "pch.h"
#include "config.h"
#include "next_track_toast.h"
#include "nocturne_text.h"
#include "dark_mode_dialog.h"
#include "now_playing_progress.h"
#include "playlist_item_helpers.h"
#include "theme_font.h"

// 自編版新增：Preferences → Tools → Nocturne 設定頁（2026-11-05）
// 跟 ESLyric 一樣放在 Tools 底下的獨立頁面，用 Columns UI 設定頁同一套分頁架構（PreferencesTabsHost），三個分頁：
// - 播放：進度條滾輪開關、播放佇列（2026-11-02 起放在「下一首提示」分頁底下，11/05 獨立出來）
// - 下一首提示：展開頁角落的提示（2026-11-02 做在 Columns UI 的分頁裡，11/05 搬到這裡）
// - 字型：開「主題字型」對話框（原本只在 View → Layout 選單裡）
// 設定都是改了立刻生效，沒有套用鈕；對話框的文字依語言在 WM_INITDIALOG 填入（.rc 裡只放版面）
// 檔名沿用 tab_next_track_toast.cpp，免得動到專案檔

namespace cui::prefs {

namespace {

constexpr auto check_state = [](bool checked) { return checked ? BST_CHECKED : BST_UNCHECKED; };

// 三個分頁共用的骨架：建立對話框、把訊息轉給 on_message
class NocturneTab : public PreferencesTab {
public:
    HWND create(HWND wnd) override
    {
        return m_helper.create(wnd, dialog_id(),
            [this](HWND dialog, UINT msg, WPARAM wp, LPARAM lp) { return on_message(dialog, msg, wp, lp); });
    }

protected:
    explicit NocturneTab(std::set<unsigned> h1_ctrl_ids) : m_helper(std::move(h1_ctrl_ids)) {}

    virtual UINT dialog_id() const = 0;
    virtual void on_init_dialog(HWND wnd) = 0;
    virtual void on_command(HWND wnd, WPARAM wp) = 0;

    // 填值時控制項會送出變更通知，先擋掉，免得把還沒填完的值寫回設定
    bool m_initialising{};

private:
    INT_PTR on_message(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg) {
        case WM_INITDIALOG:
            m_initialising = true;
            on_init_dialog(wnd);
            m_initialising = false;
            break;
        case WM_COMMAND:
            if (!m_initialising)
                on_command(wnd, wp);
            break;
        }
        return 0;
    }

    PreferencesTabHelper m_helper;
};

// ---- 播放 ----
class TabNocturnePlayback : public NocturneTab {
public:
    TabNocturnePlayback() : NocturneTab({IDC_TITLE1, IDC_TITLE2}) {}

    const char* get_name() override { return nocturne_text::playback_tab_name; }

private:
    UINT dialog_id() const override { return IDD_PREFS_NOCTURNE_PLAYBACK; }

    void on_init_dialog(HWND wnd) override
    {
        SetDlgItemText(wnd, IDC_TITLE1, nocturne_text::progress_title);
        SetDlgItemText(wnd, IDC_WHEEL_SEEK_ENABLED, nocturne_text::wheel_seek_enabled);
        SetDlgItemText(wnd, IDC_WHEEL_SEEK_NOTE, nocturne_text::wheel_seek_note);
        SetDlgItemText(wnd, IDC_TITLE2, nocturne_text::keep_queue_title);
        SetDlgItemText(wnd, IDC_KEEP_QUEUE_ON_PLAY, nocturne_text::keep_queue_on_play);

        CheckDlgButton(wnd, IDC_WHEEL_SEEK_ENABLED, check_state(now_playing_progress::wheel_seek_enabled.get()));
        CheckDlgButton(wnd, IDC_KEEP_QUEUE_ON_PLAY, check_state(playlist_item_helpers::keep_queue_on_play.get()));
    }

    void on_command(HWND wnd, WPARAM wp) override
    {
        if (HIWORD(wp) != BN_CLICKED)
            return;

        const auto id = LOWORD(wp);
        const auto is_checked = IsDlgButtonChecked(wnd, id) == BST_CHECKED;

        if (id == IDC_WHEEL_SEEK_ENABLED)
            now_playing_progress::wheel_seek_enabled = is_checked;
        else if (id == IDC_KEEP_QUEUE_ON_PLAY)
            playlist_item_helpers::keep_queue_on_play = is_checked;
    }
};

// ---- 下一首提示 ----
class TabNextTrackToast : public NocturneTab {
public:
    TabNextTrackToast() : NocturneTab({IDC_TITLE1}) {}

    const char* get_name() override { return nocturne_text::next_toast_tab_name; }

private:
    static uint32_t clamp_seconds(uint32_t value)
    {
        return std::clamp(value, next_track_toast::min_seconds, next_track_toast::max_seconds);
    }

    UINT dialog_id() const override { return IDD_PREFS_NEXT_TRACK_TOAST; }

    void on_init_dialog(HWND wnd) override
    {
        SetDlgItemText(wnd, IDC_TITLE1, nocturne_text::next_toast_title);
        SetDlgItemText(wnd, IDC_NEXT_TOAST_ENABLED, nocturne_text::next_toast_enabled);
        SetDlgItemText(wnd, IDC_NEXT_TOAST_SECONDS_LABEL, nocturne_text::next_toast_seconds_label);
        SetDlgItemText(wnd, IDC_NEXT_TOAST_SHOW_SECONDS, nocturne_text::next_toast_show_seconds);
        SetDlgItemText(wnd, IDC_NEXT_TOAST_SHOW_COVER, nocturne_text::next_toast_show_cover);
        SetDlgItemText(wnd, IDC_NEXT_TOAST_SHOW_TAG, nocturne_text::next_toast_show_tag);
        SetDlgItemText(wnd, IDC_NEXT_TOAST_POSITION_LABEL, nocturne_text::next_toast_position_label);

        CheckDlgButton(wnd, IDC_NEXT_TOAST_ENABLED, check_state(next_track_toast::enabled.get()));
        CheckDlgButton(wnd, IDC_NEXT_TOAST_SHOW_SECONDS, check_state(next_track_toast::show_seconds_in_ring.get()));
        CheckDlgButton(wnd, IDC_NEXT_TOAST_SHOW_COVER, check_state(next_track_toast::show_cover.get()));
        CheckDlgButton(wnd, IDC_NEXT_TOAST_SHOW_TAG, check_state(next_track_toast::show_source_tag.get()));

        const auto spin = GetDlgItem(wnd, IDC_NEXT_TOAST_SECONDS_SPIN);
        SendMessage(spin, UDM_SETRANGE32, next_track_toast::min_seconds, next_track_toast::max_seconds);
        SendMessage(spin, UDM_SETPOS32, 0, clamp_seconds(next_track_toast::seconds.get()));

        const auto position_combo = GetDlgItem(wnd, IDC_NEXT_TOAST_POSITION);
        for (const auto& name : nocturne_text::next_toast_position_names)
            ComboBox_AddString(position_combo, name);
        ComboBox_SetCurSel(position_combo,
            gsl::narrow<int>(std::min(next_track_toast::position.get(), next_track_toast::position_count - 1)));
    }

    void on_command(HWND wnd, WPARAM wp) override
    {
        const auto id = LOWORD(wp);
        const auto code = HIWORD(wp);
        const auto is_checked = [wnd, id] { return IsDlgButtonChecked(wnd, id) == BST_CHECKED; };

        switch (id) {
        case IDC_NEXT_TOAST_ENABLED:
            if (code == BN_CLICKED)
                next_track_toast::enabled = is_checked();
            break;
        case IDC_NEXT_TOAST_SHOW_SECONDS:
            if (code == BN_CLICKED)
                next_track_toast::show_seconds_in_ring = is_checked();
            break;
        case IDC_NEXT_TOAST_SHOW_COVER:
            if (code == BN_CLICKED)
                next_track_toast::show_cover = is_checked();
            break;
        case IDC_NEXT_TOAST_SHOW_TAG:
            if (code == BN_CLICKED)
                next_track_toast::show_source_tag = is_checked();
            break;
        case IDC_NEXT_TOAST_SECONDS:
            if (code == EN_CHANGE) {
                // 打字或按微調鈕時，範圍內的值馬上生效；打到一半（空白、超出範圍）先不動
                BOOL translated{};
                const auto value = GetDlgItemInt(wnd, id, &translated, FALSE);
                if (translated && value >= next_track_toast::min_seconds && value <= next_track_toast::max_seconds)
                    next_track_toast::seconds = value;
            } else if (code == EN_KILLFOCUS) {
                // 離開輸入框時把框裡的值整理成有效值（超出範圍就夾到邊界，空白就還原）
                BOOL translated{};
                const auto typed = GetDlgItemInt(wnd, id, &translated, FALSE);
                const auto value = clamp_seconds(translated ? typed : next_track_toast::seconds.get());
                next_track_toast::seconds = value;
                SetDlgItemInt(wnd, id, value, FALSE);
            }
            break;
        case IDC_NEXT_TOAST_POSITION:
            if (code == CBN_SELCHANGE) {
                const auto selection = ComboBox_GetCurSel(GetDlgItem(wnd, id));
                if (selection != CB_ERR)
                    next_track_toast::position = gsl::narrow<uint32_t>(selection);
            }
            break;
        }
    }
};

// ---- 字型 ----
// 區塊表（方案 A，2026-11-05 使用者選的）：第 0 列「全部區塊」＋六個區塊各一列，每列「標籤、字族下拉、大小下拉」。
// 選了就馬上套用（跟這一頁其他設定一樣沒有套用鈕）：前四個區塊（Columns UI 字型項目）即時生效；
// 播放列與展開頁、專輯標頭寫進主題檔，要重開 fb2k 才生效，只能換字族（沒有大小下拉）。分區塊的邏輯在 theme_font.cpp
class TabNocturneFont : public NocturneTab {
public:
    TabNocturneFont() : NocturneTab({IDC_TITLE1}) {}

    const char* get_name() override { return nocturne_text::font_tab_name; }

private:
    // 第 0 列是「全部區塊」，第 1～6 列是各區塊（區塊編號＝列號 − 1）
    static constexpr int row_count = 7;
    // 主題檔換過、還沒重開 fb2k：切到別的分頁再回來也要記得，所以放成靜態
    inline static bool s_restart_pending = false;

    static std::optional<size_t> region_of_row(int row)
    {
        return row == 0 ? std::nullopt : std::optional<size_t>(static_cast<size_t>(row - 1));
    }

    UINT dialog_id() const override { return IDD_PREFS_NOCTURNE_FONT; }

    void on_init_dialog(HWND wnd) override
    {
        SetDlgItemText(wnd, IDC_TITLE1, nocturne_text::font_tab_title);
        SetDlgItemText(wnd, IDC_NOCTURNE_FONT_NOTE, nocturne_text::font_tab_note);

        const std::wstring foot
            = std::wstring(nocturne_text::font_tab_foot) + L"\r\n\r\n" + nocturne_text::theme_font_lyrics_note;
        SetDlgItemText(wnd, IDC_NOCTURNE_FONT_FOOT, foot.c_str());
        SetDlgItemText(wnd, IDC_NOCTURNE_FONT_RESTART_NOTE, nocturne_text::font_restart_note);
        SetDlgItemText(wnd, IDC_NOCTURNE_FONT_RESTART, nocturne_text::font_restart_button);

        const wchar_t* labels[row_count]{nocturne_text::font_row_all, nocturne_text::font_row_playlist,
            nocturne_text::font_row_tabs, nocturne_text::font_row_tree, nocturne_text::font_row_ui,
            nocturne_text::font_row_bar, nocturne_text::font_row_header};

        for (int row = 0; row < row_count; ++row) {
            SetDlgItemText(wnd, IDC_FONT_ROW_LABEL0 + row, labels[row]);

            const auto family_combo = GetDlgItem(wnd, IDC_FONT_ROW_FAMILY0 + row);
            for (size_t index{}; index < theme_font::font_family_count(); ++index)
                ComboBox_AddString(family_combo, theme_font::font_family_name(index).c_str());

            const auto size_combo = GetDlgItem(wnd, IDC_FONT_ROW_SIZE0 + row);
            for (size_t index{}; index < theme_font::size_option_count(); ++index)
                ComboBox_AddString(size_combo, theme_font::size_option_label(index));
        }

        refresh_rows(wnd);
        update_restart_controls(wnd);
    }

    // 把每一列的下拉選到目前實際的字族與大小（「全部區塊」那一列看第一個項目；字型不在清單裡就留空）
    void refresh_rows(HWND wnd) const
    {
        for (int row = 0; row < row_count; ++row) {
            const auto region = region_of_row(row);

            const auto family = theme_font::current_family_index(region);
            ComboBox_SetCurSel(GetDlgItem(wnd, IDC_FONT_ROW_FAMILY0 + row), family ? static_cast<int>(*family) : -1);

            // 主題檔區塊（播放列、專輯標頭）沒有大小，把大小下拉藏起來
            const auto size_combo = GetDlgItem(wnd, IDC_FONT_ROW_SIZE0 + row);
            const auto has_size = !region || theme_font::font_region_has_size(*region);
            ShowWindow(size_combo, has_size ? SW_SHOW : SW_HIDE);
            if (has_size)
                ComboBox_SetCurSel(size_combo, static_cast<int>(theme_font::current_size_index(region)));
        }
    }

    void update_restart_controls(HWND wnd) const
    {
        const auto show = s_restart_pending ? SW_SHOW : SW_HIDE;
        ShowWindow(GetDlgItem(wnd, IDC_NOCTURNE_FONT_RESTART_NOTE), show);
        ShowWindow(GetDlgItem(wnd, IDC_NOCTURNE_FONT_RESTART), show);
    }

    void on_command(HWND wnd, WPARAM wp) override
    {
        const auto id = LOWORD(wp);
        const auto code = HIWORD(wp);

        if (id == IDC_NOCTURNE_FONT_RESTART && code == BN_CLICKED) {
            standard_commands::main_restart();
            return;
        }

        if (code != CBN_SELCHANGE)
            return;

        int row = -1;
        if (id >= IDC_FONT_ROW_FAMILY0 && id < IDC_FONT_ROW_FAMILY0 + row_count)
            row = id - IDC_FONT_ROW_FAMILY0;
        else if (id >= IDC_FONT_ROW_SIZE0 && id < IDC_FONT_ROW_SIZE0 + row_count)
            row = id - IDC_FONT_ROW_SIZE0;

        if (row < 0)
            return;

        // 這一列的字型目前不在清單裡（字族下拉是空的）而且只改了大小：沒有字族可以套，不處理
        const auto family = ComboBox_GetCurSel(GetDlgItem(wnd, IDC_FONT_ROW_FAMILY0 + row));
        if (family == CB_ERR)
            return;

        auto size = ComboBox_GetCurSel(GetDlgItem(wnd, IDC_FONT_ROW_SIZE0 + row));
        if (size == CB_ERR)
            size = static_cast<int>(theme_font::default_size_option());

        const auto result = theme_font::apply_font(region_of_row(row), static_cast<size_t>(family), static_cast<size_t>(size));
        const auto parent = GetAncestor(wnd, GA_ROOT);

        if (result.error) {
            dark::modal_info_box(parent, nocturne_text::theme_font_title, result.error->c_str(), uih::InfoBoxType::Error,
                uih::InfoBoxModalType::OK);
        } else if (!result.failures.empty()) {
            auto message = fmt::format(nocturne_text::failures_message, result.failures);
            message.erase(0, message.find_first_not_of('\n'));
            dark::modal_info_box(parent, nocturne_text::theme_font_title, message.c_str(), uih::InfoBoxType::Warning,
                uih::InfoBoxModalType::OK);
        }

        if (result.theme_file_changed)
            s_restart_pending = true;

        // 改「全部區塊」會動到每一列；改單一區塊也順便重讀，確保畫面跟實際一致
        refresh_rows(wnd);
        update_restart_controls(wnd);
    }
};

TabNocturnePlayback g_tab_nocturne_playback;
TabNextTrackToast g_tab_next_track_toast;
TabNocturneFont g_tab_nocturne_font;

PreferencesTab* g_nocturne_tabs[] = {&g_tab_nocturne_playback, &g_tab_next_track_toast, &g_tab_nocturne_font};

// 最後開啟的分頁（存分頁順序；之後新增分頁請加在最後面，不要改前面的順序）
cfg_int cfg_nocturne_active_tab(
    GUID{0xa3d8f152, 0x6b94, 0x4c27, {0x8e, 0x1f, 0x40, 0xb9, 0x72, 0xd5, 0x3c, 0x68}}, 0);

// {67EE972D-3FB3-4607-BDAF-CE86327347D0}
constexpr GUID guid_nocturne_preferences_page
    = {0x67ee972d, 0x3fb3, 0x4607, {0xbd, 0xaf, 0xce, 0x86, 0x32, 0x73, 0x47, 0xd0}};

// 頁面名稱是品牌名，中英文版相同；父頁面是 Tools（跟 ESLyric 同一層）
service_factory_single_t<PreferencesTabsHost> g_nocturne_preferences_page("Nocturne", g_nocturne_tabs,
    guid_nocturne_preferences_page, preferences_page::guid_tools, cfg_nocturne_active_tab);

} // namespace

PreferencesTab* g_get_tab_next_track_toast()
{
    return &g_tab_next_track_toast;
}

} // namespace cui::prefs
