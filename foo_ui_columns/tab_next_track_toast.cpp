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
#include "playlist_marquee.h"
#include "expanded_cover_bg.h"
#include "menu_items.h"
#include "now_playing_cover_zoom.h"

// 建置資訊（版本種類、日期）：build_columns_ui.ps1 每次建置時產生，不進版控。
// 沒有這個檔（例如直接從原始碼編）時，「關於」只顯示版本號，跟公開版一樣
#if __has_include("nocturne_build_info.h")
#include "nocturne_build_info.h"
#endif

// 自編版新增：Preferences → Tools → Nocturne 設定頁（2026-11-05）
// 跟 ESLyric 一樣放在 Tools 底下的獨立頁面，用 Columns UI 設定頁同一套分頁架構（PreferencesTabsHost），五個分頁：
// - 播放：進度條滾輪、輸出裝置顯示、音量條收成圖示
// - 下一首與佇列：展開頁角落的下一首提示（2026-11-02 做在 Columns UI 的分頁裡，11/05 搬到這裡）＋播放別首歌時保留播放佇列
//   （佇列 11/02 起放在這一頁底下，11/05 併進「播放」，11/06 獨立成分頁，11/08 使用者說一個選項一個分頁太浪費，又併回這裡）
// - 字型：開「主題字型」對話框（原本只在 View → Layout 選單裡）
// - 介面：細捲軸開關（11/08）、主視窗等網頁面板載好再顯示（開機優化 B，11/06）。
//   原本各自一個分頁、各只有一個設定，11/08 使用者要求合併成一頁（小標題分開）
// - 展開頁（2026-11-11 新增，第六個分頁）：封面虛化背景與亮度上限（從介面搬過來）、左上角按鈕的動作、歌詞防誤拖
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
// 資料夾樹（自編版 Album list）的搜尋框放上面還是下面（2026-11-09 使用者要求加開關）：存在 fb2k 共用設定，
// Album list 登記了變更通知，寫入就當場重排。鍵名、預設值要跟 Album list 的 main.cpp 一致；預設 false＝下方
namespace folder_search_position {
inline constexpr char config_key[] = "nocturne.folderTree.searchOnTop";
inline constexpr bool on_top_default = false;

bool is_on_top()
{
    return fb2k::configStore::get()->getConfigBool(config_key, on_top_default);
}

void set_on_top(bool on_top)
{
    fb2k::configStore::get()->setConfigBool(config_key, on_top);
}
} // namespace folder_search_position

// 暫時隱藏伴奏版（2026-11-11 使用者要求）：存在 fb2k 共用設定，自編 Album list 讀它、送「目前選取資料夾」時略過伴奏版，
// 也登記了變更通知，寫入就用目前的資料夾重送一次。鍵名、預設值要跟 Album list 的 playlist_utils.h 一致；預設 false＝不隱藏
namespace hide_instrumental {
inline constexpr char config_key[] = "nocturne.folderTree.hideInstrumental";
inline constexpr bool enabled_default = false;

bool is_enabled()
{
    return fb2k::configStore::get()->getConfigBool(config_key, enabled_default);
}

void set_enabled(bool enabled)
{
    fb2k::configStore::get()->setConfigBool(config_key, enabled);
}

// 關鍵字（2026-11-11 使用者要可以自己輸入，預設先填好）：分號分隔，比對檔名與歌名。鍵名、預設值要跟 Album list 一致
inline constexpr char keywords_config_key[] = "nocturne.folderTree.hideInstrumental.keywords";
inline constexpr char keywords_default[]
    = "instrumental;inst.;(inst;[inst;inst);inst];off vocal;off-vocal;offvocal;karaoke;\xE3\x82\xAB\xE3\x83\xA9\xE3\x82"
      "\xAA\xE3\x82\xB1;backing track;without vocal;\xE4\xBC\xB4\xE5\xA5\x8F"; // …;カラオケ;…;伴奏

std::string get_keywords()
{
    return fb2k::configStore::get()->getConfigString(keywords_config_key, keywords_default)->c_str();
}

void set_keywords(const std::string& keywords)
{
    if (keywords != get_keywords())
        fb2k::configStore::get()->setConfigString(keywords_config_key, keywords.c_str());
}

// 「自訂關鍵字」開關：關著（預設）Album list 一律用上面的預設值，開著才用輸入的關鍵字
inline constexpr char custom_config_key[] = "nocturne.folderTree.hideInstrumental.customKeywords";
inline constexpr bool custom_default = false;

bool is_custom()
{
    return fb2k::configStore::get()->getConfigBool(custom_config_key, custom_default);
}

void set_custom(bool custom)
{
    fb2k::configStore::get()->setConfigBool(custom_config_key, custom);
}
} // namespace hide_instrumental

// 2026-11-11：「展開頁」那一段（封面虛化背景）搬到新的「展開頁」分頁，這頁剩捲軸與資料夾樹、啟動兩段
class TabNocturneInterface : public NocturneTab {
public:
    TabNocturneInterface() : NocturneTab({IDC_TITLE1, IDC_TITLE2}) {}

    const char* get_name() override { return nocturne_text::interface_tab_name; }

private:
    UINT dialog_id() const override { return IDD_PREFS_NOCTURNE_APPEARANCE; }

    // 「平常隱藏」只對細捲軸有意義：沒勾細捲軸時變灰
    static void enable_autohide(HWND wnd, bool enabled)
    {
        EnableWindow(GetDlgItem(wnd, IDC_THIN_AUTOHIDE_VERTICAL), enabled);
        EnableWindow(GetDlgItem(wnd, IDC_THIN_AUTOHIDE_HORIZONTAL), enabled);
    }

    void on_init_dialog(HWND wnd) override
    {
        // 捲軸
        SetDlgItemText(wnd, IDC_TITLE1, nocturne_text::appearance_tab_title);
        SetDlgItemText(wnd, IDC_THIN_SCROLLBAR, nocturne_text::thin_scrollbar);
        SetDlgItemText(wnd, IDC_THIN_SCROLLBAR_NOTE, nocturne_text::thin_scrollbar_note);
        CheckDlgButton(wnd, IDC_THIN_SCROLLBAR, check_state(thin_scrollbar::is_enabled()));
        SetDlgItemText(wnd, IDC_THIN_AUTOHIDE_VERTICAL, nocturne_text::thin_autohide_vertical);
        SetDlgItemText(wnd, IDC_THIN_AUTOHIDE_HORIZONTAL, nocturne_text::thin_autohide_horizontal);
        CheckDlgButton(wnd, IDC_THIN_AUTOHIDE_VERTICAL, check_state(thin_scrollbar::is_autohide(true)));
        CheckDlgButton(wnd, IDC_THIN_AUTOHIDE_HORIZONTAL, check_state(thin_scrollbar::is_autohide(false)));
        enable_autohide(wnd, thin_scrollbar::is_enabled());

        // 資料夾樹搜尋框位置：0＝上方、1＝下方
        SetDlgItemText(wnd, IDC_FOLDER_SEARCH_POS_LABEL, nocturne_text::folder_search_position_label);
        const auto search_combo = GetDlgItem(wnd, IDC_FOLDER_SEARCH_POS);
        for (const auto& name : nocturne_text::folder_search_position_names)
            ComboBox_AddString(search_combo, name);
        ComboBox_SetCurSel(search_combo, folder_search_position::is_on_top() ? 0 : 1);

        // 播放清單框選方框
        SetDlgItemText(wnd, IDC_PLAYLIST_MARQUEE, nocturne_text::playlist_marquee);
        CheckDlgButton(wnd, IDC_PLAYLIST_MARQUEE, check_state(playlist_marquee::is_enabled()));

        // 啟動
        SetDlgItemText(wnd, IDC_TITLE2, nocturne_text::startup_tab_title);
        SetDlgItemText(wnd, IDC_STARTUP_HOLD, nocturne_text::startup_hold);
        SetDlgItemText(wnd, IDC_STARTUP_HOLD_NOTE, nocturne_text::startup_hold_note);
        CheckDlgButton(wnd, IDC_STARTUP_HOLD, check_state(startup_hold_until_ready.get()));
    }

    void on_command(HWND wnd, WPARAM wp) override
    {
        const auto id = LOWORD(wp);
        const auto code = HIWORD(wp);

        if (id == IDC_THIN_SCROLLBAR && code == BN_CLICKED) {
            const auto checked = IsDlgButtonChecked(wnd, IDC_THIN_SCROLLBAR) == BST_CHECKED;
            thin_scrollbar::set_enabled(checked);
            enable_autohide(wnd, checked);
        } else if ((id == IDC_THIN_AUTOHIDE_VERTICAL || id == IDC_THIN_AUTOHIDE_HORIZONTAL) && code == BN_CLICKED) {
            thin_scrollbar::set_autohide(id == IDC_THIN_AUTOHIDE_VERTICAL, IsDlgButtonChecked(wnd, id) == BST_CHECKED);
        } else if (id == IDC_FOLDER_SEARCH_POS && code == CBN_SELCHANGE) {
            const auto selection = ComboBox_GetCurSel(GetDlgItem(wnd, id));
            if (selection != CB_ERR)
                folder_search_position::set_on_top(selection == 0);
        } else if (id == IDC_PLAYLIST_MARQUEE && code == BN_CLICKED) {
            playlist_marquee::set_enabled(IsDlgButtonChecked(wnd, IDC_PLAYLIST_MARQUEE) == BST_CHECKED);
        } else if (id == IDC_STARTUP_HOLD && code == BN_CLICKED) {
            startup_hold_until_ready = IsDlgButtonChecked(wnd, IDC_STARTUP_HOLD) == BST_CHECKED;
        }
    }
};

// ---- 資料夾（2026-11-11）----
// 使用者要的：介面分頁太擠，另開一頁。預設只有「隱藏伴奏版」一個勾選；再勾「自訂關鍵字」才出現關鍵字輸入框
// （沒勾就用預設那組）。邏輯在自編 Album list 的 playlist_utils.cpp，改了馬上用目前選的資料夾重送一次
class TabNocturneFolder : public NocturneTab {
public:
    TabNocturneFolder() : NocturneTab({IDC_TITLE1}) {}

    const char* get_name() override { return nocturne_text::folder_tab_name; }

private:
    UINT dialog_id() const override { return IDD_PREFS_NOCTURNE_FOLDER; }

    // 「自訂關鍵字」只在勾了隱藏時能點；輸入框那一行只在勾了自訂時出現
    static void update_controls(HWND wnd)
    {
        const auto hide = IsDlgButtonChecked(wnd, IDC_HIDE_INSTRUMENTAL) == BST_CHECKED;
        const auto custom = IsDlgButtonChecked(wnd, IDC_HIDE_INSTRUMENTAL_CUSTOM) == BST_CHECKED;
        EnableWindow(GetDlgItem(wnd, IDC_HIDE_INSTRUMENTAL_CUSTOM), hide);
        for (const auto id : {IDC_HIDE_INSTRUMENTAL_KEYWORDS_LABEL, IDC_HIDE_INSTRUMENTAL_KEYWORDS, IDC_HIDE_INSTRUMENTAL_NOTE}) {
            const auto control = GetDlgItem(wnd, id);
            ShowWindow(control, custom ? SW_SHOW : SW_HIDE);
            EnableWindow(control, hide);
        }
    }

    // 關鍵字在輸入框離開時才存（每打一個字就存的話，資料夾樹會每次都重送一次清單）
    static void save_keywords(HWND wnd)
    {
        const auto edit = GetDlgItem(wnd, IDC_HIDE_INSTRUMENTAL_KEYWORDS);
        std::wstring text(static_cast<size_t>(GetWindowTextLength(edit)) + 1, L'\0');
        text.resize(static_cast<size_t>(GetWindowText(edit, text.data(), static_cast<int>(text.size()))));
        hide_instrumental::set_keywords(mmh::to_utf8(text));
    }

    void on_init_dialog(HWND wnd) override
    {
        SetDlgItemText(wnd, IDC_TITLE1, nocturne_text::hide_instrumental_title);
        SetDlgItemText(wnd, IDC_HIDE_INSTRUMENTAL, nocturne_text::hide_instrumental);
        SetDlgItemText(wnd, IDC_HIDE_INSTRUMENTAL_CUSTOM, nocturne_text::hide_instrumental_custom);
        SetDlgItemText(wnd, IDC_HIDE_INSTRUMENTAL_KEYWORDS_LABEL, nocturne_text::hide_instrumental_keywords_label);
        SetDlgItemText(wnd, IDC_HIDE_INSTRUMENTAL_NOTE, nocturne_text::hide_instrumental_note);
        SetDlgItemText(wnd, IDC_HIDE_INSTRUMENTAL_KEYWORDS, mmh::to_utf16(hide_instrumental::get_keywords()).c_str());
        CheckDlgButton(wnd, IDC_HIDE_INSTRUMENTAL, check_state(hide_instrumental::is_enabled()));
        CheckDlgButton(wnd, IDC_HIDE_INSTRUMENTAL_CUSTOM, check_state(hide_instrumental::is_custom()));
        update_controls(wnd);
    }

    void on_command(HWND wnd, WPARAM wp) override
    {
        const auto id = LOWORD(wp);
        const auto code = HIWORD(wp);

        if (id == IDC_HIDE_INSTRUMENTAL && code == BN_CLICKED) {
            save_keywords(wnd);
            hide_instrumental::set_enabled(IsDlgButtonChecked(wnd, IDC_HIDE_INSTRUMENTAL) == BST_CHECKED);
            update_controls(wnd);
        } else if (id == IDC_HIDE_INSTRUMENTAL_CUSTOM && code == BN_CLICKED) {
            save_keywords(wnd);
            hide_instrumental::set_custom(IsDlgButtonChecked(wnd, IDC_HIDE_INSTRUMENTAL_CUSTOM) == BST_CHECKED);
            update_controls(wnd);
        } else if (id == IDC_HIDE_INSTRUMENTAL_KEYWORDS && code == EN_KILLFOCUS) {
            save_keywords(wnd);
        }
    }
};

// ---- 展開頁（2026-11-11）----
// 使用者說設定放滿了就開新分頁、維持鬆弛度。三個小標題：
// - 背景：封面虛化背景＋亮度上限（原本在「介面」分頁，邏輯在 expanded_cover_bg.cpp）；沒勾時亮度上限整個藏起來
// - 顯示內容：左上角按鈕點一下是換下一種（預設）還是跳出選單（邏輯在 menu_items.cpp 的 on_now_playing_mode_button）
// - 歌詞：歌詞防誤拖（邏輯在 now_playing_cover_zoom.cpp 的 lyrics_lock）
// 都是改了馬上生效
class TabNocturneExpanded : public NocturneTab {
public:
    TabNocturneExpanded() : NocturneTab({IDC_TITLE1, IDC_TITLE2, IDC_TITLE3}) {}

    const char* get_name() override { return nocturne_text::expanded_page_tab_name; }

private:
    UINT dialog_id() const override { return IDD_PREFS_NOCTURNE_EXPANDED; }

    static void show_cap(HWND wnd, bool visible)
    {
        ShowWindow(GetDlgItem(wnd, IDC_EXPANDED_BG_CAP_LABEL), visible ? SW_SHOW : SW_HIDE);
        ShowWindow(GetDlgItem(wnd, IDC_EXPANDED_BG_CAP), visible ? SW_SHOW : SW_HIDE);
    }

    // 「展開頁要有」：外觀用跟下面一樣的下拉選單（2026-11-11 使用者說原本的按鈕是白底、跟下面的不一樣），
    // 裡面只放一項＝勾選的模式用頓號接起來；點下去（或 F4、Alt+↓、空白鍵）不開清單，改開可以勾選的選單（choose_modes）
    static void update_modes_text(HWND wnd)
    {
        using main_menu::NowPlayingMode;
        const auto mask = main_menu::now_playing_modes::get_mask();
        std::wstring text;
        const auto& modes = main_menu::now_playing_mode_order;
        for (size_t i = 0; i < std::size(modes); ++i) {
            if ((mask & main_menu::now_playing_modes::bit_of(modes[i])) == 0)
                continue;
            if (!text.empty())
                text += nocturne_text::modes_separator;
            text += nocturne_text::mode_short_names[main_menu::mode_name_index(modes[i])];
        }
        const auto combo = GetDlgItem(wnd, IDC_MODES);
        ComboBox_ResetContent(combo);
        ComboBox_AddString(combo, text.c_str());
        ComboBox_SetCurSel(combo, 0);
    }

    static LRESULT CALLBACK s_modes_combo_proc(
        HWND combo, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref_data)
    {
        switch (msg) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            SetFocus(combo);
            choose_modes(GetParent(combo));
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_F4 || wp == VK_SPACE || wp == VK_RETURN) {
                choose_modes(GetParent(combo));
                return 0;
            }
            if (wp == VK_UP || wp == VK_DOWN || wp == VK_HOME || wp == VK_END || wp == VK_PRIOR || wp == VK_NEXT)
                return 0;
            break;
        case WM_SYSKEYDOWN:
            if (wp == VK_DOWN || wp == VK_UP) {
                choose_modes(GetParent(combo));
                return 0;
            }
            break;
        case WM_MOUSEWHEEL:
            return 0;
        case WM_NCDESTROY:
            RemoveWindowSubclass(combo, s_modes_combo_proc, id);
            break;
        }
        return DefSubclassProc(combo, msg, wp, lp);
    }

    // 按下拉鈕：在按鈕正下方開一個可以勾選的選單；點一項就切換勾選並馬上生效，選單再開一次，點選單外面才關。
    // 最後一個勾選的不能取消（至少要留一種）。沒有 VU 元件時 VU 表一樣可以勾（記住喜好），只是註明沒有安裝
    static void choose_modes(HWND wnd)
    {
        using main_menu::NowPlayingMode;
        const auto button = GetDlgItem(wnd, IDC_MODES);
        RECT rect{};
        GetWindowRect(button, &rect);
        const auto vu_available = main_menu::is_vu_mode_available();

        for (;;) {
            const wil::unique_hmenu menu(CreatePopupMenu());
            if (!menu)
                return;
            const auto mask = main_menu::now_playing_modes::get_mask();
            const auto& modes = main_menu::now_playing_mode_order;
            for (size_t i = 0; i < std::size(modes); ++i) {
                const auto available = vu_available || !main_menu::mode_needs_lyrics_column(modes[i]);
                const auto checked = (mask & main_menu::now_playing_modes::bit_of(modes[i])) != 0;
                const auto text = nocturne_text::mode_short_names[main_menu::mode_name_index(modes[i], available)];
                AppendMenu(menu.get(), MF_STRING | (checked ? MF_CHECKED : MF_UNCHECKED), i + 1, text);
            }

            const auto chosen = TrackPopupMenu(menu.get(), TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN,
                rect.left, rect.bottom, 0, wnd, nullptr);
            if (chosen < 1 || chosen > static_cast<int>(std::size(modes)))
                return;

            const auto bit = main_menu::now_playing_modes::bit_of(modes[chosen - 1]);
            const auto next = mask ^ bit;
            if (next == 0) {
                MessageBeep(MB_OK);
                continue;
            }
            main_menu::now_playing_modes::set_mask(next);
            main_menu::on_now_playing_modes_changed();
            update_modes_text(wnd);
        }
    }

    void on_init_dialog(HWND wnd) override
    {
        // 背景
        SetDlgItemText(wnd, IDC_TITLE1, nocturne_text::expanded_section_background);
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

        // 顯示內容：0＝換下一種、1＝跳出選單
        SetDlgItemText(wnd, IDC_TITLE2, nocturne_text::expanded_section_content);
        SetDlgItemText(wnd, IDC_MODE_BUTTON_LABEL, nocturne_text::mode_button_label);
        SetDlgItemText(wnd, IDC_MODE_NOTE, nocturne_text::mode_note);
        SetDlgItemText(wnd, IDC_MODES_LABEL, nocturne_text::modes_label);
        update_modes_text(wnd);
        SetWindowSubclass(GetDlgItem(wnd, IDC_MODES), s_modes_combo_proc, 1, 0);
        const auto mode_combo = GetDlgItem(wnd, IDC_MODE_BUTTON);
        for (const auto& name : nocturne_text::mode_button_names)
            ComboBox_AddString(mode_combo, name);
        ComboBox_SetCurSel(mode_combo, main_menu::now_playing_mode_button::opens_menu() ? 1 : 0);

        // 歌詞
        SetDlgItemText(wnd, IDC_TITLE3, nocturne_text::expanded_section_lyrics);
        SetDlgItemText(wnd, IDC_LYRICS_LOCK, nocturne_text::lyrics_lock);
        SetDlgItemText(wnd, IDC_LYRICS_LOCK_NOTE, nocturne_text::lyrics_lock_note);
        CheckDlgButton(wnd, IDC_LYRICS_LOCK, check_state(lyrics_lock::is_enabled()));
    }

    void on_command(HWND wnd, WPARAM wp) override
    {
        const auto id = LOWORD(wp);
        const auto code = HIWORD(wp);

        if (id == IDC_EXPANDED_BG && code == BN_CLICKED) {
            const auto checked = IsDlgButtonChecked(wnd, IDC_EXPANDED_BG) == BST_CHECKED;
            expanded_cover_bg::enabled = checked;
            show_cap(wnd, checked);
        } else if (id == IDC_EXPANDED_BG_CAP && code == CBN_SELCHANGE) {
            const auto selection = ComboBox_GetCurSel(GetDlgItem(wnd, id));
            if (selection != CB_ERR)
                expanded_cover_bg::cap_percent
                    = expanded_cover_bg::cap_min + gsl::narrow<uint32_t>(selection) * expanded_cover_bg::cap_step;
        } else if (id == IDC_MODE_BUTTON && code == CBN_SELCHANGE) {
            const auto selection = ComboBox_GetCurSel(GetDlgItem(wnd, id));
            if (selection != CB_ERR)
                main_menu::now_playing_mode_button::set_opens_menu(selection == 1);
        } else if (id == IDC_LYRICS_LOCK && code == BN_CLICKED) {
            lyrics_lock::set_enabled(IsDlgButtonChecked(wnd, IDC_LYRICS_LOCK) == BST_CHECKED);
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
        std::wstring version = std::wstring(nocturne_text::about_version_prefix) + nocturne_text::nocturne_version;
        // 開發版（219、169）與個人版（主環境）後面加上種類與建置日期；公開版與朋友版只顯示版本號
#if defined(NOCTURNE_BUILD_DEV) || defined(NOCTURNE_BUILD_PERSONAL)
#if defined(NOCTURNE_BUILD_DEV)
        version += nocturne_text::about_edition_dev;
#else
        version += nocturne_text::about_edition_personal;
#endif
        version += L" · " NOCTURNE_BUILD_DATE;
#endif
        SetDlgItemText(wnd, IDC_ABOUT_VERSION, version.c_str());
    }

    void on_command(HWND, WPARAM) override {}
};

TabNocturnePlayback g_tab_nocturne_playback;
TabNextTrackToast g_tab_next_track_toast;
TabNocturneFont g_tab_nocturne_font;
TabNocturneInterface g_tab_nocturne_interface;
TabNocturneExpanded g_tab_nocturne_expanded;
TabNocturneFolder g_tab_nocturne_folder;
TabNocturneAbout g_tab_nocturne_about;

// 分頁順序：播放、下一首與佇列、字型、介面、關於。
// 存的「最後開啟的分頁」是這個順序的編號，所以合併或插入分頁（2026-11-06 播放佇列插在播放後面、11/08 外觀與展開頁插在關於前面、
// 11/08 晚把播放佇列／啟動／外觀／展開頁併成現在這五頁）之後，已經用過的人更新後第一次開設定頁可能停在不同的分頁
// （編號超出範圍時由分頁架構處理），無傷大雅；之後新增分頁盡量加在最後面（關於之前）
// 2026-11-11 在關於之前加了「展開頁」，之後又在關於之前加了「資料夾」
PreferencesTab* g_nocturne_tabs[] = {&g_tab_nocturne_playback, &g_tab_next_track_toast, &g_tab_nocturne_font,
    &g_tab_nocturne_interface, &g_tab_nocturne_expanded, &g_tab_nocturne_folder, &g_tab_nocturne_about};

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
    thin_scrollbar::set_autohide(true, thin_scrollbar::autohide_default);
    thin_scrollbar::set_autohide(false, thin_scrollbar::autohide_default);
    folder_search_position::set_on_top(folder_search_position::on_top_default);
    hide_instrumental::set_enabled(hide_instrumental::enabled_default);
    hide_instrumental::set_keywords(hide_instrumental::keywords_default);
    hide_instrumental::set_custom(hide_instrumental::custom_default);
    playlist_marquee::set_enabled(playlist_marquee::enabled_default);

    expanded_cover_bg::enabled.reset();
    expanded_cover_bg::cap_percent.reset();
    main_menu::now_playing_mode_button::set_opens_menu(main_menu::now_playing_mode_button::opens_menu_default);
    main_menu::now_playing_modes::set_mask(main_menu::now_playing_modes::all_modes);
    main_menu::on_now_playing_modes_changed();
    lyrics_lock::set_enabled(lyrics_lock::enabled_default);
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
