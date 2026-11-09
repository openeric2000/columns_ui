#include "pch.h"

#include <shlwapi.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <future>
#include <map>
#include <mutex>
#include <thread>

#include "expanded_cover_bg.h"
#include "menu_items.h"
#include "next_track_toast.h"

#pragma comment(lib, "shlwapi.lib")

// 說明見 expanded_cover_bg.h

namespace cui::expanded_cover_bg {

namespace {

// 設定改了（勾選、亮度上限、Reset page）：通知 ESLyric 重算標題格式，面板才會重新讀背景圖路徑。
// 設定值載入時也可能呼叫這個回呼，那時服務還沒就緒，所以排到主執行緒再做
void settings_changed()
{
    fb2k::inMainThread([] {
        main_menu::refresh_layout_fields();
        on_settings_changed();
    });
}

} // namespace

// 預設勾選（2026-11-08 使用者決定：封面虛化背景預設開啟）。
// 換過一次 GUID：舊版預設是不勾，各環境的設定檔裡已經存了「不勾」（舊 GUID 2b8e4f71-6a30-4d95-8c1e-f2075b93a64d），
// 直接改預設值蓋不掉；換新 GUID 後舊值自然失效，所有環境（含朋友版、主環境）升級後都變成預設勾選，使用者之後自己取消勾選會照樣記住
fbh::ConfigBool enabled(GUID{0xbee98704, 0x5ed2, 0x445c, {0x93, 0xba, 0x42, 0xed, 0xee, 0xc8, 0xcb, 0x07}}, true,
    [](bool, bool, std::optional<uint32_t>) { settings_changed(); });

fbh::ConfigUint32 cap_percent(GUID{0x91c5a3d8, 0x0e74, 0x4b2f, {0xa9, 0x68, 0x3d, 0xc1, 0x7e, 0x05, 0xb2, 0x8f}},
    cap_default, [](uint32_t, uint32_t, std::optional<uint32_t>) { settings_changed(); });

namespace {

// 做出來的圖的大小：2:1 的小圖，面板放大鋪滿時雙線性內插，所以邊緣很柔
constexpr int image_width = 64;
constexpr int image_height = 32;
// 先用高品質縮到這個大小，再用平均縮到 image_width×image_height（一次縮太多 GDI+ 的雙三次會有鋸齒）
constexpr int work_width = 256;
constexpr int work_height = 128;
// 模糊：三次方框模糊（近似高斯），半徑以小圖的像素計
constexpr int blur_radius = 3;
constexpr int blur_passes = 3;
// 壓暗用的深色（sRGB 值，同預覽頁的 SHADE）與最大壓暗比例
constexpr double shade_value = 10.0;
constexpr double max_dim = 0.95;
// 壓暗後補彩度：飽和度放大倍數＝1＋這個值×壓暗比例（壓暗 85% 約放大 2.7 倍），放大後的飽和度最高到 max_saturation
constexpr double saturation_boost = 2.0;
// 飽和度天花板（2026-11-09 使用者看了紅色封面整片太豔，對照 0.9／0.8／0.75／0.65 後選 0.8）：
// 補彩度最多補到這裡；封面本來就超過的（鮮紅、鮮藍）也壓回這裡，亮度維持不變。偏灰的封面碰不到，不受影響
constexpr double max_saturation = 0.8;
// 實際存成圖檔的大小：小圖放大到這個解析度並抖色（見 generate 裡的說明）。640×320 約 4 倍於面板放大前的像素，PNG 約 100～200 KB
constexpr int output_width = 640;
constexpr int output_height = 320;
// 抖色雜訊的幅度（8 位元色階數）
constexpr double noise_amplitude = 1.5;
// 改演算法時加一，舊檔案就不會被拿來用（2：壓暗後補彩度；3：放大並抖色，避免暗色漸層的色階斷層；4：抖色幅度 1→1.5；
// 5：飽和度天花板 0.8，原本就太豔的也往下壓）
constexpr unsigned algorithm_version = 5;
// 等背景執行緒的最長時間（ESLyric 在主執行緒要圖，所以這段時間介面是停住的）。逾時就先回空：
// 實測 ESLyric 只在換歌時才讀這個路徑，所以逾時的那首歌就沒有背景（下一首起恢復），寧可多等一點。
// 平常下一首已經提前做好，不會真的等；沒預測到（隨機、點別首歌）才會等 60～200 毫秒
constexpr std::chrono::milliseconds wait_limit{600};
// 記憶體裡最多記幾首
constexpr size_t max_entries = 300;

constexpr const char* folder_name = "nocturne-bg";

double luma(double r, double g, double b)
{
    return (0.299 * r + 0.587 * g + 0.114 * b) / 255.0;
}

std::wstring profile_folder()
{
    static const auto path = [] {
        const pfc::string8 native = ::filesystem::g_get_native_path(core_api::get_profile_path());
        return std::wstring(pfc::stringcvt::string_wide_from_utf8(native.c_str()).get_ptr()) + L"\\"
            + std::wstring(pfc::stringcvt::string_wide_from_utf8(folder_name).get_ptr());
    }();
    return path;
}

bool ensure_gdiplus()
{
    // 自己握一份 GDI+ 的啟動計數，用到進程結束（不依賴主視窗那一份什麼時候啟動、關閉）
    static const bool ok = [] {
        ULONG_PTR token{};
        const Gdiplus::GdiplusStartupInput input;
        return Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok;
    }();
    return ok;
}

bool png_encoder(CLSID& clsid)
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
        if (wcscmp(encoders[i].MimeType, L"image/png") == 0) {
            clsid = encoders[i].Clsid;
            return true;
        }
    }
    return false;
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

// 一個顏色通道的方框模糊（水平＋垂直各一次），邊緣用最近的像素補
void box_blur_once(std::vector<float>& plane, int width, int height, int radius)
{
    std::vector<float> temp(plane.size());
    const float count = static_cast<float>(radius * 2 + 1);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float sum{};
            for (int k = -radius; k <= radius; ++k)
                sum += plane[static_cast<size_t>(y) * width + std::clamp(x + k, 0, width - 1)];
            temp[static_cast<size_t>(y) * width + x] = sum / count;
        }
    }
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float sum{};
            for (int k = -radius; k <= radius; ++k)
                sum += temp[static_cast<size_t>(std::clamp(y + k, 0, height - 1)) * width + x];
            plane[static_cast<size_t>(y) * width + x] = sum / count;
        }
    }
}

// 讀封面、做圖、存檔，回傳完整路徑（UTF-8）；沒封面或失敗回空
std::optional<std::string> generate(const metadb_handle_ptr& track, uint32_t cap)
{
    try {
        album_art_data_ptr data;
        abort_callback_dummy abort;

        {
            pfc::list_single_ref_t<metadb_handle_ptr> items(track);
            pfc::list_single_ref_t<GUID> ids(album_art_ids::cover_front);
            const auto instance = album_art_manager_v2::get()->open(items, ids, abort);
            if (!instance->query(album_art_ids::cover_front, data, abort) || !data.is_valid() || data->get_size() == 0)
                return std::nullopt;
        }

        // 檔名＝封面內容＋亮度上限＋演算法版本的雜湊：同一張封面（同專輯的每首歌）共用同一個檔案
        auto hash = fnv1a(data->get_ptr(), data->get_size());
        const uint32_t salt[] = {cap, algorithm_version, image_width, image_height, output_width, output_height};
        hash = fnv1a(salt, sizeof(salt), hash);
        wchar_t name[32];
        std::swprintf(name, std::size(name), L"%016llx.png", static_cast<unsigned long long>(hash));

        const auto folder = profile_folder();
        const auto final_path = folder + L"\\" + name;

        const auto to_utf8 = [](const std::wstring& text) {
            return std::string(pfc::stringcvt::string_utf8_from_wide(text.c_str()).get_ptr());
        };

        if (std::filesystem::exists(final_path))
            return to_utf8(final_path);

        if (!ensure_gdiplus())
            return std::nullopt;

        wil::com_ptr<IStream> stream;
        stream.attach(SHCreateMemStream(static_cast<const BYTE*>(data->get_ptr()), gsl::narrow<UINT>(data->get_size())));
        if (!stream)
            return std::nullopt;

        Gdiplus::Bitmap source(stream.get());
        const auto source_width = static_cast<int>(source.GetWidth());
        const auto source_height = static_cast<int>(source.GetHeight());
        if (source.GetLastStatus() != Gdiplus::Ok || source_width <= 0 || source_height <= 0)
            return std::nullopt;

        // 取封面中間一條 2:1 的橫帶（面板用「填充」鋪滿比 2:1 更寬的展開頁，本來就只看得到中間這一段）
        int crop_width = source_width;
        int crop_height = std::max(1, source_width / 2);
        if (crop_height > source_height) {
            crop_height = source_height;
            crop_width = std::max(1, source_height * 2);
        }
        const int crop_x = (source_width - crop_width) / 2;
        const int crop_y = (source_height - crop_height) / 2;

        Gdiplus::Bitmap work(work_width, work_height, PixelFormat32bppARGB);
        {
            Gdiplus::Graphics graphics(&work);
            graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
            // 有透明的封面墊深色，不要變黑洞或白洞
            graphics.Clear(Gdiplus::Color(255, 10, 10, 10));
            graphics.DrawImage(&source, Gdiplus::Rect(0, 0, work_width, work_height), crop_x, crop_y, crop_width,
                crop_height, Gdiplus::UnitPixel);
        }

        Gdiplus::BitmapData work_data{};
        const Gdiplus::Rect work_rect(0, 0, work_width, work_height);
        if (work.LockBits(&work_rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &work_data) != Gdiplus::Ok)
            return std::nullopt;

        // 平均縮成 image_width×image_height（浮點數、每個顏色通道一個平面）
        const size_t pixel_count = static_cast<size_t>(image_width) * image_height;
        std::vector<float> plane_r(pixel_count);
        std::vector<float> plane_g(pixel_count);
        std::vector<float> plane_b(pixel_count);
        const int block_w = work_width / image_width;
        const int block_h = work_height / image_height;
        for (int y = 0; y < image_height; ++y) {
            for (int x = 0; x < image_width; ++x) {
                double sum_r{};
                double sum_g{};
                double sum_b{};
                for (int by = 0; by < block_h; ++by) {
                    const auto* row = static_cast<const BYTE*>(work_data.Scan0)
                        + static_cast<ptrdiff_t>(y * block_h + by) * work_data.Stride;
                    for (int bx = 0; bx < block_w; ++bx) {
                        const BYTE* pixel = row + static_cast<ptrdiff_t>(x * block_w + bx) * 4;
                        sum_b += pixel[0];
                        sum_g += pixel[1];
                        sum_r += pixel[2];
                    }
                }
                const double n = static_cast<double>(block_w) * block_h;
                const size_t index = static_cast<size_t>(y) * image_width + x;
                plane_r[index] = static_cast<float>(sum_r / n);
                plane_g[index] = static_cast<float>(sum_g / n);
                plane_b[index] = static_cast<float>(sum_b / n);
            }
        }
        work.UnlockBits(&work_data);

        for (int pass = 0; pass < blur_passes; ++pass) {
            box_blur_once(plane_r, image_width, image_height, blur_radius);
            box_blur_once(plane_g, image_width, image_height, blur_radius);
            box_blur_once(plane_b, image_width, image_height, blur_radius);
        }

        // 亮度：模糊後的平均 B 與最亮處 P（0～1，sRGB 數值直接算，同預覽頁）
        double mean_luma{};
        double peak_luma{};
        for (size_t i = 0; i < pixel_count; ++i) {
            const auto value = luma(plane_r[i], plane_g[i], plane_b[i]);
            mean_luma += value;
            peak_luma = std::max(peak_luma, value);
        }
        mean_luma /= static_cast<double>(pixel_count);

        const double limit = std::clamp<double>(cap, cap_min, cap_max) / 100.0;
        const double shade_luma = luma(shade_value, shade_value, shade_value);
        double dim{};
        if (mean_luma > limit)
            dim = std::max(dim, (mean_luma - limit) / (mean_luma - shade_luma));
        if (peak_luma > limit * 2.0)
            dim = std::max(dim, (peak_luma - limit * 2.0) / (peak_luma - shade_luma));
        dim = std::clamp(dim, 0.0, max_dim);

        // 壓暗後補彩度（2026-11-08 使用者看了粉彩封面壓暗後變成灰灰的、沒有色彩感）：往深色混會讓亮度與色彩一起縮，
        // 又淡又亮的封面壓到 10～35% 亮度後幾乎只剩灰色。所以壓暗越多，飽和度（HSV 的 S）放大越多——只放大色相的偏離、
        // 不動色相；再把亮度校正回壓暗後原本的值，所以「亮度上限」不受影響。白、灰封面本來沒有色（S≈0），放大後還是灰，不會被染色
        const double saturation_gain = 1.0 + saturation_boost * dim;
        // 先算出壓暗＋補彩度之後，每個小圖像素的最終顏色（浮點數，還沒進位成 8 位元）
        std::vector<float> final_r(pixel_count);
        std::vector<float> final_g(pixel_count);
        std::vector<float> final_b(pixel_count);
        for (size_t index = 0; index < pixel_count; ++index) {
            double r = plane_r[index] * (1.0 - dim) + shade_value * dim;
            double g = plane_g[index] * (1.0 - dim) + shade_value * dim;
            double b = plane_b[index] * (1.0 - dim) + shade_value * dim;

            const double top = std::max({r, g, b});
            const double bottom = std::min({r, g, b});
            if (top > 1.0 && top > bottom) {
                const double saturation = (top - bottom) / top;
                // 有壓暗才補彩度；不論有沒有壓暗，超過天花板的都壓回天花板
                const double boosted = dim > 0.0 ? saturation * saturation_gain : saturation;
                const double wanted = std::min(boosted, max_saturation);
                if (wanted != saturation) {
                    const double before = 0.299 * r + 0.587 * g + 0.114 * b;
                    const double scale = wanted / saturation;
                    r = top - (top - r) * scale;
                    g = top - (top - g) * scale;
                    b = top - (top - b) * scale;
                    const double after = 0.299 * r + 0.587 * g + 0.114 * b;
                    if (after > 0.0) {
                        const double restore = before / after;
                        r *= restore;
                        g *= restore;
                        b *= restore;
                    }
                }
            }

            final_r[index] = static_cast<float>(r);
            final_g[index] = static_cast<float>(g);
            final_b[index] = static_cast<float>(b);
        }

        // 放大到 output_width×output_height 並「抖色」（dither）。
        // 背景是暗色的緩慢漸層，8 位元每個色階只差 1，放大後同一個色階會連續好幾百像素，每個色彩通道的階梯位置又不同，
        // 在最左、最右這種變化最慢的地方就看得到一條條長條色柱（2026-11-08 使用者回報；實測 219 左邊有 257 像素寬的單一色階）。
        // 所以這裡先自己放大到接近面板的解析度、進位前加上正負 1 階的三角分布雜訊（每個通道各自獨立），
        // ESLyric 再放大時階梯就被打散成看不出來的細顆粒。雜訊用雜湊當種子，同一張封面每次做出來都一樣
        Gdiplus::Bitmap result(output_width, output_height, PixelFormat32bppARGB);
        Gdiplus::BitmapData result_data{};
        const Gdiplus::Rect result_rect(0, 0, output_width, output_height);
        if (result.LockBits(&result_rect, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &result_data)
            != Gdiplus::Ok)
            return std::nullopt;

        uint32_t seed = static_cast<uint32_t>(hash ^ (hash >> 32)) | 1u;
        const auto next_random = [&seed] {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            return seed;
        };
        // 三角分布、正負 noise_amplitude 階（ESLyric 放大時會把相鄰的雜訊平均掉一部分，所以幅度要比 1 階大一點）
        const auto noise = [&next_random] {
            return (next_random() / 4294967296.0 + next_random() / 4294967296.0 - 1.0) * noise_amplitude;
        };
        const auto sample = [](const std::vector<float>& plane, int x0, int y0, int x1, int y1, double fx, double fy) {
            const auto at = [&plane](int x, int y) { return plane[static_cast<size_t>(y) * image_width + x]; };
            const double top = at(x0, y0) * (1.0 - fx) + at(x1, y0) * fx;
            const double bottom = at(x0, y1) * (1.0 - fx) + at(x1, y1) * fx;
            return top * (1.0 - fy) + bottom * fy;
        };
        const auto finish = [](double value) { return static_cast<BYTE>(std::clamp(std::lround(value), 0l, 255l)); };

        for (int oy = 0; oy < output_height; ++oy) {
            auto* row = static_cast<BYTE*>(result_data.Scan0) + static_cast<ptrdiff_t>(oy) * result_data.Stride;
            const double v = std::clamp((oy + 0.5) * image_height / output_height - 0.5, 0.0, image_height - 1.0);
            const int y0 = static_cast<int>(v);
            const int y1 = std::min(y0 + 1, image_height - 1);
            const double fy = v - y0;
            for (int ox = 0; ox < output_width; ++ox) {
                const double u = std::clamp((ox + 0.5) * image_width / output_width - 0.5, 0.0, image_width - 1.0);
                const int x0 = static_cast<int>(u);
                const int x1 = std::min(x0 + 1, image_width - 1);
                const double fx = u - x0;
                row[ox * 4 + 0] = finish(sample(final_b, x0, y0, x1, y1, fx, fy) + noise());
                row[ox * 4 + 1] = finish(sample(final_g, x0, y0, x1, y1, fx, fy) + noise());
                row[ox * 4 + 2] = finish(sample(final_r, x0, y0, x1, y1, fx, fy) + noise());
                row[ox * 4 + 3] = 255;
            }
        }
        result.UnlockBits(&result_data);

        CLSID png{};
        if (!png_encoder(png))
            return std::nullopt;

        std::error_code ignored;
        std::filesystem::create_directories(folder, ignored);

        // 先存成暫存檔再改名，面板不會讀到寫到一半的圖
        const auto temp_path = final_path + L".tmp";
        if (result.Save(temp_path.c_str(), &png, nullptr) != Gdiplus::Ok)
            return std::nullopt;
        if (!MoveFileExW(temp_path.c_str(), final_path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW(temp_path.c_str());
            return std::nullopt;
        }

        // 順手清掉兩週沒碰過的舊圖（只在每次啟動第一次做圖時清一次）
        static std::once_flag cleanup_once;
        std::call_once(cleanup_once, [&folder] {
            std::error_code error;
            const auto limit_time = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 14);
            for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
                std::error_code entry_error;
                if (entry.is_regular_file(entry_error) && entry.last_write_time(entry_error) < limit_time)
                    std::filesystem::remove(entry.path(), entry_error);
            }
        });

        return to_utf8(final_path);
    } catch (...) {
        return std::nullopt;
    }
}

struct Entry {
    std::shared_future<std::optional<std::string>> result;
    // 呼叫端等逾時之後立起來；背景工作做完時看到它，就通知 ESLyric 重算
    std::shared_ptr<std::atomic<bool>> late;
};

std::mutex g_mutex;
std::map<std::string, Entry> g_entries;

// 找出（沒有就開始做）這首歌這個亮度上限的背景圖。做圖在背景執行緒，這裡不等；cache_hit 回報是不是原本就有
Entry acquire(const metadb_handle_ptr& track, uint32_t cap, bool& cache_hit)
{
    // 記憶體裡的鍵：檔案、子曲目、修改時間、亮度上限（任何一個變了就重做）
    char suffix[64];
    std::snprintf(suffix, sizeof(suffix), "|%d|%llu|%u", static_cast<int>(track->get_subsong_index()),
        static_cast<unsigned long long>(track->get_filetimestamp()), cap);
    const std::string key = std::string(track->get_path()) + suffix;

    std::scoped_lock lock(g_mutex);
    auto found = g_entries.find(key);
    cache_hit = found != g_entries.end();

    if (!cache_hit) {
        if (g_entries.size() >= max_entries)
            g_entries.clear();

        auto promise = std::make_shared<std::promise<std::optional<std::string>>>();
        Entry created{promise->get_future().share(), std::make_shared<std::atomic<bool>>(false)};
        found = g_entries.emplace(key, created).first;

        std::thread([promise, late = created.late, track, cap] {
            promise->set_value(generate(track, cap));
            if (late->load())
                settings_changed();
        }).detach();
    }

    return found->second;
}

// 趁現在先替預測的下一首做好圖（只在主執行緒呼叫，因為預測下一首要讀播放清單）
void prefetch_for_next_of(const metadb_handle_ptr& now_playing)
{
    if (!enabled.get() || now_playing.is_empty())
        return;

    const auto next = next_track_toast::predict_next_track(now_playing);
    if (next.is_empty())
        return;

    bool cache_hit{};
    acquire(next, std::clamp(cap_percent.get(), cap_min, cap_max), cache_hit);
}

// 換歌（主執行緒）：替下一首先做好圖，換歌時 ESLyric 在主執行緒要圖就不用等
class PrefetchCallback : public play_callback_static {
public:
    unsigned get_flags() override { return flag_on_playback_new_track; }

    void on_playback_new_track(metadb_handle_ptr track) override { prefetch_for_next_of(track); }

    void on_playback_starting(play_control::t_track_command command, bool paused) override {}
    void on_playback_stop(play_control::t_stop_reason reason) override {}
    void on_playback_seek(double time) override {}
    void on_playback_pause(bool state) override {}
    void on_playback_edited(metadb_handle_ptr track) override {}
    void on_playback_dynamic_info(const file_info& info) override {}
    void on_playback_dynamic_info_track(const file_info& info) override {}
    void on_playback_time(double time) override {}
    void on_volume_change(float new_value) override {}
};

play_callback_static_factory_t<PrefetchCallback> g_prefetch_callback;

} // namespace

void on_settings_changed()
{
    // 剛打開或換了亮度上限：目前這首歌 ESLyric 要等下一次換歌才會重讀背景，所以先替下一首做好圖
    metadb_handle_ptr track;
    if (playback_control::get()->get_now_playing(track))
        prefetch_for_next_of(track);
}

std::optional<std::string> background_path_for(const metadb_handle_ptr& track)
{
    if (!enabled.get() || track.is_empty())
        return std::nullopt;

    bool cache_hit{};
    const auto entry = acquire(track, std::clamp(cap_percent.get(), cap_min, cap_max), cache_hit);

    if (entry.result.wait_for(wait_limit) == std::future_status::ready)
        return entry.result.get();

    entry.late->store(true);
    // 標記立起來之前剛好做完的話，工作那邊看不到標記、不會通知，所以這裡再看一次
    if (entry.result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
        return entry.result.get();

    return std::nullopt;
}

} // namespace cui::expanded_cover_bg
