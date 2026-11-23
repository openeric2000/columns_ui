#pragma once

// 自編版新增（2026-11-17，v1.5 待做清單第 9 個）：播 DSD 時給 VU／頻譜用的 PCM，自己讀檔轉出來
// foo_input_sacd 輸出純 DSD（直送 ASIO）時，fb2k 給視覺化的資料是 44100 Hz、樣本全是 0，VU 與頻譜不會動。
// 這裡在背景執行緒開正在播的 .dff／.dsf，照播放位置讀前後一段、把 1 位元 DSD 降頻成 44.1 kHz 浮點，
// VU 與頻譜要資料時從記憶體拿。只讀本機固定磁碟（網路磁碟不讀，使用者 2026-11-17 決定）。
// 計畫書：docs\計畫書\DSD播放時VU與頻譜自己讀檔計畫書.md

namespace cui::dsd_source {

constexpr unsigned output_rate = 44100;

// 這首是不是要自己讀的檔：副檔名 .dff／.dsf、在本機固定磁碟上（只看路徑，不開檔）
bool is_candidate(const metadb_handle_ptr& track);

// 拿到 end_seconds（曲目內的秒數）之前 samples 個樣本（44.1 kHz、雙聲道）。
// 背景還沒讀到這段（剛開始、剛跳轉）或檔案不支援時傳回 false。主執行緒呼叫；每次呼叫也告訴背景現在播到哪
bool read(const metadb_handle_ptr& track, double end_seconds, size_t samples, audio_chunk_impl& out);

// 現在播到哪（曲目內的秒數），已加上聲音與畫面的時間差
double playback_seconds();

// 開發版：profile 裡有 nocturne_dsd_force 這個檔時，不管 fb2k 有沒有給資料都自己讀（在 219 對照用）
bool forced();

} // namespace cui::dsd_source
