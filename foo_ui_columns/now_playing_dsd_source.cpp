#include "pch.h"

#if __has_include("nocturne_build_info.h")
#include "nocturne_build_info.h"
#endif

#include "now_playing_dsd_source.h"

// 播 DSD 時給 VU／頻譜用的 PCM，說明見 now_playing_dsd_source.h

namespace cui::dsd_source {

namespace {

constexpr double pi = 3.14159265358979323846;

// 記憶體裡保留「播放位置往前 keep_before_s 秒到往後 read_ahead_s 秒」
constexpr double keep_before_s = 0.5;
constexpr double read_ahead_s = 1.0;
// 跳轉後從目標往前這麼多秒開始讀（濾波器要先吃一點資料，VU 也要往前看一小段）
constexpr double seek_back_s = 0.3;
// 每次讀多少（秒）
constexpr double read_step_s = 0.125;
// 沒人要資料超過這麼久就關檔、停下來（展開頁收合、換到不用 VU 的模式）
constexpr ULONGLONG idle_ms = 1000;
// 聲音與畫面的時間差（秒，加在播放位置上）：219（PCM、Null Output）六首 DSD 對照 fb2k 給視覺化的資料，
// 不補正時只差約 5 ms（2026-11-17），所以是 0。主環境純 DSD 走 ASIO，要再實測
constexpr double playback_offset_s = 0.0;
// 轉出來的音量倍數：219 對照 foo_input_sacd 轉的 PCM，DSD 全部 1＝1.0 時剛好低 6.0 dB
// （它把 50% 調變當 0 dBFS，SACD 的慣例），所以乘 2
constexpr float output_gain = 2.0f;

uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint32_t be32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | p[3];
}
uint64_t be64(const uint8_t* p) { return (static_cast<uint64_t>(be32(p)) << 32) | be32(p + 4); }
uint32_t le32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[3]) << 24) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[0];
}
uint64_t le64(const uint8_t* p) { return (static_cast<uint64_t>(le32(p + 4)) << 32) | le32(p); }

bool read_at(HANDLE file, uint64_t offset, void* buffer, DWORD size)
{
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN))
        return false;
    DWORD got{};
    return ReadFile(file, buffer, size, &got, nullptr) && got == size;
}

// ---- 檔頭 ----

struct Format {
    uint64_t data_start{};
    // 每聲道的資料位元組數
    uint64_t bytes_per_channel{};
    // DSD 取樣率（每聲道每秒位元數，DSD64＝2822400）
    unsigned sample_rate{};
    unsigned channels{};
    // DSF 每樣本位元數＝1：一個位元組裡時間較早的在低位元；DFF 一律高位元在前
    bool lsb_first{};
    // DSF：每聲道 block_size 位元組一塊輪流排；DFF：聲道逐位元組交錯
    bool block_interleaved{};
    unsigned block_size{};
};

// DSF：DSD 區塊（28 bytes）→ fmt 區塊 → data 區塊
std::optional<Format> parse_dsf(HANDLE file)
{
    uint8_t header[28 + 52]{};
    if (!read_at(file, 0, header, sizeof(header)) || memcmp(header, "DSD ", 4) != 0 || memcmp(header + 28, "fmt ", 4) != 0)
        return {};

    // fmt：ID(4) 大小(8) 版本(4) 格式(4) 聲道配置(4) 聲道數(4) 取樣率(4) 每樣本位元數(4) 樣本數(8) 區塊大小(4) 保留(4)
    const auto* fmt = header + 28;
    const auto fmt_size = le64(fmt + 4);
    const auto format_id = le32(fmt + 16);
    const auto channels = le32(fmt + 24);
    const auto rate = le32(fmt + 28);
    const auto bits = le32(fmt + 32);
    const auto sample_count = le64(fmt + 36);
    const auto block = le32(fmt + 44);
    if (format_id != 0 || block == 0 || channels == 0 || (bits != 1 && bits != 8))
        return {};

    const auto data_offset = 28 + fmt_size;
    uint8_t data[12]{};
    if (!read_at(file, data_offset, data, sizeof(data)) || memcmp(data, "data", 4) != 0)
        return {};

    Format format;
    format.data_start = data_offset + 12;
    format.sample_rate = rate;
    format.channels = channels;
    format.lsb_first = bits == 1;
    format.block_interleaved = true;
    format.block_size = block;
    const auto data_size = le64(data + 4) > 12 ? le64(data + 4) - 12 : 0;
    format.bytes_per_channel = std::min<uint64_t>(sample_count / 8, data_size / channels);
    return format;
}

// DFF（DSDIFF）：FRM8 底下 PROP（SND：FS、CHNL、CMPR）與 DSD 區塊；大小都是大端序、區塊補到偶數
std::optional<Format> parse_dff(HANDLE file, uint64_t file_size)
{
    uint8_t header[16]{};
    if (!read_at(file, 0, header, sizeof(header)) || memcmp(header, "FRM8", 4) != 0 || memcmp(header + 12, "DSD ", 4) != 0)
        return {};

    const auto end = std::min<uint64_t>(file_size, 12 + be64(header + 4));
    Format format;
    bool have_prop{};
    bool uncompressed{};
    uint64_t position = 16;
    while (position + 12 <= end) {
        uint8_t chunk[12]{};
        if (!read_at(file, position, chunk, sizeof(chunk)))
            return {};
        const auto size = be64(chunk + 4);
        const auto body = position + 12;

        if (memcmp(chunk, "PROP", 4) == 0) {
            std::vector<uint8_t> prop(static_cast<size_t>(std::min<uint64_t>(size, 64 * 1024)));
            if (prop.size() < 4 || !read_at(file, body, prop.data(), static_cast<DWORD>(prop.size()))
                || memcmp(prop.data(), "SND ", 4) != 0)
                return {};
            size_t p = 4;
            while (p + 12 <= prop.size()) {
                const auto* sub = prop.data() + p;
                const auto sub_size = be64(sub + 4);
                if (sub_size > prop.size() - p - 12)
                    break;
                const auto* sub_body = sub + 12;
                if (memcmp(sub, "FS  ", 4) == 0 && sub_size >= 4)
                    format.sample_rate = be32(sub_body);
                else if (memcmp(sub, "CHNL", 4) == 0 && sub_size >= 2)
                    format.channels = be16(sub_body);
                else if (memcmp(sub, "CMPR", 4) == 0 && sub_size >= 4)
                    uncompressed = memcmp(sub_body, "DSD ", 4) == 0;
                p += 12 + static_cast<size_t>(sub_size) + static_cast<size_t>(sub_size & 1);
            }
            have_prop = true;
        } else if (memcmp(chunk, "DSD ", 4) == 0) {
            if (!have_prop || !uncompressed || format.channels == 0)
                return {};
            format.data_start = body;
            format.bytes_per_channel = std::min(size, end - body) / format.channels;
            return format;
        } else if (memcmp(chunk, "DST ", 4) == 0) {
            // DST 壓縮不支援（曲庫目前沒有）
            return {};
        }
        position = body + size + (size & 1);
    }
    return {};
}

bool has_extension(const std::wstring& path, const wchar_t* extension)
{
    const auto length = wcslen(extension);
    return path.size() > length && _wcsicmp(path.c_str() + path.size() - length, extension) == 0;
}

std::optional<std::wstring> local_path_of(const metadb_handle_ptr& track)
{
    pfc::string8 native;
    if (!track.is_valid() || !filesystem::g_get_native_path(track->get_path(), native) || native.is_empty())
        return {};

    auto path = mmh::to_utf16(native.c_str());
    if (!has_extension(path, L".dff") && !has_extension(path, L".dsf"))
        return {};
    if (path.size() < 3 || path[1] != L':')
        return {};
    const wchar_t root[] = {path[0], L':', L'\\', 0};
    if (GetDriveType(root) != DRIVE_FIXED)
        return {};
    return path;
}

// ---- DSD → 44.1 kHz ----
// 三段：位元組查表 FIR（64 個位元，除 8）→ FIR 降到 88.2 kHz → FIR 降到 44.1 kHz。
// DSD 的雜訊集中在 20 kHz 以上而且很大聲，最後一段要確實擋到 22 kHz 以上，不然安靜段落的指針與頻譜高頻會浮起來

// Blackman 窗的低通，直流增益 1；cutoff 是截止頻率 ÷ 取樣率
std::vector<float> lowpass(size_t taps, double cutoff)
{
    std::vector<double> h(taps);
    const double m = static_cast<double>(taps - 1);
    double sum{};
    for (size_t i = 0; i < taps; ++i) {
        const double x = static_cast<double>(i) - m / 2;
        const double sinc = x == 0 ? 2 * cutoff : std::sin(2 * pi * cutoff * x) / (pi * x);
        const double window = 0.42 - 0.5 * std::cos(2 * pi * i / m) + 0.08 * std::cos(4 * pi * i / m);
        h[i] = sinc * window;
        sum += h[i];
    }
    std::vector<float> result(taps);
    for (size_t i = 0; i < taps; ++i)
        result[i] = static_cast<float>(h[i] / sum);
    return result;
}

// 每 factor 個輸入算一個輸出的 FIR（係數對稱，不用管方向）
class Decimator {
public:
    void init(std::vector<float> taps, unsigned factor)
    {
        m_taps = std::move(taps);
        m_factor = factor;
        reset();
    }

    void reset()
    {
        m_history.assign(m_taps.size() * 2, 0.f);
        m_position = 0;
        m_phase = 0;
    }

    bool push(float value, float& out)
    {
        const auto n = m_taps.size();
        // 存兩份，m_history[m_position, m_position + n) 永遠是由舊到新、連續的
        m_history[m_position] = value;
        m_history[m_position + n] = value;
        m_position = (m_position + 1) % n;
        if (++m_phase < m_factor)
            return false;
        m_phase = 0;
        const float* history = m_history.data() + m_position;
        float sum{};
        for (size_t i = 0; i < n; ++i)
            sum += m_taps[i] * history[i];
        out = sum;
        return true;
    }

private:
    std::vector<float> m_taps;
    std::vector<float> m_history;
    size_t m_position{};
    unsigned m_factor{1};
    unsigned m_phase{};
};

constexpr size_t stage1_bytes = 8;

struct Converter {
    // 第一段：最近 8 個位元組各自查表相加。table[k][b]＝第 k 舊的位元組是 b 時的貢獻
    std::array<std::array<float, 256>, stage1_bytes> table{};
    std::array<uint8_t, 256> reverse{};
    bool lsb_first{};
    // 每聲道最近 8 個位元組（環狀）與後兩段
    struct Channel {
        std::array<uint8_t, stage1_bytes> bytes{};
        size_t next{};
        Decimator stage2;
        Decimator stage3;
    };
    std::array<Channel, 2> channels;

    void init(const Format& format)
    {
        lsb_first = format.lsb_first;
        // 第一段：截止約取樣率的 1/35（DSD64 約 80 kHz），擋掉會折回去的那一帶
        const auto h1 = lowpass(stage1_bytes * 8, 1.0 / 35.0);
        for (size_t k = 0; k < stage1_bytes; ++k) {
            for (int b = 0; b < 256; ++b) {
                float sum{};
                for (int t = 0; t < 8; ++t) {
                    // 位元組裡第 t 個（時間順序，高位元在前）；越新的位元年紀越小
                    const auto bit = (b >> (7 - t)) & 1;
                    sum += h1[k * 8 + 7 - t] * (bit ? 1.f : -1.f);
                }
                table[k][b] = sum;
            }
        }
        for (int b = 0; b < 256; ++b) {
            int r{};
            for (int i = 0; i < 8; ++i)
                r |= ((b >> i) & 1) << (7 - i);
            reverse[b] = static_cast<uint8_t>(r);
        }

        const auto rate1 = format.sample_rate / 8.0;
        const auto factor2 = static_cast<unsigned>(std::lround(rate1 / 88200.0));
        for (auto& channel : channels) {
            channel.stage2.init(lowpass(12 * factor2, 40000.0 / rate1), factor2);
            channel.stage3.init(lowpass(127, 21500.0 / 88200.0), 2);
        }
        reset();
    }

    void reset()
    {
        for (auto& channel : channels) {
            // 0x69 是 DSD 的靜音樣式（平均為 0）
            channel.bytes.fill(0x69);
            channel.next = 0;
            channel.stage2.reset();
            channel.stage3.reset();
        }
    }

    // 吃一個位元組；有 44.1 kHz 的輸出時寫到 out
    bool push(int index, uint8_t byte, float& out)
    {
        auto& channel = channels[index];
        channel.bytes[channel.next] = lsb_first ? reverse[byte] : byte;
        float value{};
        for (size_t k = 0; k < stage1_bytes; ++k)
            value += table[k][channel.bytes[(channel.next + stage1_bytes - k) % stage1_bytes]];
        channel.next = (channel.next + 1) % stage1_bytes;
        float mid{};
        if (!channel.stage2.push(value, mid))
            return false;
        return channel.stage3.push(mid, out);
    }
};

// ---- 背景執行緒 ----

struct Shared {
    std::mutex mutex;
    std::condition_variable wake;
    bool quit{};
    // 主執行緒要的：檔案、播到哪、最後一次要的時間
    std::wstring want_path;
    double want_seconds{};
    ULONGLONG want_tick{};
    // 已轉好的：哪個檔、第一個樣本的編號（44.1 kHz）、雙聲道交錯的樣本
    std::wstring ready_path;
    int64_t ready_start{};
    std::vector<float> ready;
};

Shared g_shared;
std::thread g_thread;

class Worker {
public:
    void run()
    {
        std::unique_lock lock(g_shared.mutex);
        while (!g_shared.quit) {
            if (!m_busy)
                g_shared.wake.wait_for(lock, std::chrono::milliseconds(20));
            if (g_shared.quit)
                break;

            const bool idle = g_shared.want_path.empty() || GetTickCount64() - g_shared.want_tick > idle_ms;
            if (idle) {
                m_busy = false;
                if (m_file) {
                    m_file.reset();
                    m_path.clear();
                    g_shared.ready_path.clear();
                    g_shared.ready.clear();
                }
                continue;
            }

            const auto path = g_shared.want_path;
            const auto seconds = g_shared.want_seconds;
            lock.unlock();
            step(path, seconds);
            lock.lock();
        }
    }

private:
    void open(const std::wstring& path)
    {
        m_file.reset();
        m_format.reset();
        m_path = path;
        wil::unique_hfile file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        LARGE_INTEGER size{};
        if (!file || !GetFileSizeEx(file.get(), &size))
            return;

        auto format = has_extension(path, L".dsf") ? parse_dsf(file.get())
                                                   : parse_dff(file.get(), static_cast<uint64_t>(size.QuadPart));
        // 只支援雙聲道、DSD64／128／256
        if (!format || format->channels != 2)
            return;
        const auto ratio = format->sample_rate / (output_rate * 64u);
        if (format->sample_rate % (output_rate * 64u) != 0 || (ratio != 1 && ratio != 2 && ratio != 4))
            return;

        m_format = format;
        m_bytes_per_output = format->sample_rate / output_rate / 8;
        m_converter.init(*format);
        m_file = std::move(file);
        m_decoded = false;
    }

    // 從 sample（44.1 kHz 的樣本編號）開始重新轉
    void restart(int64_t sample)
    {
        m_converter.reset();
        m_next_byte = static_cast<uint64_t>(sample) * m_bytes_per_output;
        m_next_sample = sample;
        m_decoded = true;
        std::scoped_lock lock(g_shared.mutex);
        g_shared.ready_path = m_path;
        g_shared.ready_start = sample;
        g_shared.ready.clear();
    }

    void step(const std::wstring& path, double seconds)
    {
        if (path != m_path)
            open(path);
        if (!m_file || !m_format) {
            m_busy = false;
            return;
        }

        const auto want = static_cast<int64_t>(seconds * output_rate);
        int64_t ready_start{};
        {
            std::scoped_lock lock(g_shared.mutex);
            ready_start = g_shared.ready_start;
        }
        // 第一次、往回跳、往前跳太遠：從目標往前一點重新開始
        if (!m_decoded || want < ready_start || want > m_next_sample + static_cast<int64_t>(0.5 * output_rate))
            restart(std::max<int64_t>(0, want - static_cast<int64_t>(seek_back_s * output_rate)));

        const auto total_samples = static_cast<int64_t>(m_format->bytes_per_channel / m_bytes_per_output);
        const auto target = std::min(total_samples, want + static_cast<int64_t>(read_ahead_s * output_rate));
        if (m_next_sample >= target) {
            m_busy = false;
            trim(want);
            return;
        }

        const auto count = std::min<int64_t>(target - m_next_sample, static_cast<int64_t>(read_step_s * output_rate));
        std::vector<float> pcm;
        if (!decode(static_cast<size_t>(count), pcm)) {
            m_busy = false;
            return;
        }
        {
            std::scoped_lock lock(g_shared.mutex);
            if (g_shared.ready_path == m_path)
                g_shared.ready.insert(g_shared.ready.end(), pcm.begin(), pcm.end());
        }
        m_next_sample += static_cast<int64_t>(pcm.size() / 2);
        trim(want);
        m_busy = m_next_sample < target;
    }

    // 丟掉播放位置往前 keep_before_s 秒以前的
    void trim(int64_t want)
    {
        std::scoped_lock lock(g_shared.mutex);
        const auto keep_from = want - static_cast<int64_t>(keep_before_s * output_rate);
        const auto drop = std::min<int64_t>(keep_from - g_shared.ready_start, static_cast<int64_t>(g_shared.ready.size() / 2));
        if (drop > 0) {
            g_shared.ready.erase(g_shared.ready.begin(), g_shared.ready.begin() + drop * 2);
            g_shared.ready_start += drop;
        }
    }

    // 讀 count 個輸出樣本份量的 DSD，轉成雙聲道交錯的 44.1 kHz
    bool decode(size_t count, std::vector<float>& pcm)
    {
        const auto& format = *m_format;
        const auto bytes = count * m_bytes_per_output;
        std::vector<uint8_t> left(bytes);
        std::vector<uint8_t> right(bytes);

        if (format.block_interleaved) {
            // DSF：涵蓋這段的那幾組（每組＝每聲道各一塊）整段讀進來再拆
            const auto block = static_cast<uint64_t>(format.block_size);
            const auto first_group = m_next_byte / block;
            const auto last_group = (m_next_byte + bytes - 1) / block;
            const auto group_bytes = block * format.channels;
            std::vector<uint8_t> raw(static_cast<size_t>((last_group - first_group + 1) * group_bytes));
            if (!read_at(m_file.get(), format.data_start + first_group * group_bytes, raw.data(), static_cast<DWORD>(raw.size())))
                return false;
            for (size_t i = 0; i < bytes; ++i) {
                const auto index = m_next_byte + i;
                const auto group = index / block - first_group;
                const auto within = index % block;
                left[i] = raw[static_cast<size_t>(group * group_bytes + within)];
                right[i] = raw[static_cast<size_t>(group * group_bytes + block + within)];
            }
        } else {
            // DFF：聲道逐位元組交錯
            std::vector<uint8_t> raw(bytes * format.channels);
            if (!read_at(m_file.get(), format.data_start + m_next_byte * format.channels, raw.data(), static_cast<DWORD>(raw.size())))
                return false;
            for (size_t i = 0; i < bytes; ++i) {
                left[i] = raw[i * format.channels];
                right[i] = raw[i * format.channels + 1];
            }
        }
        m_next_byte += bytes;

        pcm.resize(count * 2);
        size_t out_left{};
        size_t out_right{};
        for (size_t i = 0; i < bytes; ++i) {
            float value{};
            if (m_converter.push(0, left[i], value) && out_left < count)
                pcm[2 * out_left++] = value * output_gain;
            if (m_converter.push(1, right[i], value) && out_right < count)
                pcm[2 * out_right++ + 1] = value * output_gain;
        }
        pcm.resize(std::min(out_left, out_right) * 2);
        return true;
    }

    wil::unique_hfile m_file;
    std::wstring m_path;
    std::optional<Format> m_format;
    Converter m_converter;
    unsigned m_bytes_per_output{8};
    uint64_t m_next_byte{};
    int64_t m_next_sample{};
    bool m_decoded{};
    bool m_busy{};
};

void ensure_thread()
{
    if (g_thread.joinable())
        return;
    g_thread = std::thread([] {
        Worker worker;
        worker.run();
    });
}

class DsdSourceInitQuit : public initquit {
public:
    void on_init() override {}
    void on_quit() override
    {
        {
            std::scoped_lock lock(g_shared.mutex);
            g_shared.quit = true;
        }
        g_shared.wake.notify_all();
        if (g_thread.joinable())
            g_thread.join();
    }
};

initquit_factory_t<DsdSourceInitQuit> g_dsd_source_initquit;

} // namespace

bool is_candidate(const metadb_handle_ptr& track)
{
    return local_path_of(track).has_value();
}

bool read(const metadb_handle_ptr& track, double end_seconds, size_t samples, audio_chunk_impl& out)
{
    const auto path = local_path_of(track);
    if (!path || samples == 0)
        return false;

    ensure_thread();
    const auto end = static_cast<int64_t>(end_seconds * output_rate);
    const auto begin = end - static_cast<int64_t>(samples);
    bool ok{};
    {
        std::scoped_lock lock(g_shared.mutex);
        g_shared.want_path = *path;
        g_shared.want_seconds = end_seconds;
        g_shared.want_tick = GetTickCount64();

        const auto available = static_cast<int64_t>(g_shared.ready.size() / 2);
        if (g_shared.ready_path == *path && begin >= g_shared.ready_start && end <= g_shared.ready_start + available) {
            out.set_data_32(g_shared.ready.data() + (begin - g_shared.ready_start) * 2, samples, 2, output_rate);
            ok = true;
        }
    }
    g_shared.wake.notify_one();
    return ok;
}

double playback_seconds()
{
    return play_control::get()->playback_get_position() + playback_offset_s;
}

bool forced()
{
#if defined(NOCTURNE_BUILD_DEV)
    static const bool value = [] {
        pfc::string8 profile;
        if (!filesystem::g_get_native_path(core_api::get_profile_path(), profile))
            return false;
        const auto flag = mmh::to_utf16(profile.c_str()) + L"\\nocturne_dsd_force";
        return GetFileAttributesW(flag.c_str()) != INVALID_FILE_ATTRIBUTES;
    }();
    return value;
#else
    return false;
#endif
}

} // namespace cui::dsd_source
