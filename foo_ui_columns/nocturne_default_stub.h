#pragma once

namespace cui::nocturne {

// 公開版專用（NOCTURNE_PUBLIC_BUILD）：還沒設過「沒封面佔位圖」時，自動指到 profile 根目錄附的 no-cover.png（唱片圖）。
// 個人版編譯時是空函式。要在任何面板查封面之前呼叫（主視窗建立前）。
void apply_default_stub_image();

} // namespace cui::nocturne
