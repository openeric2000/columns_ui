#pragma once

// 自編版新增（2026-11-14）：右鍵挑選卡共用的「分段切換」
// 使用者在預覽頁（https://claude.ai/artifact/FXUzUwZjCEZe4t2A4rQdTS）選 D 分段切換＋等級 2 動畫＋按住拖曳＋卡片開關淡入淡出：
// - 一條淡色軌道，選中的那段鋪亮底（白 14%，拖曳中 20%）；點別段亮底 160 ms 滑過去（先快後慢）
// - 軌道下面的說明行換字時交叉淡化 180 ms，新字從點的方向移進來 10 DIP
// - 按住拖曳：亮底跟著滑鼠，經過哪一段就先通知（預覽），放開才吸過去並選定；拖出兩端只跟四分之一
// - 卡片開：淡入＋0.97→1 放大 140 ms；關：淡出 100 ms
// - Windows「動畫效果」關掉時全部直接跳到結果；開發版可用環境變數 NOCTURNE_ANIM_SLOW＝倍數放慢（截中間格用）
// 用在封面下一排（cover_line.cpp）、VU 表／頻譜樣式卡（now_playing_vu_meter.cpp）

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace cui::nocturne_segment {

using Clock = std::chrono::steady_clock;

// 時間（毫秒）與外觀（DIP）
inline constexpr float pill_ms = 160.f;
inline constexpr float caption_ms = 180.f;
inline constexpr float open_ms = 140.f;
inline constexpr float close_ms = 100.f;
inline constexpr float open_scale_from = 0.97f;
inline constexpr float caption_shift_dip = 10.f;
inline constexpr float drag_threshold_dip = 4.f;
inline constexpr float track_pad_dip = 3.f;
inline constexpr float cell_gap_dip = 2.f;
inline constexpr float cell_h_dip = 32.f;
inline constexpr float row_h_dip = cell_h_dip + 2 * track_pad_dip;
inline constexpr float track_radius_dip = 9.f;
inline constexpr float pill_radius_dip = 7.f;
inline constexpr float label_font_dip = 14.5f;
inline constexpr float caption_font_dip = 15.f;

// 卡片要放在哪（2026-11-14 使用者要求：卡片不可超出 fb2k 視窗）
// 範圍＝owner 所在主視窗的內容區與螢幕工作區的交集；card＝卡片本體在卡片視窗裡的位置大小（陰影留白不算）。
// prefer＝卡片本體左上角想放的螢幕座標；本體下緣超出範圍就改成本體下緣貼著 flip_bottom（往上翻），最後再夾進範圍。
// 回傳卡片視窗（含陰影留白）該放的左上角。
POINT place_card(HWND owner, POINT prefer, LONG flip_bottom, const Gdiplus::RectF& card);

// 開卡時讀一次：系統動畫設定、開發版的放慢倍數
void refresh_settings();
bool animations_enabled();
float slow_factor();

// 會動的數值：go() 從目前值接著跑向新目標（連點很快時不會先跑完上一段）
class Tween {
public:
    void set(float value);
    void go(float target, float duration_ms);
    // 更新目前值；回傳是否還在跑
    bool update(Clock::time_point now);
    float value() const { return m_value; }
    float target() const { return m_to; }
    bool running() const { return m_running; }

private:
    float m_from{};
    float m_to{};
    float m_value{};
    float m_duration{};
    Clock::time_point m_start{};
    bool m_running{};
};

// 文字顏色（244,240,230）乘上不透明度
Gdiplus::Color ink(double alpha);
std::unique_ptr<Gdiplus::FontFamily> theme_family();
Gdiplus::FontStyle regular_style(const Gdiplus::FontFamily& family);
void add_rounded(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& rect, float radius);
// 在方框裡置中畫一行字（太長出現刪節號）
void draw_centered(Gdiplus::Graphics& g, const std::wstring& text, const Gdiplus::Font& font, const Gdiplus::Color& colour,
    const Gdiplus::RectF& box);
void draw_left(Gdiplus::Graphics& g, const std::wstring& text, const Gdiplus::Font& font, const Gdiplus::Color& colour,
    const Gdiplus::RectF& box);

// ---- 一條分段 ----
struct Row {
    std::vector<std::wstring> labels;
    int selected{};  // 目前存的選擇
    int shown{};     // 字最亮的那段（拖曳中＝經過的段）
    int hover{-1};
    Gdiplus::RectF track;
    std::vector<Gdiplus::RectF> cells;
    Tween pill;  // 亮底左緣 x
    // 拖曳
    bool pressed{};
    bool dragging{};
    float press_x{};
    float press_y{};
    float grab{};
};

// 依軌道位置排出每一段；亮底直接放在 selected（不動畫）
void layout(Row& row, const Gdiplus::RectF& track, float k);
int cell_at(const Row& row, float x, float y);
void draw(Gdiplus::Graphics& g, const Row& row, const Gdiplus::Font& font, float k);
// 程式換選擇（設定從別處改了）：亮底滑過去
void select(Row& row, int index, bool animate);
bool update(Row& row, Clock::time_point now);
// 滑鼠：mouse_down 在軌道內才接（回傳 true，呼叫端要 SetCapture）；
// mouse_move 拖曳中經過新的段時呼叫 on_preview；mouse_up 選定時呼叫 on_commit（點同一段或拖回原段不呼叫）
bool mouse_down(Row& row, float x, float y);
void mouse_move(Row& row, float x, float k, const std::function<void(int)>& on_preview);
void mouse_up(Row& row, const std::function<void(int)>& on_commit);
// 拖曳被中斷（卡片要關、失去捕捉）：亮底回到原本的選擇
void cancel_drag(Row& row);

// ---- 說明行：換字交叉淡化 ----
struct Caption {
    std::wstring old_text;
    std::wstring new_text;
    int dir{1};
    Tween t;  // 0＝只有舊字、1＝只有新字
};
void caption_set(Caption& caption, const std::wstring& text, int dir, bool animate);
void draw_caption(Gdiplus::Graphics& g, const Caption& caption, const Gdiplus::RectF& box, const Gdiplus::Font& font,
    double alpha, float k);
bool caption_update(Caption& caption, Clock::time_point now);

// ---- 縮圖底板：二維滑動（頻譜兩排時可以斜著滑）----
struct Plate {
    Tween x;
    Tween y;
};
void plate_place(Plate& plate, const Gdiplus::RectF& cell, bool animate);
bool plate_update(Plate& plate, Clock::time_point now);

// ---- 卡片開關 ----
// 卡片每次重畫後把整張畫面交給 present：開卡動畫期間以 anchor（卡片左上角）為中心縮放並淡入，關卡時淡出
class Presenter {
public:
    Presenter() = default;
    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;
    ~Presenter()
    {
        if (m_dc && m_old_bitmap)
            SelectObject(m_dc.get(), m_old_bitmap);
    }

    void begin_open();
    void begin_close();
    bool closing() const { return m_closing; }
    // 淡出跑完了（呼叫端該 DestroyWindow）
    bool closed() const { return m_closing && !m_alpha.running(); }
    bool update(Clock::time_point now);
    void present(HWND wnd, const uint32_t* pixels, int width, int height, Gdiplus::PointF anchor);

private:
    Tween m_alpha;
    Tween m_scale;
    bool m_closing{};
    // 畫面緩衝重複使用（每格都重建 DIB 太慢，240 Hz 一格只有約 4 ms）
    wil::unique_hdc m_dc;
    wil::unique_hbitmap m_bitmap;
    HGDIOBJ m_old_bitmap{};
    void* m_bits{};
    int m_width{};
    int m_height{};
};

// 卡片的動畫時脈（2026-11-14 使用者說幀數不夠：主螢幕 240 Hz、副螢幕 144 Hz，原本 16 ms 計時器只有 60 格）：
// 有東西在動才開一條執行緒，每次 DwmFlush（等到下一次畫面合成，也就是螢幕的每一格）就送 message() 給卡片視窗；
// 卡片處理完一格呼叫 frame_done 才會送下一則，不會堆積。拖曳中的滑鼠移動只更新狀態，由時脈每格畫一次
class FrameClock {
public:
    FrameClock() = default;
    FrameClock(const FrameClock&) = delete;
    FrameClock& operator=(const FrameClock&) = delete;
    ~FrameClock() { stop(); }

    void ensure(HWND wnd);
    void stop();
    void frame_done() { m_pending = false; }
    bool active() const { return m_running; }
    static UINT message();

private:
    std::thread m_thread;
    std::atomic<bool> m_running{};
    std::atomic<bool> m_pending{};
    HWND m_wnd{};
};

} // namespace cui::nocturne_segment
