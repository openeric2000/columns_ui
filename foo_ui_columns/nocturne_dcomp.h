#pragma once

#include <d3d11.h>
#include <dcomp.h>

#pragma comment(lib, "dcomp.lib")

// 自編版新增（2026-11-22）：DirectComposition 共用的東西，從 now_playing_style_transition.cpp 搬出來給輸出裝置顯示的滑動動畫一起用。
// 動畫交給 DWM 跑：每格不花 CPU、照螢幕更新走，主執行緒忙（換歌時 ESLyric、網頁標頭、播放統計都在重算）也不會停住或跳格

namespace cui::nocturne_dcomp {

struct Graphics {
    wil::com_ptr<ID3D11Device> d3d;
    wil::com_ptr<ID3D11DeviceContext> context;
    wil::com_ptr<IDCompositionDevice> dcomp;
};

// 整個行程共用一份（inline 函式裡的 static 只有一份）；不在結束時釋放（DLL 卸載時再碰 D3D 可能出事）。
// 裝置失效（換顯示卡驅動、遠端桌面切換）時重建；建不起來傳回 nullptr，呼叫端改用原本的做法
inline Graphics* graphics()
{
    static auto* shared = new Graphics;
    if (shared->dcomp) {
        BOOL valid{};
        if (SUCCEEDED(shared->dcomp->CheckDeviceState(&valid)) && valid)
            return shared;
        *shared = {};
    }

    for (const auto driver : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
        if (SUCCEEDED(D3D11CreateDevice(nullptr, driver, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                D3D11_SDK_VERSION, shared->d3d.put(), nullptr, shared->context.put())))
            break;
    }
    if (!shared->d3d)
        return nullptr;

    const auto dxgi = shared->d3d.try_query<IDXGIDevice>();
    if (!dxgi || FAILED(DCompositionCreateDevice(dxgi.get(), IID_PPV_ARGS(shared->dcomp.put())))) {
        *shared = {};
        return nullptr;
    }
    return shared;
}

// 一張圖（預乘透明度的 BGRA）上傳成 DirectComposition 的內容
inline wil::com_ptr<IDCompositionSurface> make_surface(Graphics& g, Gdiplus::Bitmap& bitmap)
{
    const auto width = bitmap.GetWidth();
    const auto height = bitmap.GetHeight();
    if (width == 0 || height == 0)
        return {};

    wil::com_ptr<IDCompositionSurface> surface;
    if (FAILED(g.dcomp->CreateSurface(
            width, height, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, surface.put())))
        return {};

    Gdiplus::BitmapData data{};
    const Gdiplus::Rect all(0, 0, static_cast<INT>(width), static_cast<INT>(height));
    if (bitmap.LockBits(&all, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &data) != Gdiplus::Ok)
        return {};
    const auto unlock = wil::scope_exit([&] { bitmap.UnlockBits(&data); });

    wil::com_ptr<ID3D11Texture2D> texture;
    POINT offset{};
    if (FAILED(surface->BeginDraw(nullptr, IID_PPV_ARGS(texture.put()), &offset)))
        return {};
    const D3D11_BOX box{static_cast<UINT>(offset.x), static_cast<UINT>(offset.y), 0, static_cast<UINT>(offset.x) + width,
        static_cast<UINT>(offset.y) + height, 1};
    g.context->UpdateSubresource(texture.get(), 0, &box, data.Scan0, static_cast<UINT>(data.Stride), 0);
    if (FAILED(surface->EndDraw()))
        return {};
    return surface;
}

// from → to、照 ease（時間比例 0～1 → 進度 0～1）與它的導數 slope 走 length_ms 的動畫。
// DirectComposition 只吃分段三次多項式，所以切 segments 段，每段用兩端的值與斜率做 Hermite 內插：
// 曲線本身是三次（例如 1-(1-t)^3）時一段就完全一樣；五次的 smootherstep 切 12 段，差距遠小於 0.01 px
template <typename Ease, typename Slope>
wil::com_ptr<IDCompositionAnimation> make_animation(
    Graphics& g, float from, float to, double length_ms, Ease&& ease, Slope&& slope, int segments = 12)
{
    wil::com_ptr<IDCompositionAnimation> animation;
    if (FAILED(g.dcomp->CreateAnimation(animation.put())))
        return {};

    const double total = length_ms / 1000.0;
    const auto value = [&](double x) { return from + (to - from) * ease(x); };
    // 對時間（秒）的斜率
    const auto rate = [&](double x) { return (to - from) * slope(x) / total; };

    for (int i = 0; i < segments; ++i) {
        const auto x0 = static_cast<double>(i) / segments;
        const auto x1 = static_cast<double>(i + 1) / segments;
        const auto h = (x1 - x0) * total;
        const auto v0 = value(x0);
        const auto v1 = value(x1);
        const auto d0 = rate(x0);
        const auto d1 = rate(x1);
        const auto c = (3 * (v1 - v0) / h - 2 * d0 - d1) / h;
        const auto d = (2 * (v0 - v1) / h + d0 + d1) / (h * h);
        animation->AddCubic(x0 * total, static_cast<float>(v0), static_cast<float>(d0), static_cast<float>(c),
            static_cast<float>(d));
    }
    animation->End(total, to);
    return animation;
}

} // namespace cui::nocturne_dcomp
