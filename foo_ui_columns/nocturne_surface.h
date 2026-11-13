#pragma once

// 自編版新增（2026-11-12）：浮動元件的統一外觀（使用者在預覽頁選的「甲」，以標頭的提示膠囊為基準）
// - 實心底：灰 38、不透明度 240（約 94%）；細邊：白 8%；柔和陰影往下偏；卡片圓角 12、提示泡圓角 8（DIP）
// - 用在播放列上的浮動元件（輸出裝置清單、音量滑桿與 dB 提示泡、滑鼠提示）與 VU／頻譜樣式卡
// - 浮在展開頁上、使用者另外選過材質的保留材質，只統一圓角與邊框：
//   下一首卡片維持半透明（11/08 使用者要求透出背景色）、展開頁模式卡維持霧面玻璃（預覽頁 B）
// 要改整體外觀只改這裡

namespace cui::nocturne_surface {

inline constexpr BYTE fill_grey = 38;
inline constexpr BYTE fill_alpha = 240;
inline constexpr BYTE edge_alpha = 20;
inline constexpr float card_radius_dip = 12.f;
inline constexpr float tip_radius_dip = 8.f;
// 文字：主要、次要
inline constexpr BYTE text_bright = 241;
inline constexpr BYTE text_muted = 170;

inline Gdiplus::Color fill_colour(BYTE alpha = fill_alpha)
{
    return Gdiplus::Color(alpha, fill_grey, fill_grey, fill_grey);
}

inline Gdiplus::Color edge_colour()
{
    return Gdiplus::Color(edge_alpha, 255, 255, 255);
}

inline void add_rounded(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& rect, float radius)
{
    radius = std::min({radius, rect.Width / 2.f, rect.Height / 2.f});
    if (radius <= 0.f) {
        path.AddRectangle(rect);
        return;
    }
    const auto d = radius * 2.f;
    path.AddArc(rect.X, rect.Y, d, d, 180, 90);
    path.AddArc(rect.GetRight() - d, rect.Y, d, d, 270, 90);
    path.AddArc(rect.GetRight() - d, rect.GetBottom() - d, d, d, 0, 90);
    path.AddArc(rect.X, rect.GetBottom() - d, d, d, 90, 90);
    path.CloseFigure();
}

// 柔和陰影：往外擴的幾層很淡的圓角框，整體往下偏 shadow 的三分之一。shadow＝陰影往外擴的像素（視窗四周要留這麼多）
inline void paint_shadow(Gdiplus::Graphics& g, const Gdiplus::RectF& body, float radius, int shadow)
{
    if (shadow <= 0)
        return;
    for (int i = shadow; i >= 1; i -= std::max(1, shadow / 5)) {
        Gdiplus::GraphicsPath path;
        add_rounded(path,
            Gdiplus::RectF(body.X - i, body.Y - i + shadow / 3.f, body.Width + 2.f * i, body.Height + 2.f * i),
            radius + i);
        const Gdiplus::SolidBrush brush(Gdiplus::Color(static_cast<BYTE>(10 + (shadow - i) * 2), 0, 0, 0));
        g.FillPath(&brush, &path);
    }
}

// 實心面板：陰影＋底＋細邊（g 要先開反鋸齒）
inline void paint_panel(Gdiplus::Graphics& g, const Gdiplus::RectF& body, float radius, int shadow)
{
    paint_shadow(g, body, radius, shadow);
    Gdiplus::GraphicsPath path;
    add_rounded(path, body, radius);
    const Gdiplus::SolidBrush fill(fill_colour());
    g.FillPath(&fill, &path);
    const Gdiplus::Pen edge(edge_colour(), 1.f);
    g.DrawPath(&edge, &path);
}

} // namespace cui::nocturne_surface
