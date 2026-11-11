#pragma once

// 自編版新增：展開頁「VU 表」模式的自繪 VU 表（2026-11-11，v1.5 待做清單第 4 個）
// 原本用外掛元件 foo_vis_vumeter，但它用 DirectX 畫一張實心圖，Columns UI 底下不能透明，換哪款皮膚都是一塊方框，
// 跟展開頁的封面虛化背景很突兀。改成 Columns UI 自己畫（使用者看過預覽定案 A3）：
// - 左右兩個錶框，各是一片「玻璃」：透出後面的虛化背景（截圖取得），只在邊緣折射放大（liquid-glass-js 的厚度曲線），
//   依邊緣朝向算反光（左上亮、右下弱），外面一圈陰影；錶面深淺依背景亮度自動調，字一直看得清楚
// - 錶面：真 VU 刻度（位置與電壓成正比，−20～+3，0 以上紅區）、VU 標誌、L／R、校準字、峰值燈、霧面軸蓋、可關的暖色背光；
//   字型跟著主題字型（Common (labels)）
// - 指針：平均音量（RMS），0 VU＝−7 dBFS（同使用者先前選的 C 組：外掛 VU 增益 +7）；二階系統 300 毫秒到位、過衝 1.5%，左右分開
// - 峰值燈：瞬間峰值到 −1 dBFS 亮紅，亮 150 毫秒後 0.4 秒內熄
// - 只在展開頁完全展開、VU 表模式、版面標記有歌詞欄位置時顯示（位置同 cover_zoom::vu_rect_in_stage）；
//   錶框以外不擋滑鼠（點擊照常落到底下的 ESLyric）；錶框上按右鍵開自己的樣式挑選卡（2026-11-12），滾輪照常轉給 ESLyric
// - 視窗由 FlatSplitterPanel 在有播放列時建立，跟放大鈕同一批排版
// - 2026-11-12 加了圓形錶頭、單錶雙指針兩種樣式（Style），共用同一套玻璃、指針動態與背景截取

namespace cui::vu_meter {

// 暖色背光：存在 fb2k 共用設定；預設開
inline constexpr char backlight_config_key[] = "nocturne.vuMeter.backlight";
inline constexpr bool backlight_default = true;

bool is_backlight_enabled();
void set_backlight_enabled(bool enabled);

// 樣式（2026-11-12 使用者看五種預覽後選：方框雙錶之外加圓形錶頭與單錶雙指針，在錶上按右鍵、從挑選卡挑；使用者說不要放設定頁）
// 存 fb2k 共用設定的整數；數字順序就是挑選卡縮圖的順序，之後新增只能加在後面
enum class Style : int {
    box = 0,   // 方框雙錶（原本這版）
    round = 1, // 圓形錶頭：兩顆圓形玻璃錶，亮面軸心蓋
    dual = 2,  // 單錶雙指針：一個寬錶，左聲道白、右聲道琥珀
};
inline constexpr int style_count = 3;
inline constexpr char style_config_key[] = "nocturne.vuMeter.style";
inline constexpr Style style_default = Style::box;

Style get_style();
void set_style(Style style);

// 指針設定（2026-11-12 使用者要「幾種適合的指針設定」放進右鍵挑選卡）：校準與反應速度的組合，數值見 needle_tunings
enum class NeedlePreset : int {
    standard = 0,  // 標準：0 VU = −7 dBFS、300 毫秒
    lively = 1,    // 活潑：−10 dBFS、300 毫秒
    snappy = 2,    // 靈敏：−10 dBFS、150 毫秒
    automatic = 3, // 自動：跟著歌曲平均音量校準、300 毫秒
};
inline constexpr int needle_preset_count = 4;
inline constexpr char needle_preset_config_key[] = "nocturne.vuMeter.needle";
inline constexpr NeedlePreset needle_preset_default = NeedlePreset::standard;

NeedlePreset get_needle_preset();
void set_needle_preset(NeedlePreset preset);

HWND create(HWND parent);
void layout(HWND wnd, const RECT& bar_rect);
HDWP defer_layout(HDWP dwp, HWND wnd, const RECT& bar_rect);
// 用上一次的播放列位置重排所有 VU 表（展開動畫結束、切換展開頁模式、設定改了時用）
void refresh_layouts();

} // namespace cui::vu_meter
