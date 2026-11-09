#pragma once

// 自編版新增（2026-11-09）：播放清單框選方框的開關（使用者要求「像 Windows 原生那樣壓住左鍵拉出方形的反白」）
// 方框本身做在清單程式庫裡（ui_helpers\list_view\list_view_marquee.cpp），這裡只管「開或關」：
// - 開關存在 fb2k 共用設定（fb2k::configStore）的 nocturne.playlistMarquee，跟細捲軸開關放同一個地方
// - 沒有這個條目就是開；設定頁的「Reset page」會還原成開
// - 清單在按下滑鼠時才讀開關，所以改了下一次按下就生效，不用登記變更通知

namespace cui::playlist_marquee {

inline constexpr char config_key[] = "nocturne.playlistMarquee";
inline constexpr bool enabled_default = true;

inline bool is_enabled()
{
    return fb2k::configStore::get()->getConfigBool(config_key, enabled_default);
}

// 把目前的設定值套用到清單程式庫；建立播放清單視窗時呼叫
inline void sync_to_list_views()
{
    uih::ListView::s_set_marquee_selection_enabled(is_enabled());
}

// 設定頁勾選、Reset page 用：寫入設定並馬上套用
inline void set_enabled(bool enabled)
{
    fb2k::configStore::get()->setConfigBool(config_key, enabled);
    sync_to_list_views();
}

} // namespace cui::playlist_marquee
