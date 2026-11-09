#include "pch.h"
#include <charconv>
#include "config.h"
#include "next_track_toast.h"
#include "nocturne_text.h"
#include "dark_mode_dialog.h"
#include "main_window.h"
#include "now_playing_progress.h"
#include "output_chip.h"
#include "volume_chip.h"
#include "playlist_item_helpers.h"
#include "theme_font.h"
#include "thin_scrollbar.h"
#include "expanded_cover_bg.h"

// 自編版新增：Preferences → Tools → Nocturne 設定頁（2026-11-05）
// 跟 ESLyric 一樣放在 Tools 底下的獨立頁面，用 Columns UI 設定頁同一套分頁架構（PreferencesTabsHost），五個分頁：
// - 播放：進度條滾輪、輸出裝置顯示、音量條收成圖示
// - 下一首與佇列：展開頁角落的下一首提示（2026-11-02 做在 Columns UI 的分頁裡，11/05 搬到這裡）＋播放別首歌時保留播放佇列
//   （佇列 11/02 起放在這一頁底下，11/05 併進「播放」，11/06 獨立成分頁，11/08 使用者說一個選項一個分頁太浪費，又併回這裡）
// - 字型：開「主題字型」對話框（原本只在 View → Layout 選單裡）
// - 介面：細捲軸開關（11/08）、封面虛化背景與亮度上限（11/08）、主視窗等網頁面板載好再顯示（開機優化 B，11/06）。
//   這三個原本各自一個分頁、各只有一個設定，11/08 使用者要求合併成一頁（小標題分開）
// - 關於：只放產品名稱與版本號（2026-11-06）
// 設定都是改了立刻生效，沒有套用鈕；對話框的文字依語言在 WM_INITDIALOG 填入（.rc 裡只放版面）
// 設定視窗下方的「Reset page」會把播放、下一首與佇列、介面三個分頁還原成預設（字型分頁不動）
// 檔名沿用 tab_next_track_toast.cpp，免得動到專案檔

namespace cui::prefs {

namespace {

constexpr auto check_state = [](bool checked) { return checked ? BST_CHECKED : BST_UNCHECKED; };

// 秒數輸入框＋微調鈕（播放分頁用）：範圍內的值馬上生效；打到一半（空白、超出範圍）先不動，離開輸入框時整理成有效值
void init_seconds_spin(HWND wnd, int spin_id, uint32_t min, uint32_t max, uint32_t value)
{
    const auto spin = GetDlgItem(wnd, spin_id);
    SendMessage(spin, UDM_SETRANGE32, min, max);
    SendMessage(spin, UDM_SETPOS32, 0, std::clamp(value, min, max));
}

void apply_seconds_edit(HWND wnd, int edit_id, int code, uint32_t min, uint32_t max, fbh::ConfigUint32& setting)
{
    BOOL translated{};
    const auto typed = GetDlgItemInt(wnd, edit_id, &translated, FALSE);

    if (code == EN_CHANGE) {
        if (translated && typed >= min && typed <= max)
            setting = typed;
    } else if (code == EN_KILLFOCUS) {
        const auto value = std::clamp(translated ? typed : setting.get(), min, max);
        setting = value;
        SetDlgItemInt(wnd, edit_id, value, FALSE);
    }
}

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
    TabNocturnePlayback() : NocturneTab({IDC_TITLE1, IDC_TITLE2, IDC_TITLE3}) {}

    const char* get_name() override { return nocturne_text::playback_tab_name; }

private:
    UINT dialog_id() const override { return IDD_PREFS_NOCTURNE_PLAYBACK; }

    void on_init_dialog(HWND wnd) override
    {
        SetDlgItemText(wnd, IDC_TITLE1, nocturne_text::progress_title);
        SetDlgItemText(wnd, IDC_WHEEL_SEEK_ENABLED, nocturne_text::wheel_seek_enabled);
        SetDlgItemText(wnd, IDC_WHEEL_STEP_LABEL, nocturne_text::wheel_step_label);
        SetDlgItemText(wnd, IDC_WHEEL_SHIFT_LABEL, nocturne_text::wheel_shift_step_label);
        SetDlgItemText(wnd, IDC_TITLE2, nocturne_text::chip_settings_title);
        SetDlgItemText(wnd, IDC_CHIP_ENABLED, nocturne_text::chip_show);
        SetDlgItemText(wnd, IDC_CHIP_CLICK, nocturne_text::chip_click);
        SetDlgItemText(wnd, IDC_CHIP_ALIASES, nocturne_text::chip_edit_names);
        SetDlgItemText(wnd, IDC_TITLE3, nocturne_text::volume_title);
        SetDlgItemText(wnd, IDC_VOLUME_ICON, nocturne_text::volume_icon_only);

        CheckDlgButton(wnd, IDC_VOLUME_ICON, check_state(volume_chip::enabled.get()));
        CheckDlgButton(wnd, IDC_WHEEL_SEEK_ENABLED, check_state(now_playing_progress::wheel_seek_enabled.get()));
        CheckDlgButton(wnd, IDC_CHIP_ENABLED, check_state(output_chip::enabled.get()));
        CheckDlgButton(wnd, IDC_CHIP_CLICK, check_state(output_chip::click_to_switch.get()));

        init_seconds_spin(wnd, IDC_WHEEL_STEP_SPIN, now_playing_progress::wheel_step_min,
            now_playing_progress::wheel_step_max, now_playing_progress::wheel_step_seconds.get());
        init_seconds_spin(wnd, IDC_WHEEL_SHIFT_SPIN, now_playing_progress::wheel_shift_step_min,
            now_playing_progress::wheel_shift_step_max, now_playing_progress::wheel_shift_step_seconds.get());
    }

    void on_command(HWND wnd, WPARAM wp) override
    {
        if (LOWORD(wp) == IDC_WHEEL_STEP_SECONDS) {
            apply_seconds_edit(wnd, IDC_WHEEL_STEP_SECONDS, HIWORD(wp), now_playing_progress::wheel_step_min,
                now_playing_progress::wheel_step_max, now_playing_progress::wheel_step_seconds);
            return;
        }
        if (LOWORD(wp) == IDC_WHEEL_SHIFT_SECONDS) {
            apply_seconds_edit(wnd, IDC_WHEEL_SHIFT_SECONDS, HIWORD(wp), now_playing_progress::wheel_shift_step_min,
                now_playing_progress::wheel_shift_step_max, now_playing_progress::wheel_shift_step_seconds);
            return;
        }

        if (HIWORD(wp) != BN_CLICKED)
            return;

        const auto id = LOWORD(wp);
        const auto is_checked = IsDlgButtonChecked(wnd, id) == BST_CHECKED;

        if (id == IDC_WHEEL_SEEK_ENABLED)
            now_playing_progress::wheel_seek_enabled = is_checked;
        else if (id == IDC_CHIP_ENABLED)
            output_chip::enabled = is_checked;
        else if (id == IDC_CHIP_CLICK)
            output_chip::click_to_switch = is_checked;
        else if (id == IDC_VOLUME_ICON)
            volume_chip::enabled = is_checked;
        else if (id == IDC_CHIP_ALIASES)
            output_chip::open_alias_file();
    }
};

// ---- 下一首與佇列 ----
// 上半是下一首提示，下半是播放佇列（11/08 把原本獨立的「播放佇列」分頁併進來）
class TabNextTrackToast : public NocturneTab {
public:
    TabNextTrackToast() : NocturneTab({IDC_TITLE1, IDC_TITLE2}) {}

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
        SetDlgItemText(wnd, IDC_NEXT_TOAST_SIZE_LABEL, nocturne_text::next_toast_size_label);
        SetDlgItemText(wnd, IDC_TITLE2, nocturne_text::keep_queue_title);
        SetDlgItemText(wnd, IDC_KEEP_QUEUE_ON_PLAY, nocturne_text::keep_queue_on_play);

        CheckDlgButton(wnd, IDC_KEEP_QUEUE_ON_PLAY, check_state(playlist_item_helpers::keep_queue_on_play.get()));
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

        const auto size_combo = GetDlgItem(wnd, IDC_NEXT_TOAST_SIZE);
        for (const auto& name : nocturne_text::next_toast_size_names)
            ComboBox_AddString(size_combo, name);
        ComboBox_SetCurSel(size_combo,
            gsl::narrow<int>(std::min(next_track_toast::size_index.get(), next_track_toast::size_count - 1)));
    }

    void on_command(HWND wnd, WPARAM wp) override
    {
        const auto id = LOWORD(wp);
        const auto code = HIWORD(wp);
        const auto is_checked = [wnd, id] { return IsDlgButtonChecked(wnd, id) == BST_CHECKED; };

        switch (id) {
        case IDC_KEEP_QUEUE_ON_PLAY:
            if (code == BN_CLICKED)
                playlist_item_helpers::keep_queue_on_play = is_checked();
            break;
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
        case IDC_NEXT_TOAST_SIZE:
            if (code == CBN_SELCHANGE) {
                const auto selection = ComboBox_GetCurSel(GetDlgItem(wnd, id));
                if (selection != CB_ERR)
                    next_track_toast::size_index = gsl::narrow<uint32_t>(selection);
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
    // 各列的字族下拉有沒有填過整份清單（沒填過時下拉裡只有「目前字型」一個項目，每次開分頁都重設）
    std::array<bool, row_count> m_family_filled{};

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

        // 字族下拉先不填：系統有 700 多個字族，7 個下拉一次填滿要 5 千多次 AddString，開分頁要卡 0.7 秒以上（2026-11-05 實測）。
        // 平常每個下拉只放「目前的字型」一個項目，使用者按開下拉（CBN_DROPDOWN）時才把整份清單填進去
        m_family_filled.fill(false);

        for (int row = 0; row < row_count; ++row) {
            SetDlgItemText(wnd, IDC_FONT_ROW_LABEL0 + row, labels[row]);

            const auto size_combo = GetDlgItem(wnd, IDC_FONT_ROW_SIZE0 + row);
            for (size_t index{}; index < theme_font::size_option_count(); ++index)
                ComboBox_AddString(size_combo, theme_font::size_option_label(index));
        }

        refresh_rows(wnd);
        update_restart_controls(wnd);
    }

    // 這一列的字族下拉還沒填整份清單時，把它換成只有「目前字型」一個項目；填過了就直接選到對的位置
    void show_current_family(HWND wnd, int row, std::optional<size_t> family) const
    {
        const auto combo = GetDlgItem(wnd, IDC_FONT_ROW_FAMILY0 + row);

        if (m_family_filled[row]) {
            ComboBox_SetCurSel(combo, family ? static_cast<int>(*family) : -1);
            return;
        }

        ComboBox_ResetContent(combo);
        if (family) {
            ComboBox_AddString(combo, theme_font::font_family_name(*family).c_str());
            ComboBox_SetCurSel(combo, 0);
        }
    }

    // 按開下拉的當下才把整份字族清單填進去（清單位置＝字族編號），並選到目前的字型
    void fill_family_list(HWND wnd, int row)
    {
        if (m_family_filled[row])
            return;

        const auto combo = GetDlgItem(wnd, IDC_FONT_ROW_FAMILY0 + row);
        const auto family = theme_font::current_family_index(region_of_row(row));
        const auto count = theme_font::font_family_count();

        SetWindowRedraw(combo, FALSE);
        ComboBox_ResetContent(combo);
        SendMessage(combo, CB_INITSTORAGE, count, count * 40 * sizeof(wchar_t));
        for (size_t index{}; index < count; ++index)
            ComboBox_AddString(combo, theme_font::font_family_name(index).c_str());
        ComboBox_SetCurSel(combo, family ? static_cast<int>(*family) : -1);
        SetWindowRedraw(combo, TRUE);

        m_family_filled[row] = true;
    }

    // 把每一列的下拉選到目前實際的字族與大小（「全部區塊」那一列看第一個項目；字型不在清單裡就留空）
    void refresh_rows(HWND wnd) const
    {
        for (int row = 0; row < row_count; ++row) {
            const auto region = region_of_row(row);

            show_current_family(wnd, row, theme_font::current_family_index(region));

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

        const auto is_family_combo = id >= IDC_FONT_ROW_FAMILY0 && id < IDC_FONT_ROW_FAMILY0 + row_count;

        // 按開字族下拉的當下才把整份清單填進去（見 on_init_dialog 的說明）
        if (code == CBN_DROPDOWN && is_family_combo) {
            fill_family_list(wnd, id - IDC_FONT_ROW_FAMILY0);
            return;
        }

        if (code != CBN_SELCHANGE)
            return;

        int row = -1;
        if (is_family_combo)
            row = id - IDC_FONT_ROW_FAMILY0;
        else if (id >= IDC_FONT_ROW_SIZE0 && id < IDC_FONT_ROW_SIZE0 + row_count)
            row = id - IDC_FONT_ROW_SIZE0;

        if (row < 0)
            return;

        // 字族清單還沒填（下拉裡只有「目前字型」一個項目）時，下拉的選取位置不是字族編號：
        // 改了大小就用目前的字族編號；字族下拉本身不會有選取變化，有的話也不處理
        std::optional<size_t> family;
        if (m_family_filled[row]) {
            const auto selection = ComboBox_GetCurSel(GetDlgItem(wnd, IDC_FONT_ROW_FAMILY0 + row));
            if (selection != CB_ERR)
                family = static_cast<size_t>(selection);
        } else if (!is_family_combo) {
            family = theme_font::current_family_index(region_of_row(row));
        }

        // 這一列的字型目前不在清單裡（字族下拉是空的）而且只改了大小：沒有字族可以套，不處理
        if (!family)
            return;

        auto size = ComboBox_GetCurSel(GetDlgItem(wnd, IDC_FONT_ROW_SIZE0 + row));
        if (size == CB_ERR)
            size = static_cast<int>(theme_font::default_size_option());

        const auto result = theme_font::apply_font(region_of_row(row), *family, static_cast<size_t>(size));
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

// ---- 介面（2026-11-08）----
// 三個各只有一個設定的小項目合併成一頁，各自一個小標題（原本是「外觀」「展開頁」「啟動」三個分頁）：
// - 捲軸：細捲軸開關。開關存在 fb2k 共用設定（資料夾樹那個元件也讀它），邏輯在 thin_scrollbar.cpp；改了馬上生效
// - 展開頁：封面虛化背景一個勾選＋「亮度上限」下拉（10～35％，每 5％一格）；沒勾時亮度上限整個藏起來。
//   之後有別的展開頁選項（例如封面光暈）放在這一塊。邏輯在 expanded_cover_bg.cpp；改了馬上生效
// - 啟動：開機優化 B，主視窗等網頁面板回報「好了」再顯示（邏輯在 main_window.cpp）。設定在啟動時才讀，所以改了要下次啟動才有效
class TabNocturneInterface : public NocturneTab {
public:
    TabNocturneInterface() : NocturneTab({IDC_TITLE1, IDC_TITLE2, IDC_TITLE3}) {}

    const char* get_name() override { return nocturne_text::interface_tab_name; }

private:
    UINT dialog_id() const override { return IDD_PREFS_NOCTURNE_APPEARANCE; }

    static void show_cap(HWND wnd, bool visible)
    {
        ShowWindow(GetDlgItem(wnd, IDC_EXPANDED_BG_CAP_LABEL), visible ? SW_SHOW : SW_HIDE);
        ShowWindow(GetDlgItem(wnd, IDC_EXPANDED_BG_CAP), visible ? SW_SHOW : SW_HIDE);
    }

    void on_init_dialog(HWND wnd) override
    {
        // 捲軸
        SetDlgItemText(wnd, IDC_TITLE1, nocturne_text::appearance_tab_title);
        SetDlgItemText(wnd, IDC_THIN_SCROLLBAR, nocturne_text::thin_scrollbar);
        SetDlgItemText(wnd, IDC_THIN_SCROLLBAR_NOTE, nocturne_text::thin_scrollbar_note);
        CheckDlgButton(wnd, IDC_THIN_SCROLLBAR, check_state(thin_scrollbar::is_enabled()));

        // 展開頁
        SetDlgItemText(wnd, IDC_TITLE2, nocturne_text::expanded_tab_title);
        SetDlgItemText(wnd, IDC_EXPANDED_BG, nocturne_text::expanded_bg);
        SetDlgItemText(wnd, IDC_EXPANDED_BG_CAP_LABEL, nocturne_text::expanded_bg_cap_label);

        const auto cap_combo = GetDlgItem(wnd, IDC_EXPANDED_BG_CAP);
        for (const auto& name : nocturne_text::expanded_bg_cap_names)
            ComboBox_AddString(cap_combo, name);
        const auto cap = std::clamp(expanded_cover_bg::cap_percent.get(), expanded_cover_bg::cap_min,
            expanded_cover_bg::cap_max);
        ComboBox_SetCurSel(cap_combo, gsl::narrow<int>((cap - expanded_cover_bg::cap_min) / expanded_cover_bg::cap_step));

        CheckDlgButton(wnd, IDC_EXPANDED_BG, check_state(expanded_cover_bg::enabled.get()));
        show_cap(wnd, expanded_cover_bg::enabled.get());

        // 啟動
        SetDlgItemText(wnd, IDC_TITLE3, nocturne_text::startup_tab_title);
        SetDlgItemText(wnd, IDC_STARTUP_HOLD, nocturne_text::startup_hold);
        SetDlgItemText(wnd, IDC_STARTUP_HOLD_NOTE, nocturne_text::startup_hold_note);
        CheckDlgButton(wnd, IDC_STARTUP_HOLD, check_state(startup_hold_until_ready.get()));
    }

    void on_command(HWND wnd, WPARAM wp) override
    {
        const auto id = LOWORD(wp);
        const auto code = HIWORD(wp);

        if (id == IDC_THIN_SCROLLBAR && code == BN_CLICKED) {
            thin_scrollbar::set_enabled(IsDlgButtonChecked(wnd, IDC_THIN_SCROLLBAR) == BST_CHECKED);
        } else if (id == IDC_EXPANDED_BG && code == BN_CLICKED) {
            const auto checked = IsDlgButtonChecked(wnd, IDC_EXPANDED_BG) == BST_CHECKED;
            expanded_cover_bg::enabled = checked;
            show_cap(wnd, checked);
        } else if (id == IDC_EXPANDED_BG_CAP && code == CBN_SELCHANGE) {
            const auto selection = ComboBox_GetCurSel(GetDlgItem(wnd, id));
            if (selection != CB_ERR)
                expanded_cover_bg::cap_percent
                    = expanded_cover_bg::cap_min + gsl::narrow<uint32_t>(selection) * expanded_cover_bg::cap_step;
        } else if (id == IDC_STARTUP_HOLD && code == BN_CLICKED) {
            startup_hold_until_ready = IsDlgButtonChecked(wnd, IDC_STARTUP_HOLD) == BST_CHECKED;
        }
    }
};

// ---- 關於（2026-11-06，使用者要求加回來但「不要寫太厚、主要就寫版本號」）----
// 只放產品名稱與版本號；版本號在 nocturne_text.h 的 nocturne_version。11/05 做過的「關於」分頁有版本與必要元件檢查，使用者說不要，這次刻意只留版本號
class TabNocturneAbout : public NocturneTab {
public:
    TabNocturneAbout() : NocturneTab({IDC_TITLE1}) {}

    const char* get_name() override { return nocturne_text::about_tab_name; }

private:
    UINT dialog_id() const override { return IDD_PREFS_NOCTURNE_ABOUT; }

    void on_init_dialog(HWND wnd) override
    {
        SetDlgItemText(wnd, IDC_TITLE1, nocturne_text::about_tab_title);
        SetDlgItemText(wnd, IDC_ABOUT_NAME, nocturne_text::about_product_name);
        const std::wstring version = std::wstring(nocturne_text::about_version_prefix) + nocturne_text::nocturne_version;
        SetDlgItemText(wnd, IDC_ABOUT_VERSION, version.c_str());
    }

    void on_command(HWND, WPARAM) override {}
};

TabNocturnePlayback g_tab_nocturne_playback;
TabNextTrackToast g_tab_next_track_toast;
TabNocturneFont g_tab_nocturne_font;
TabNocturneInterface g_tab_nocturne_interface;
TabNocturneAbout g_tab_nocturne_about;

// 分頁順序：播放、下一首與佇列、字型、介面、關於。
// 存的「最後開啟的分頁」是這個順序的編號，所以合併或插入分頁（2026-11-06 播放佇列插在播放後面、11/08 外觀與展開頁插在關於前面、
// 11/08 晚把播放佇列／啟動／外觀／展開頁併成現在這五頁）之後，已經用過的人更新後第一次開設定頁可能停在不同的分頁
// （編號超出範圍時由分頁架構處理），無傷大雅；之後新增分頁盡量加在最後面（關於之前）
PreferencesTab* g_nocturne_tabs[] = {&g_tab_nocturne_playback, &g_tab_next_track_toast, &g_tab_nocturne_font,
    &g_tab_nocturne_interface, &g_tab_nocturne_about};

// 最後開啟的分頁（存分頁順序，見上面 g_nocturne_tabs 的說明）
cfg_int cfg_nocturne_active_tab(
    GUID{0xa3d8f152, 0x6b94, 0x4c27, {0x8e, 0x1f, 0x40, 0xb9, 0x72, 0xd5, 0x3c, 0x68}}, 0);

// {67EE972D-3FB3-4607-BDAF-CE86327347D0}
constexpr GUID guid_nocturne_preferences_page
    = {0x67ee972d, 0x3fb3, 0x4607, {0xbd, 0xaf, 0xce, 0x86, 0x32, 0x73, 0x47, 0xd0}};

// 專輯標頭等網頁面板：焦點在網頁裡時（滑鼠點過面板之後），WebView2 把瀏覽器的快速鍵吃掉——Ctrl+P 變成「列印」視窗、
// F5 重新整理、Ctrl+F 尋找……根本不會交給 foobar2000（2026-11-05）。網頁（index.html）攔下 Ctrl／Alt 組合鍵與功能鍵，
// 再用 fb2k.component.print 往 Console 印一行暗號；這裡收到暗號就把那個鍵交給 foobar2000 的快速鍵系統，
// 效果等於焦點在別處按同一個鍵（使用者改過的快速鍵也有效）。
// 暗號只是一行 Console 文字：沒有這個元件的環境（朋友版、舊版）網頁印了也無害，只是這些鍵在網頁裡什麼都不會發生。
// 不用 shell.execute：網頁面板預設沒開「執行 shell」權限（實測 canExecuteShellOperations＝false），也不該為了這個去開
// - 「[Nocturne] key <虛擬鍵碼> <修飾鍵>」：修飾鍵 Shift＝1、Ctrl＝2、Alt＝4，跟 HOTKEYF_* 同值，所以 (鍵碼 | 修飾鍵 << 8)
//   就是 process_keydown_simple 要的格式（Columns UI 自己處理快速鍵也是這樣，見 fb2k_misc.cpp）
// - 「[Nocturne] open-preferences」：舊版網頁只攔 Ctrl+P 時用的暗號，保留給還沒更新的 index.html
constexpr std::string_view web_key_marker = "[Nocturne] key ";
constexpr std::string_view open_preferences_marker = "[Nocturne] open-preferences";
// - 「[Nocturne] ready」：網頁第一次載入完成（標頭、封面都套用了）。主視窗啟動時先不顯示，收到這句才顯示（開機優化 B，2026-11-06）；
//   視窗不在等的時候（網頁重新載入、已經顯示）收到只是忽略
constexpr std::string_view page_ready_marker = "[Nocturne] ready";

class WebKeyFromConsole : public console_receiver {
public:
    void print(const char* message, t_size length) override
    {
        // SDK 的 console::print(const char*) 對所有 receiver 都傳 SIZE_MAX（＝以 \0 結尾、沒給長度），
        // 直接當長度用，字串搜尋會往記憶體後面一路讀下去（2026-11-05 實測：std::string_view::find 內部 memmove 寫爆堆疊，fb2k 啟動就崩潰）
        const std::string_view text(message, length == SIZE_MAX ? std::strlen(message) : length);

        if (text.find(open_preferences_marker) != std::string_view::npos) {
            // 這個函式可能在任何執行緒被呼叫，開設定視窗一定要回主執行緒做
            fb2k::inMainThread([] { standard_commands::main_preferences(); });
            return;
        }

        if (text.find(page_ready_marker) != std::string_view::npos) {
            fb2k::inMainThread([] { main_window.end_startup_hold("page ready", true); });
            return;
        }

        const auto position = text.find(web_key_marker);
        if (position == std::string_view::npos)
            return;

        const auto numbers = text.substr(position + web_key_marker.size());
        unsigned key{};
        unsigned modifiers{};
        const auto first = std::from_chars(numbers.data(), numbers.data() + numbers.size(), key);
        if (first.ec != std::errc{} || first.ptr == numbers.data() + numbers.size() || *first.ptr != ' ')
            return;
        const auto second = std::from_chars(first.ptr + 1, numbers.data() + numbers.size(), modifiers);
        if (second.ec != std::errc{} || key == 0 || key > 0xFF || modifiers > 7)
            return;

        const auto key_code = static_cast<uint32_t>(key | (modifiers << 8));
        fb2k::inMainThread([key_code] { keyboard_shortcut_manager_v2::get()->process_keydown_simple(key_code); });
    }
};

service_factory_single_t<WebKeyFromConsole> g_web_key_from_console;

// 設定視窗的「Reset page」：播放、下一首與佇列、介面三個分頁的所有設定還原成預設。
// 字型分頁不動——字型是改 Columns UI 字型項目與主題檔，沒有「預設字型」可以還原，要回到原本的設計請把整體大小選「標準」
void reset_nocturne_settings()
{
    now_playing_progress::wheel_seek_enabled.reset();
    now_playing_progress::wheel_step_seconds.reset();
    now_playing_progress::wheel_shift_step_seconds.reset();
    playlist_item_helpers::keep_queue_on_play.reset();
    output_chip::enabled.reset();
    output_chip::click_to_switch.reset();
    volume_chip::enabled.reset();

    next_track_toast::enabled.reset();
    next_track_toast::seconds.reset();
    next_track_toast::show_seconds_in_ring.reset();
    next_track_toast::show_cover.reset();
    next_track_toast::show_source_tag.reset();
    next_track_toast::position.reset();
    next_track_toast::size_index.reset();

    startup_hold_until_ready.reset();

    thin_scrollbar::set_enabled(thin_scrollbar::enabled_default);

    expanded_cover_bg::enabled.reset();
    expanded_cover_bg::cap_percent.reset();
}

// 頁面名稱是品牌名，中英文版相同；父頁面是 Tools（跟 ESLyric 同一層）
service_factory_single_t<PreferencesTabsHost> g_nocturne_preferences_page("Nocturne", g_nocturne_tabs,
    guid_nocturne_preferences_page, preferences_page::guid_tools, cfg_nocturne_active_tab, reset_nocturne_settings);

} // namespace

PreferencesTab* g_get_tab_next_track_toast()
{
    return &g_tab_next_track_toast;
}

} // namespace cui::prefs
