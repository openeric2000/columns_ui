#include "pch.h"

#include <shlwapi.h>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <thread>

#pragma comment(lib, "shlwapi.lib")

// 聆聽紀錄（2026-11-23，工作區 docs\計畫書\聆聽紀錄計畫書.md 的 Step 2、Step 3）
//
// 自己記每一次播放，不靠 foo_playcount／foo_enhanced_playcount：
// - fb2k 主程式「算一次播放」的門檻固定是 60 秒（SDK play_callback.h 的 playback_statistics_collector），
//   兩個統計元件都只是收這個通知，改不了。使用者要「聽一半或滿 4 分鐘」，所以這裡記下每次實際聽了幾秒，
//   門檻交給聆聽紀錄面板（網頁）判斷，之後想改門檻，記過的紀錄也能照新規則重算。
// - 每播完一首（換歌、停止、關 fb2k）在 profile\nocturne-stats\plays-年.jsonl 加一行：
//   {"t":開始時間（UTC 毫秒）,"p":路徑,"s":子音軌,"len":歌長秒,"sec":實際聽了幾秒,"ti":歌名,"ar":藝人,"al":專輯,"img":封面縮圖}
//   只往後加、不改寫；聽不到 5 秒的不記（多半是在選歌）。fb2k 當掉最多少掉正在播的那一首。
// - 實際聽了幾秒：fb2k 播放中每秒呼叫一次 on_playback_time，數這個就好。暫停時不會呼叫；往後拖動不會讓它變多。
// - 封面縮圖：網頁讀不到每首歌的封面，所以開始播一首時在背景取封面、裁成正方形縮成 200×200 JPEG，
//   存到 profile\webview\default\thumbs\（網頁直接 <img> 引用）。檔名＝封面內容的雜湊，同一張封面只存一次。
// - 舊紀錄（Step 3）：第一次啟動時把 foo_enhanced_playcount 已經記下的每次播放時間（%played_times_js%），
//   連同路徑存成 profile\nocturne-stats\history.jsonl，第一行記切換時間。enhanced 照標籤對紀錄，之後改標籤就讀不到了，
//   所以趁現在照路徑存一份固定的。已經有這個檔就不再做。

namespace cui::listening_log {

namespace {

constexpr uint32_t min_listened_seconds = 5;
constexpr int thumb_size = 200;
constexpr ULONG thumb_jpeg_quality = 85;

std::wstring widen(const char* text)
{
    return std::wstring(pfc::stringcvt::string_wide_from_utf8(text).get_ptr());
}

std::string narrow(const wchar_t* text)
{
    return std::string(pfc::stringcvt::string_utf8_from_wide(text).get_ptr());
}

const std::wstring& profile_dir()
{
    static const auto path = [] {
        const pfc::string8 native = ::filesystem::g_get_native_path(core_api::get_profile_path());
        return widen(native.c_str());
    }();
    return path;
}

std::wstring stats_dir()
{
    return profile_dir() + L"\\nocturne-stats";
}

std::wstring thumbs_dir()
{
    return profile_dir() + L"\\webview\\default\\thumbs";
}

int64_t now_unix_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count();
}

uint64_t fnv1a(const void* data, size_t size, uint64_t hash = 14695981039346656037ull)
{
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

void append_json_string(std::string& out, std::string_view text)
{
    out += '"';
    for (const char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20)
                out += fmt::format("\\u{:04x}", c);
            else
                out += ch;
        }
    }
    out += '"';
}

// 路徑一律存成 Windows 路徑（E:\音樂\…）；不是本機檔案（網路串流等）就原樣
std::string native_path(const metadb_handle_ptr& track)
{
    pfc::string8 out;
    if (!::filesystem::g_get_native_path(track->get_path(), out))
        out = track->get_path();
    return std::string(out.c_str());
}

std::string format(const metadb_handle_ptr& track, const char* script)
{
    titleformat_object::ptr object;
    titleformat_compiler::get()->compile_safe(object, script);
    pfc::string8 text;
    track->format_title(nullptr, text, object, nullptr);
    return std::string(text.c_str());
}

// 把一行加到檔案尾端（UTF-8，換行 \n）。所有寫入共用一把鎖，背景執行緒與關 fb2k 時的同步寫入不會交錯
std::mutex file_mutex;

void append_line(const std::wstring& path, const std::string& line)
{
    std::scoped_lock lock(file_mutex);
    std::error_code ignored;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ignored);
    const auto file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    const auto text = line + "\n";
    DWORD written{};
    WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(file);
}

// ---------- 封面縮圖 ----------

bool ensure_gdiplus()
{
    // 自己握一份 GDI+ 的啟動計數，用到進程結束
    static const bool ok = [] {
        ULONG_PTR token{};
        const Gdiplus::GdiplusStartupInput input;
        return Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok;
    }();
    return ok;
}

bool jpeg_encoder(CLSID& clsid)
{
    UINT count{};
    UINT size{};
    if (Gdiplus::GetImageEncodersSize(&count, &size) != Gdiplus::Ok || size == 0)
        return false;
    std::vector<BYTE> buffer(size);
    auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
    if (Gdiplus::GetImageEncoders(count, size, encoders) != Gdiplus::Ok)
        return false;
    for (UINT i = 0; i < count; ++i) {
        if (wcscmp(encoders[i].MimeType, L"image/jpeg") == 0) {
            clsid = encoders[i].Clsid;
            return true;
        }
    }
    return false;
}

// 取封面、存縮圖，回傳檔名（不含副檔名）；沒封面或失敗回空字串。已經存過同一張就直接回傳檔名
std::string make_thumb(const metadb_handle_ptr& track)
{
    try {
        album_art_data_ptr data;
        abort_callback_dummy abort;
        {
            pfc::list_single_ref_t<metadb_handle_ptr> items(track);
            pfc::list_single_ref_t<GUID> ids(album_art_ids::cover_front);
            const auto instance = album_art_manager_v2::get()->open(items, ids, abort);
            if (!instance->query(album_art_ids::cover_front, data, abort) || !data.is_valid() || data->get_size() == 0)
                return {};
        }

        const auto hash = fnv1a(data->get_ptr(), data->get_size());
        const auto key = fmt::format("{:016x}", hash);
        const auto folder = thumbs_dir();
        const auto final_path = folder + L"\\" + widen(key.c_str()) + L".jpg";
        std::error_code ignored;
        if (std::filesystem::exists(final_path, ignored))
            return key;

        if (!ensure_gdiplus())
            return {};

        wil::com_ptr<IStream> stream;
        stream.attach(SHCreateMemStream(static_cast<const BYTE*>(data->get_ptr()), gsl::narrow<UINT>(data->get_size())));
        if (!stream)
            return {};

        Gdiplus::Bitmap source(stream.get());
        const auto width = static_cast<int>(source.GetWidth());
        const auto height = static_cast<int>(source.GetHeight());
        if (source.GetLastStatus() != Gdiplus::Ok || width <= 0 || height <= 0)
            return {};

        // 裁中間的正方形（同網頁的 background-size:cover）
        const int side = std::min(width, height);
        const int crop_x = (width - side) / 2;
        const int crop_y = (height - side) / 2;

        Gdiplus::Bitmap result(thumb_size, thumb_size, PixelFormat24bppRGB);
        {
            Gdiplus::Graphics graphics(&result);
            graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
            graphics.Clear(Gdiplus::Color(255, 24, 24, 24));
            Gdiplus::ImageAttributes attributes;
            attributes.SetWrapMode(Gdiplus::WrapModeTileFlipXY);      // 邊緣不要混進透明的黑邊
            graphics.DrawImage(&source, Gdiplus::Rect(0, 0, thumb_size, thumb_size), crop_x, crop_y, side, side,
                Gdiplus::UnitPixel, &attributes);
        }

        CLSID jpeg{};
        if (!jpeg_encoder(jpeg))
            return {};
        Gdiplus::EncoderParameters parameters{};
        ULONG quality = thumb_jpeg_quality;
        parameters.Count = 1;
        parameters.Parameter[0].Guid = Gdiplus::EncoderQuality;
        parameters.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
        parameters.Parameter[0].NumberOfValues = 1;
        parameters.Parameter[0].Value = &quality;

        std::filesystem::create_directories(folder, ignored);
        // 先存暫存檔再改名，網頁不會讀到寫到一半的圖；暫存檔名帶執行緒編號，兩首同封面的歌同時做也不會互搶
        wchar_t temp_suffix[32];
        std::swprintf(temp_suffix, std::size(temp_suffix), L".%lu.tmp", GetCurrentThreadId());
        const auto temp_path = final_path + temp_suffix;
        if (result.Save(temp_path.c_str(), &jpeg, &parameters) != Gdiplus::Ok) {
            DeleteFileW(temp_path.c_str());
            return std::filesystem::exists(final_path, ignored) ? key : std::string{};
        }
        if (!MoveFileExW(temp_path.c_str(), final_path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW(temp_path.c_str());
            return std::filesystem::exists(final_path, ignored) ? key : std::string{};
        }
        return key;
    } catch (const std::exception&) {
        return {};
    }
}

// ---------- 這一次播放 ----------

struct Session {
    uint64_t id{};
    metadb_handle_ptr track;
    int64_t start_ms{};
    double length{};
    uint32_t listened{};
    std::string thumb;
};

std::mutex session_mutex;
std::optional<Session> current;
uint64_t next_session_id{1};

std::wstring plays_file(int64_t start_ms)
{
    const auto seconds = static_cast<time_t>(start_ms / 1000);
    tm local{};
    localtime_s(&local, &seconds);
    return stats_dir() + L"\\plays-" + std::to_wstring(local.tm_year + 1900) + L".jsonl";
}

// 收尾：把上一首寫成一行。sync＝關 fb2k 時直接寫完；平常交給背景執行緒
void finish(bool sync)
{
    std::optional<Session> done;
    {
        std::scoped_lock lock(session_mutex);
        done.swap(current);
    }
    if (!done || done->listened < min_listened_seconds || done->length <= 0 || !done->track.is_valid())
        return;

    // 歌名等等在主執行緒取（這個函式只從播放回呼與 on_quit 呼叫，都在主執行緒）
    std::string line = "{\"t\":";
    line += std::to_string(done->start_ms);
    line += ",\"p\":";
    append_json_string(line, native_path(done->track));
    line += ",\"s\":";
    line += std::to_string(done->track->get_subsong_index());
    line += fmt::format(",\"len\":{:.1f},\"sec\":{}", done->length, done->listened);
    line += ",\"ti\":";
    append_json_string(line, format(done->track, "[%title%]"));
    line += ",\"ar\":";
    append_json_string(line, format(done->track, "[%artist%]"));
    line += ",\"al\":";
    append_json_string(line, format(done->track, "[%album%]"));
    line += ",\"img\":";
    append_json_string(line, done->thumb);
    line += "}";

    const auto path = plays_file(done->start_ms);
    if (sync)
        append_line(path, line);
    else
        std::thread([path, line] { append_line(path, line); }).detach();
}

void start(const metadb_handle_ptr& track)
{
    finish(false);
    if (!track.is_valid())
        return;

    uint64_t id{};
    {
        std::scoped_lock lock(session_mutex);
        id = next_session_id++;
        current = Session{id, track, now_unix_ms(), track->get_length(), 0, {}};
    }

    // 封面縮圖在背景做；做好時這首還在播，就記到這一次播放上
    std::thread([track, id] {
        if (core_api::is_shutting_down())
            return;
        auto key = make_thumb(track);
        std::scoped_lock lock(session_mutex);
        if (current && current->id == id)
            current->thumb = std::move(key);
    }).detach();
}

class ListeningLogPlayCallback : public play_callback_static {
public:
    unsigned get_flags() override { return flag_on_playback_new_track | flag_on_playback_stop | flag_on_playback_time; }

    void on_playback_new_track(metadb_handle_ptr track) override { start(track); }

    // 播放中每秒一次（暫停時不會來）：這一次播放實際聽了幾秒
    void on_playback_time(double time) override
    {
        std::scoped_lock lock(session_mutex);
        if (current)
            ++current->listened;
    }

    void on_playback_stop(play_control::t_stop_reason reason) override
    {
        finish(reason == play_control::stop_reason_shutting_down);
    }

    void on_playback_starting(play_control::t_track_command command, bool paused) override {}
    void on_playback_seek(double time) override {}
    void on_playback_pause(bool state) override {}
    void on_playback_edited(metadb_handle_ptr track) override {}
    void on_playback_dynamic_info(const file_info& info) override {}
    void on_playback_dynamic_info_track(const file_info& info) override {}
    void on_volume_change(float new_value) override {}
};

play_callback_static_factory_t<ListeningLogPlayCallback> listening_log_play_callback;

class ListeningLogInitQuit : public initquit {
public:
    void on_quit() override { finish(true); }
};

initquit_factory_t<ListeningLogInitQuit> listening_log_initquit;

// ---------- 舊紀錄（Step 3） ----------

void save_history()
{
    const auto final_path = stats_dir() + L"\\history.jsonl";
    std::error_code ignored;
    if (std::filesystem::exists(final_path, ignored))
        return;

    // 媒體庫清單只能在主執行緒拿；逐首讀欄位交給背景執行緒（主環境 8.8 萬首只讀這一欄約 0.3 秒）
    auto items = std::make_shared<metadb_handle_list>();
    library_manager::get()->get_all_items(*items);
    const auto cutover = now_unix_ms();

    std::thread([items, cutover, final_path] {
        titleformat_object::ptr times_script;
        titleformat_object::ptr meta_script;
        titleformat_compiler::get()->compile_safe(times_script, "[%played_times_js%]");
        // 欄位之間用 $char(1) 分開：歌名、藝人、專輯裡不會有這個字元
        titleformat_compiler::get()->compile_safe(
            meta_script, "[%title%]$char(1)[%artist%]$char(1)[%album%]$char(1)%length_seconds_fp%");

        std::string text = fmt::format("{{\"cutover\":{},\"source\":\"foo_enhanced_playcount\"}}\n", cutover);
        size_t tracks{};
        pfc::string8 buffer;
        for (size_t index = 0; index < items->get_count(); ++index) {
            if (index % 1000 == 0 && core_api::is_shutting_down())
                return;
            const auto& track = items->get_item_ref(index);
            track->format_title(nullptr, buffer, times_script, nullptr);
            const std::string_view times(buffer.c_str());
            // 沒有紀錄是「[]」；沒裝 foo_enhanced_playcount 時欄位不存在（空字串）
            if (times.size() < 3 || times.front() != '[' || times.back() != ']')
                continue;
            const std::string times_copy(times);

            pfc::string8 meta;
            track->format_title(nullptr, meta, meta_script, nullptr);
            std::vector<std::string> fields;
            std::string field;
            for (const char ch : std::string_view(meta.c_str())) {
                if (ch == '\x01') {
                    fields.push_back(std::move(field));
                    field.clear();
                } else {
                    field += ch;
                }
            }
            fields.push_back(std::move(field));
            fields.resize(4);

            text += "{\"p\":";
            append_json_string(text, native_path(track));
            text += ",\"s\":";
            text += std::to_string(track->get_subsong_index());
            text += ",\"len\":";
            text += fields[3].empty() ? "0" : fields[3];
            text += ",\"ti\":";
            append_json_string(text, fields[0]);
            text += ",\"ar\":";
            append_json_string(text, fields[1]);
            text += ",\"al\":";
            append_json_string(text, fields[2]);
            text += ",\"times\":";
            text += times_copy;
            text += "}\n";
            ++tracks;
        }

        std::error_code error;
        std::filesystem::create_directories(std::filesystem::path(final_path).parent_path(), error);
        const auto temp_path = final_path + L".tmp";
        const auto file = CreateFileW(temp_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return;
        DWORD written{};
        const auto ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr)
            && written == text.size();
        CloseHandle(file);
        if (!ok || !MoveFileExW(temp_path.c_str(), final_path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW(temp_path.c_str());
            return;
        }
        fb2k::inMainThread([tracks] {
            console::print(fmt::format("Nocturne: listening history saved ({} tracks)", tracks).c_str());
        });
    }).detach();
}

FB2K_ON_INIT_STAGE(save_history, init_stages::after_library_init);

} // namespace

} // namespace cui::listening_log
