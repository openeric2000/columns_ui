#pragma once
#include "fb2k_keyboard.h"

namespace cui::panels::splitter {

constexpr auto max_panel_dimension = 0x7fff;

// 自編版新增（View/Layout/切換播放頁）：主內容與播放列兩個面板的自訂標題
// 播放列面板的子視窗（ESLyric）另外會掛 s_now_playing_bar_subclass_proc
inline constexpr std::string_view now_playing_main_title = "主內容";
inline constexpr std::string_view now_playing_bar_title = "播放列";
// 自編版新增：播放列高度固定為 ESLyric 佈局 main.xml 播放列（bar）的高度 138 DIP，改了那邊要跟著改
// 版面裡存的高度會跟著存檔時的 DPI 換算而有誤差（219 實測少約 5px），播放列頂端的進度條就會被切掉
inline constexpr int now_playing_bar_height = 138;
// 自編版新增（展開頁樣式 2）：同一層自訂標題「展開頁右側」的面板（VU 表），展開且樣式 2 時疊到展開頁右側
inline constexpr std::string_view now_playing_side_title = "展開頁右側";
// 自編版新增（展開頁動畫）：展開時視窗區域的上緣從播放列往上推、收合時往下蓋回的時間長度
// 2026-10-28 使用者在示意頁選 A 款；280ms 實測「不夠絲滑」，改 400ms 並換平緩的曲線（見 get_now_playing_animation_rect）
inline constexpr int now_playing_animation_duration_ms = 400;
// 下一格動畫的訊息：每格畫完、等螢幕顯示出來就送下一個，不靠計時器
// （計時器最快約 15.6ms 一次，主螢幕 240Hz 時看得出跟不上）
inline constexpr UINT msg_now_playing_animation_frame = WM_USER + 0x4e5;

enum Orientation {
    horizontal,
    vertical,
};

class FlatSplitterPanel : public uie::container_uie_window_v3_t<uie::splitter_window_v3> {
public:
    virtual Orientation get_orientation() const = 0;
    static int g_get_caption_size();
    void get_category(pfc::string_base& p_out) const override;
    unsigned get_type() const override;

    void insert_panel(size_t index, const uie::splitter_item_t* p_item) override;

    void remove_panel(size_t index) override;
    void replace_panel(size_t index, const uie::splitter_item_t* p_item) override;
    void reorder_panels(const size_t* order, size_t count) override;

    size_t get_panel_count() const override;
    uie::splitter_item_t* get_panel(size_t index) const override;
    enum {
        stream_version_current = 0
    };

    void set_config(stream_reader* config, size_t p_size, abort_callback& p_abort) override;
    void import_config(stream_reader* p_reader, size_t p_size, abort_callback& p_abort) override;
    void export_config(stream_writer* p_writer, abort_callback& p_abort) const override;

    void get_config(stream_writer* out, abort_callback& p_abort) const override;

    bool is_index_valid(size_t index) const;

    bool get_config_item_supported(size_t index, const GUID& p_type) const override;

    bool get_config_item(
        size_t index, const GUID& p_type, stream_writer* p_out, abort_callback& p_abort) const override;

    bool set_config_item(size_t index, const GUID& p_type, stream_reader* p_source, abort_callback& p_abort) override;

    class FlatSplitterPanelHost : public ui_extension::window_host_ex {
    public:
        FlatSplitterPanelHost() {}
        explicit FlatSplitterPanelHost(FlatSplitterPanel* window_instance) : m_this(window_instance) {}

        const GUID& get_host_guid() const override;

        bool get_keyboard_shortcuts_enabled() const override;
        void get_children(pfc::list_base_t<window::ptr>& p_out) override;

        void on_size_limit_change(HWND wnd, unsigned flags) override;

        // unsigned get_orientation();
        Orientation get_orientation() const;

        unsigned is_resize_supported(HWND wnd) const override;

        bool request_resize(HWND wnd, unsigned flags, unsigned width, unsigned height) override;

        bool override_status_text_create(service_ptr_t<ui_status_text_override>& p_out) override;

        bool is_visible(HWND wnd) const override;

        bool is_visibility_modifiable(HWND wnd, bool desired_visibility) const override;
        bool set_window_visibility(HWND wnd, bool visibility) override;

        void relinquish_ownership(HWND wnd) override;

    private:
        service_ptr_t<FlatSplitterPanel> m_this;
    };

    bool is_point_ours(HWND wnd_point, const POINT& pt_screen, pfc::list_base_t<window::ptr>& p_hierarchy) override;
    void get_supported_panels(
        const pfc::list_base_const_t<window::ptr>& p_windows, bit_array_var& p_mask_unsupported) override;

    static void s_on_size_change();

    // 自編版新增（View/Layout/切換播放頁）：直向分割面板裡同一層有自訂標題「主內容」「播放列」兩個面板時，
    // 播放列的容器一直撐滿整個分割面板、疊在最上層，收合時用視窗區域只露出排版給它的位置，展開時拿掉區域整塊蓋住主內容。
    // 切換時沒有任何視窗移動或改變大小（改大小時 ESLyric 會先貼一格舊畫面、主內容要重新排版，都會閃）。
    // 展開狀態只存在記憶體，重開一律收合
    static bool s_toggle_now_playing();
    static bool s_is_now_playing_expanded();
    // 展開頁樣式改變時重新排版，讓「展開頁右側」面板跟著顯示或隱藏
    static void s_refresh_now_playing();
    // 給標題格式欄位用（可能在別的執行緒讀），跟 m_now_playing_expanded 同步
    static std::atomic<bool> s_now_playing_expanded_for_fields;

private:
    struct SizeLimit {
        int min_height{};
        int max_height{};
        int min_width{};
        int max_width{};
        SizeLimit() = default;
    };
    class Panel : public std::enable_shared_from_this<Panel> {
    public:
        class PanelContainer : fbh::LowLevelMouseHookManager::HookCallback {
        public:
            enum {
                MSG_AUTOHIDE_END = WM_USER + 2
            };

            explicit PanelContainer(Panel* p_panel);

            ~PanelContainer();

            HWND create(HWND parent) const;
            void destroy() const;
            HWND get_wnd() const { return m_wnd; }

            void set_window_ptr(FlatSplitterPanel* p_ptr);
            void enter_autohide_hook();
            // private:
            LRESULT on_message(HWND wnd, UINT msg, WPARAM wp, LPARAM lp);
            void on_hooked_message(WPARAM msg, const MSLLHOOKSTRUCT& mllhs) override;
            service_ptr_t<FlatSplitterPanel> m_this;

            wil::unique_htheme m_theme;
            Panel* m_panel;

            bool m_hook_active;
            bool m_timer_active;

        private:
            void reopen_theme();
            void close_theme();
            bool test_autohide_window(HWND wnd);

            inline constexpr static auto class_name = L"foo_ui_columns_splitter_panel_child_container";

            HWND m_wnd{};
            std::unique_ptr<uie::container_window_v3> m_window;
            std::unique_ptr<colours::dark_mode_notifier> m_dark_mode_notifier;
        } m_container;

        GUID m_guid{};
        unsigned m_caption_orientation{NULL};
        bool m_locked{false};
        bool m_hidden{false};
        bool m_autohide{false};
        HWND m_wnd{nullptr};
        HWND m_wnd_child{nullptr};
        bool m_show_caption{};
        pfc::array_t<uint8_t> m_child_data;
        SizeLimit m_size_limits;
        uie::window_ptr m_child;
        bool m_show_toggle_area{false};
        bool m_use_custom_title{false};
        pfc::string8 m_custom_title;

        uih::IntegerAndDpi<uint32_t> m_size{150};

        bool has_custom_title(std::string_view title) const;

        uie::splitter_item_full_v2_t* create_splitter_item(bool b_set_ptr = true);

        void set_from_splitter_item(const uie::splitter_item_t* p_source);

        void refresh_child_data(abort_callback& aborter = fb2k::noAbort);
        void _export(stream_writer* out, abort_callback& p_abort);
        void write(stream_writer* out, abort_callback& p_abort);
        void import(stream_reader* t, abort_callback& p_abort);
        void read(stream_reader* t, abort_callback& p_abort);
        void read_extra(stream_reader* reader, abort_callback& p_abort);
        void write_extra(stream_writer* writer, abort_callback& p_abort);

        void set_hidden(bool val);

        void on_size();
        void on_size(int cx, int cy);

        void destroy();
        Panel();

        using Ptr = std::shared_ptr<Panel>;
    };

    void read_config(stream_reader* p_reader, size_t p_size, bool is_import, abort_callback& p_abort);
    void write_config(stream_writer* p_writer, bool is_export, abort_callback& p_abort) const;

    void start_autohide_dehide(size_t index, bool b_next_too = true);

    LRESULT on_message(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) override;
    void get_panels_sizes(unsigned width, unsigned height, pfc::list_base_t<unsigned>& p_out);
    bool find_by_divider_pt(POINT& pt, size_t& p_out);
    bool test_divider_pt(const POINT& pt, size_t p_out);

    int get_panel_divider_size(size_t index) const;

    void on_size_changed(unsigned width, unsigned height);
    void on_size_changed();
    void save_sizes();

    void save_sizes(unsigned width, unsigned height);

    bool can_resize_divider(size_t index) const;
    bool can_resize_panel(size_t index) const;
    int override_size(const size_t panel, int delta);

    void refresh_children();
    void destroy_children();

    std::vector<Panel::Ptr>::iterator find_panel_by_container_wnd(HWND wnd);
    std::vector<Panel::Ptr>::iterator find_panel_by_panel_wnd(HWND wnd);
    Panel::Ptr find_panel_by_custom_title(std::string_view title) const;
    // 直向且同一層有主內容與播放列兩個面板時傳回播放列，否則 nullptr
    Panel::Ptr get_now_playing_bar_panel() const;
    // 有播放列時，同一層自訂標題「展開頁右側」的面板，否則 nullptr
    Panel::Ptr get_now_playing_side_panel() const;
    // 自編版新增（展開頁動畫）：從目前看得到的區域開始，往 m_now_playing_expanded 的最終區域移動
    void start_now_playing_animation();
    void on_now_playing_animation_frame();
    // 動畫目前這一刻的播放列視窗區域
    RECT get_now_playing_animation_rect(unsigned width, unsigned height) const;
    // 自編版新增（展開頁動畫 B 款）：展開頁截圖整頁從播放列後面滑上來／滑下去（見 start_now_playing_animation）
    bool start_now_playing_slide();
    void on_now_playing_slide_frame();
    // 截圖視窗目前往下偏移多少：0＝蓋住整個展開頁，展開頁高度＝整個收到播放列後面
    int get_now_playing_slide_offset() const;
    // 滑完：展開時確定底下是真的展開頁，再拿掉截圖
    void finish_now_playing_slide();

    service_ptr_t<FlatSplitterPanelHost> m_window_host;
    std::vector<Panel::Ptr> m_panels;
    HWND m_wnd{nullptr};

    bool m_refresh_children_in_progress{};
    bool m_now_playing_expanded{};
    // 自編版新增（展開頁動畫）：收合時的視窗區域（排版給播放列的位置，on_size_changed 時更新）、動畫的起點與開始時間
    RECT m_now_playing_bar_rect{};
    bool m_now_playing_animating{};
    RECT m_now_playing_animation_from{};
    std::chrono::steady_clock::time_point m_now_playing_animation_start;
    // 每格重畫時間的移動平均（毫秒），決定每格等幾次螢幕更新；同一次動畫裡只往上調。還沒畫第一格時是空的
    std::optional<double> m_now_playing_animation_paint_average;
    int m_now_playing_animation_refreshes_per_frame{1};
    // B 款：截圖視窗（展開時固定蓋住展開頁、收合時跟著往下移），
    // 以及這一段滑動的開始時間、展開頁在螢幕上的位置、起點偏移與方向
    HWND m_now_playing_snapshot_wnd{};
    std::chrono::steady_clock::time_point m_now_playing_snapshot_start;
    RECT m_now_playing_slide_rect{};
    int m_now_playing_slide_from{};
    bool m_now_playing_slide_expanding{};
    int m_last_position{};
    size_t m_panel_dragging{};
    bool m_panel_dragging_valid{false};
    std::unique_ptr<colours::dark_mode_notifier> m_dark_mode_notifier;
    std::optional<uih::BufferedPaintInitialiser> m_buffered_paint_initialiser;
    fb2k_utils::SimpleKeyboardShortcutProcessor m_keyboard_shortcut_processor;

    static wil::unique_hfont g_font_menu_horizontal;
    static wil::unique_hfont g_font_menu_vertical;
    static unsigned g_count;
    static pfc::ptr_list_t<FlatSplitterPanel> g_instances;

    // 自編版新增（切換播放頁）：上次切換的時間（GetTickCount64），給播放列的雙擊處理判斷第一下有沒有觸發切換
    static ULONGLONG s_last_now_playing_toggle_tick;

    static LRESULT CALLBACK s_now_playing_bar_subclass_proc(
        HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref_data);

public:
    FlatSplitterPanel() = default;
};

} // namespace cui::panels::splitter
