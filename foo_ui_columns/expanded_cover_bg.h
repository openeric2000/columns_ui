#pragma once

// 自編版新增：展開頁「封面虛化背景」（2026-11-08，待做清單 v1.4 第 9 個）
//
// 做法（實測後定案，跟最初「用佈局疊 19 層壓暗」的想法不同）：
// - ESLyric 佈局的 albumart（含 follow-background）畫在文字「上面」，不能拿來當背景；佈局也沒有透明度，疊半透明層的話，
//   歌詞上下漸隱那兩塊會露出沒壓暗的原圖（亮帶）。所以改用 ESLyric「面板參數選項」自己的背景：
//   背景類型＝圖片、圖片來源＝自訂、路徑填標題格式 %cui_expanded_bg%、混合濃度拉滿、關掉模糊效果。
// - 這個欄位 %cui_expanded_bg% 由這裡提供：把正在播放那首的封面讀出來，縮成很小的圖、模糊、壓暗到「亮度上限」以下，
//   存成 PNG 放在 profile\nocturne-bg\，回傳那個檔案的完整路徑。面板把這張圖當背景畫（放大時雙線性內插，所以很柔）。
// - 沒勾「封面虛化背景」或沒有封面時欄位不存在（路徑是空的）→ 面板沒有背景圖 → 就是原本的純深色。
// - 亮度計算沿用預覽頁的定義：封面（模糊後）平均亮度 B 超過上限 T 就往深色 (10,10,10) 混，混合比例讓 B 剛好壓到 T，最多 95%；
//   另外多一條保險：最亮處超過 2T 也要壓（只對左右明暗懸殊的封面有影響）。
// - 圖是「中間一條 2:1 的橫帶」：面板用「填充」鋪滿比 2:1 更寬的展開頁，本來就只看得到封面中間那一段，所以量亮度也只量那一段。

namespace cui::expanded_cover_bg {

// 亮度上限（％）：10、15、20、25 四格，預設 15（使用者 2026-11-08 選的）
inline constexpr uint32_t cap_min = 10;
inline constexpr uint32_t cap_max = 25;
inline constexpr uint32_t cap_step = 5;
inline constexpr uint32_t cap_default = 15;

extern fbh::ConfigBool enabled;
extern fbh::ConfigUint32 cap_percent;

// 給標題格式欄位 %cui_expanded_bg% 用：回傳這首歌的背景圖完整路徑（UTF-8）；沒開、沒封面或做不出來時回空。
// 第一次遇到這首歌要讀封面、算圖，會在背景執行緒做並最多等一小段時間（逾時就先回空）；
// 做過的會記在記憶體裡，之後馬上回傳。可以在任何執行緒呼叫（實測 ESLyric 是在主執行緒呼叫，而且只在換歌時才讀路徑）。
// 換歌時會趁現在先替「預測的下一首」做好圖，所以平常這裡不用等
std::optional<std::string> background_path_for(const metadb_handle_ptr& track);

// 設定改了（勾選、亮度上限）：目前這首歌 ESLyric 要等下次換歌才會重讀，所以先替下一首做好圖。要在主執行緒呼叫
void on_settings_changed();

} // namespace cui::expanded_cover_bg
