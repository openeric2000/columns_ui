#include "pch.h"

#include "thin_scrollbar.h"

namespace cui::thin_scrollbar {

bool is_enabled()
{
    return fb2k::configStore::get()->getConfigBool(config_key, enabled_default);
}

void sync_to_list_views()
{
    // 第一次呼叫時登記變更通知：資料夾樹那邊（或任何人）改了開關，清單也要跟著切
    static bool notify_registered = false;

    if (!notify_registered) {
        notify_registered = true;
        fb2k::configStore::get()->addPermanentNotify(config_key, [] {
            // 通知可能來自別的執行緒，切換捲軸要回主執行緒做
            fb2k::inMainThread([] { uih::ListView::s_set_thin_scrollbar_enabled(is_enabled()); });
        });
    }

    uih::ListView::s_set_thin_scrollbar_enabled(is_enabled());
}

void set_enabled(bool enabled)
{
    fb2k::configStore::get()->setConfigBool(config_key, enabled);
    // 不等通知，直接套用（通知到了再套用一次也無妨：s_set_thin_scrollbar_enabled 值相同就不處理）
    sync_to_list_views();
}

} // namespace cui::thin_scrollbar
