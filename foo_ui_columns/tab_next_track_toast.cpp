#include "pch.h"
#include "config.h"
#include "next_track_toast.h"
#include "nocturne_text.h"
#include "playlist_item_helpers.h"

// 自編版新增：「下一首提示」設定（2026-11-02 做在 Columns UI 的分頁裡；2026-11-05 起搬到 Preferences → Tools → Nocturne，見檔案後半）
// 設定都存在 next_track_toast 的設定變數裡，改了立刻生效；啟用開關跟 View → Layout → 下一首提示 是同一個設定
// 對話框的文字依語言在 WM_INITDIALOG 填入（.rc 裡只放版面）

namespace cui::prefs {

namespace {

class TabNextTrackToast : public PreferencesTab {
public:
    INT_PTR ConfigProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg) {
        case WM_INITDIALOG:
            on_init_dialog(wnd);
            break;
        case WM_COMMAND:
            on_command(wnd, wp);
            break;
        }
        return 0;
    }

    HWND create(HWND wnd) override
    {
        return m_helper.create(wnd, IDD_PREFS_NEXT_TRACK_TOAST,
            [this](auto&&... args) { return ConfigProc(std::forward<decltype(args)>(args)...); });
    }

    const char* get_name() override { return nocturne_text::next_toast_tab_name; }

private:
    static uint32_t clamp_seconds(uint32_t value)
    {
        return std::clamp(value, next_track_toast::min_seconds, next_track_toast::max_seconds);
    }

    void on_init_dialog(HWND wnd)
    {
        // 填值時控制項會送出變更通知，先擋掉，免得把還沒填完的值寫回設定
        m_initialising = true;

        SetDlgItemText(wnd, IDC_TITLE1, nocturne_text::next_toast_title);
        SetDlgItemText(wnd, IDC_NEXT_TOAST_ENABLED, nocturne_text::next_toast_enabled);
        SetDlgItemText(wnd, IDC_NEXT_TOAST_SECONDS_LABEL, nocturne_text::next_toast_seconds_label);
        SetDlgItemText(wnd, IDC_NEXT_TOAST_SHOW_SECONDS, nocturne_text::next_toast_show_seconds);
        SetDlgItemText(wnd, IDC_NEXT_TOAST_SHOW_COVER, nocturne_text::next_toast_show_cover);
        SetDlgItemText(wnd, IDC_NEXT_TOAST_SHOW_TAG, nocturne_text::next_toast_show_tag);
        SetDlgItemText(wnd, IDC_NEXT_TOAST_POSITION_LABEL, nocturne_text::next_toast_position_label);
        SetDlgItemText(wnd, IDC_NEXT_TOAST_NOTE, nocturne_text::next_toast_note);
        SetDlgItemText(wnd, IDC_TITLE2, nocturne_text::keep_queue_title);
        SetDlgItemText(wnd, IDC_KEEP_QUEUE_ON_PLAY, nocturne_text::keep_queue_on_play);

        const auto check = [wnd](int id, bool checked) { CheckDlgButton(wnd, id, checked ? BST_CHECKED : BST_UNCHECKED); };
        check(IDC_NEXT_TOAST_ENABLED, next_track_toast::enabled.get());
        check(IDC_NEXT_TOAST_SHOW_SECONDS, next_track_toast::show_seconds_in_ring.get());
        check(IDC_NEXT_TOAST_SHOW_COVER, next_track_toast::show_cover.get());
        check(IDC_NEXT_TOAST_SHOW_TAG, next_track_toast::show_source_tag.get());
        check(IDC_KEEP_QUEUE_ON_PLAY, playlist_item_helpers::keep_queue_on_play.get());

        const auto spin = GetDlgItem(wnd, IDC_NEXT_TOAST_SECONDS_SPIN);
        SendMessage(spin, UDM_SETRANGE32, next_track_toast::min_seconds, next_track_toast::max_seconds);
        SendMessage(spin, UDM_SETPOS32, 0, clamp_seconds(next_track_toast::seconds.get()));

        const auto position_combo = GetDlgItem(wnd, IDC_NEXT_TOAST_POSITION);
        for (const auto& name : nocturne_text::next_toast_position_names)
            ComboBox_AddString(position_combo, name);
        ComboBox_SetCurSel(
            position_combo, gsl::narrow<int>(std::min(next_track_toast::position.get(), next_track_toast::position_count - 1)));

        m_initialising = false;
    }

    void on_command(HWND wnd, WPARAM wp)
    {
        if (m_initialising)
            return;

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
        case IDC_KEEP_QUEUE_ON_PLAY:
            if (code == BN_CLICKED)
                playlist_item_helpers::keep_queue_on_play = is_checked();
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

    bool m_initialising{};
    PreferencesTabHelper m_helper{{IDC_TITLE1, IDC_TITLE2}};
};

TabNextTrackToast g_tab_next_track_toast;

// Preferences → Tools → Nocturne（2026-11-05）：跟 ESLyric 一樣放在 Tools 底下的獨立頁面
// 自編版自己新增的設定（目前是「下一首提示」與「播放佇列」）集中在這一頁，不再放在 Columns UI 的分頁裡
// 頁面內容就是上面那個對話框，直接當頁面的視窗（不經過 Columns UI 的分頁列，因為只有一頁）
class NocturnePreferencesInstance : public preferences_page_instance {
public:
    explicit NocturnePreferencesInstance(HWND parent)
        : m_wnd(g_tab_next_track_toast.create(parent))
        // 跟 PreferencesInstanceTabsHost 一樣：fb2k 有提供深色模式就宣告這一頁支援（對話框本身在建立時已掛上深色模式）
        , m_has_dark_mode(fbh::create_dark_mode_obj().is_valid())
    {
    }

    t_uint32 get_state() override { return m_has_dark_mode ? preferences_state::dark_mode_supported : 0; }
    fb2k::hwnd_t get_wnd() override { return m_wnd; }
    // 所有設定都是改了立刻生效，沒有「套用」要做；也沒有「重設」
    void apply() override {}
    void reset() override {}

private:
    HWND m_wnd{};
    bool m_has_dark_mode{};
};

// {67EE972D-3FB3-4607-BDAF-CE86327347D0}
constexpr GUID guid_nocturne_preferences_page
    = {0x67ee972d, 0x3fb3, 0x4607, {0xbd, 0xaf, 0xce, 0x86, 0x32, 0x73, 0x47, 0xd0}};

class NocturnePreferencesPage : public preferences_page_v3 {
public:
    // 頁面名稱是品牌名，中英文版相同
    const char* get_name() override { return "Nocturne"; }
    GUID get_guid() override { return guid_nocturne_preferences_page; }
    GUID get_parent_guid() override { return preferences_page::guid_tools; }
    bool reset_query() override { return false; }
    void reset() override {}
    bool get_help_url(pfc::string_base& p_out) override { return false; }

    preferences_page_instance::ptr instantiate(fb2k::hwnd_t parent, preferences_page_callback::ptr callback) override
    {
        return fb2k::service_new<NocturnePreferencesInstance>(parent);
    }
};

service_factory_single_t<NocturnePreferencesPage> g_nocturne_preferences_page;

} // namespace

PreferencesTab* g_get_tab_next_track_toast()
{
    return &g_tab_next_track_toast;
}

} // namespace cui::prefs
