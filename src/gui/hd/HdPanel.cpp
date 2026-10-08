#include "HdPanel.h"

#include <cstdint>
#include <string>
#include <vector>

#include "Sprite.h"

namespace sq8l::gui::hd {

namespace {

constexpr int kW = 626, kH = 430;  // the form

// ---------------------------------------------------------------- grain
// The original's surfaces have a fine grey grain: about 1.65 levels of standard deviation,
// one form pixel in size (a pixel is 39% correlated with its neighbours). Here: white noise
// on the form's pixel grid filtered like that (fixed seed), smoothly interpolated at the
// window's resolution, so it keeps its size and character at any zoom.
const std::vector<float>& grainLattice() {
    static const std::vector<float> lattice = [] {
        const size_t n = size_t(kW) * kH;
        std::vector<float> w(n), t(n), g(n);
        uint32_t s = 0x2545F491u;
        auto next = [&s] {
            s ^= s << 13, s ^= s >> 17, s ^= s << 5;
            return (float(s >> 8) + 0.5f) * (1.f / 16777216.f);
        };
        for (size_t i = 0; i < n; i += 2) {  // Gaussian (Box-Muller)
            const float r = std::sqrt(-2.f * std::log(next())), a = 6.2831853f * next();
            w[i] = r * std::cos(a);
            if (i + 1 < n) w[i + 1] = r * std::sin(a);
        }
        const float k = 0.21f;  // [k 1 k] across and down: neighbour correlation 2k / (1 + 2k^2)
        for (int y = 0; y < kH; y++)
            for (int x = 0; x < kW; x++) {
                const size_t i = size_t(y) * kW + x;
                t[i] = w[i] + k * (w[size_t(y) * kW + std::max(x - 1, 0)] + w[size_t(y) * kW + std::min(x + 1, kW - 1)]);
            }
        double sum2 = 0;
        for (int y = 0; y < kH; y++)
            for (int x = 0; x < kW; x++) {
                const size_t i = size_t(y) * kW + x;
                g[i] = t[i] + k * (t[size_t(std::max(y - 1, 0)) * kW + x] + t[size_t(std::min(y + 1, kH - 1)) * kW + x]);
                sum2 += double(g[i]) * g[i];
            }
        const float norm = float(1.65 / std::sqrt(sum2 / double(n)));
        for (float& v : g) v *= norm;
        return g;
    }();
    return lattice;
}

}  // namespace

float grain(float x, float y) {
    const std::vector<float>& g = grainLattice();
    x -= 0.5f, y -= 0.5f;  // lattice values at the pixel centres
    const float fx0 = std::floor(x), fy0 = std::floor(y);
    float tx = x - fx0, ty = y - fy0;
    tx = tx * tx * (3 - 2 * tx), ty = ty * ty * (3 - 2 * ty);
    const int x0 = clampi(int(fx0), 0, kW - 1), x1 = clampi(int(fx0) + 1, 0, kW - 1);
    const int y0 = clampi(int(fy0), 0, kH - 1), y1 = clampi(int(fy0) + 1, 0, kH - 1);
    auto at = [&g](int xx, int yy) { return g[size_t(yy) * kW + xx]; };
    return (at(x0, y0) * (1 - tx) + at(x1, y0) * tx) * (1 - ty) + (at(x0, y1) * (1 - tx) + at(x1, y1) * tx) * ty;
}

namespace {

Color withGrain(Color c, float levels) {
    const int d = int(std::lround(levels));
    return rgb(clampi(ch(c, 16) + d, 0, 255), clampi(ch(c, 8) + d, 0, 255), clampi(ch(c, 0) + d, 0, 255));
}

// A surface: a rectangle (form coordinates) of colour c with `amount` of the grain; the
// grain fades out from x = fadeFrom to x = fadeTo when they are given.
void surface(Hd& hd, float S, float x0, float y0, float x1, float y1, Color c, float amount, float fadeFrom = 1e9f,
             float fadeTo = 1e9f) {
    hd.rect(x0 * S, y0 * S, x1 * S, y1 * S, [=](int X, int Y) {
        if (amount == 0) return c;
        const float x = (X + 0.5f) / S, y = (Y + 0.5f) / S;
        const float a = x <= fadeFrom ? amount : amount * clampf((fadeTo - x) / (fadeTo - fadeFrom), 0.f, 1.f);
        return a > 0 ? withGrain(c, a * grain(x, y)) : c;
    });
}

// ---------------------------------------------------------------- the picture
const Color kOuter = rgb(27, 27, 37), kOutline = rgb(23, 23, 32), kRedShadow = rgb(21, 4, 12);
const Color kFrame = rgb(140, 141, 160), kRed = rgb(121, 1, 36), kPanel = rgb(54, 53, 69);
const Color kLine = rgb(136, 135, 153), kLabelColour = rgb(141, 142, 160), kMenuColour = rgb(119, 118, 136);
const Color kLogo = rgb(147, 145, 168), kBezelRing = rgb(44, 43, 57);

// Signal flow lines (segments with round ends, width) and the SYNC arrow.
const struct {
    float x0, y0, x1, y1, w;
} kLines[] = {
    {147.300f, 118.700f, 327.688f, 118.700f, 1.300f},  // OSC 1 -> filter
    {327.688f, 118.700f, 363.812f, 144.500f, 1.300f},
    {363.812f, 144.500f, 419.375f, 144.500f, 1.300f},
    {148.100f, 118.700f, 148.100f, 147.000f, 2.400f},  // sync
    {147.800f, 147.000f, 483.125f, 147.062f, 1.750f},  // OSC 2 -> DCA 4
    {203.000f, 175.300f, 328.750f, 175.300f, 1.325f},  // OSC 3 -> filter
    {328.750f, 175.300f, 364.688f, 149.625f, 1.325f},
    {364.688f, 149.625f, 419.375f, 149.500f, 1.325f},
    {480.750f, 144.500f, 511.300f, 144.500f, 1.500f},  // left / right outputs
    {510.830f, 121.675f, 510.830f, 144.000f, 2.450f},
    {480.750f, 149.500f, 511.175f, 149.500f, 1.500f},
    {510.830f, 150.000f, 510.830f, 172.725f, 2.200f},
    {202.250f, 120.500f, 215.312f, 120.500f, 1.700f},  // sync switch
    {215.312f, 120.500f, 223.812f, 124.688f, 1.700f},
    {253.625f, 141.500f, 260.125f, 144.488f, 1.700f},  // AM switch
    {260.125f, 144.488f, 276.000f, 144.738f, 1.700f},
};
const float kArrow[6] = {141.69f, 123.38f, 154.19f, 123.56f, 148.0f, 133.75f};

// The SQ8L logo: rectangles.
const struct {
    float x0, y0, x1, y1;
} kLogoRects[] = {
    {27.75f, 305.15f, 43.75f, 307.8f}, {27.4f, 305.4f, 29.8f, 315.0f},     {28.4f, 312.25f, 43.75f, 314.75f},  // S
    {41.5f, 313.5f, 43.8f, 321.1f},    {26.25f, 319.25f, 43.5f, 321.85f},                                       //
    {45.75f, 305.15f, 61.3f, 307.8f},  {45.4f, 305.65f, 47.8f, 321.85f},   {59.4f, 305.65f, 61.8f, 321.85f},   // Q
    {45.5f, 319.25f, 54.25f, 321.85f}, {57.75f, 319.0f, 61.8f, 321.35f},                                       //
    {64.25f, 307.75f, 65.15f, 319.25f},                                                                        // |
    {67.75f, 305.15f, 82.75f, 307.8f}, {67.15f, 305.65f, 69.8f, 321.35f},  {81.4f, 305.4f, 83.8f, 321.6f},     // 8
    {67.4f, 312.0f, 83.5f, 314.75f},   {67.75f, 319.25f, 83.25f, 321.85f},                                     //
    {85.15f, 305.4f, 87.8f, 320.85f},  {85.4f, 319.25f, 101.85f, 321.85f},                                     // L
};

// Labels: Arial (here Liberation Sans) bold italic 14 (menus) and 11 pixels, italic 10;
// the left end of the baseline.
enum Style { kMenu, kLabel, kSmall };
const struct {
    const char* text;
    float x, y;
    Style style;
} kLabels[] = {
    {"FILE", 26.375f, 16.750f, kMenu},       {"OPTIONS", 72.375f, 16.750f, kMenu},   {"INFO", 150.375f, 16.750f, kMenu},
    {"PANIC", 199.375f, 16.875f, kMenu},     {"BANK", 323.375f, 46.750f, kLabel},    {"WRITE", 388.500f, 46.750f, kLabel},
    {"INIT", 444.250f, 46.750f, kLabel},     {"SEND", 504.250f, 46.750f, kLabel},    {"REQ", 555.375f, 46.750f, kLabel},
    {"OSC 1-3", 159.375f, 104.875f, kLabel}, {"DCA 1-3", 279.125f, 104.875f, kLabel}, {"MONO", 557.375f, 104.750f, kLabel},
    {"SYNC", 108.250f, 122.875f, kLabel},    {"MIX", 40.250f, 132.875f, kLabel},     {"FILTER", 378.875f, 132.750f, kLabel},
    {"DCA 4", 451.250f, 132.750f, kLabel},   {"AM", 255.250f, 137.750f, kLabel},     {"MODES", 554.375f, 143.875f, kLabel},
    {"/ EMU", 558.250f, 154.750f, kLabel},   {"LFO 1", 56.375f, 363.750f, kLabel},   {"2", 113.250f, 363.875f, kLabel},
    {"3", 160.250f, 363.750f, kLabel},       {"4", 205.250f, 363.875f, kLabel},      {"ENV 1", 250.250f, 363.875f, kLabel},
    {"2", 308.250f, 363.875f, kLabel},       {"3", 355.250f, 363.750f, kLabel},      {"4", 400.250f, 363.875f, kLabel},
    {"MAT 1", 447.125f, 363.750f, kLabel},   {"2", 505.250f, 363.875f, kLabel},      {"3", 551.250f, 363.750f, kLabel},
    {"Left", 502.000f, 117.000f, kSmall},    {"pan", 473.000f, 168.000f, kSmall},    {"Right", 499.000f, 185.875f, kSmall},
};

// A display bezel: the black opening (form coordinates) with rounded corners, in a dark ring,
// lit along the bottom (brighter to the right) and the right side (from a few pixels below
// the top).
void bezel(Hd& hd, float S, float x0, float y0, float x1, float y1) {
    const float r = 5.f;
    const float hx0 = x0 - 1.2f, hy0 = y0 - 1.2f, hx1 = x1 + 2.6f, hy1 = y1 + 1.6f;
    hd.roundRect(hx0 * S, hy0 * S, hx1 * S, hy1 * S, (r + 1.5f) * S, [=](float fx, float fy) {
        const float x = hx0 + fx * (hx1 - hx0), y = hy0 + fy * (hy1 - hy0);
        const float level = y > y1 ? 99.f + 31.f * clampf(((x - x0) / (x1 - x0) - 0.1f) / 0.5f, 0.f, 1.f)
                                   : 54.f + 68.f * clampf((y - y0 - 3.f) / 9.f, 0.f, 1.f);
        const float k = level / 142.f;
        return rgb(int(140 * k + 0.5f), int(141 * k + 0.5f), int(160 * k + 0.5f));
    });
    hd.roundRect(hx0 * S, hy0 * S, (x1 + 1.6f) * S, (y1 + 0.6f) * S, (r + 1.f) * S, [](float, float) { return kBezelRing; });
    hd.roundRect(x0 * S, y0 * S, x1 * S, y1 * S, r * S, [](float, float) { return Color(0); });
}

// The screws: the original's pixels, smoothly enlarged, as a darkening of the surface they
// sit on (so the grain goes on around them).
void screw(Hd& hd, float S, float cx, float cy) {
    const Bitmap& bg = assets::background();
    const float R = 9.f;
    const Rect k = hd.area((cx - R) * S, (cy - R) * S, (cx + R) * S, (cy + R) * S, 0);
    if (k.empty()) return;
    float base[3] = {};  // the surface around the screw in the original
    int n = 0;
    for (int a = 0; a < 64; a++) {
        const float t = a * 6.2831853f / 64;
        const int x = clampi(int(cx + (R + 0.5f) * std::cos(t)), 0, kW - 1), y = clampi(int(cy + (R + 0.5f) * std::sin(t)), 0, kH - 1);
        const Color c = bg.pixel(x, y);
        base[0] += ch(c, 16), base[1] += ch(c, 8), base[2] += ch(c, 0), n++;
    }
    for (float& b : base) b = std::max(b / float(n), 1.f);
    for (int Y = k.top; Y < k.bottom; Y++)
        for (int X = k.left; X < k.right; X++) {
            const float x = (X + 0.5f) / S - 0.5f, y = (Y + 0.5f) / S - 0.5f;
            const float r = std::hypot(x + 0.5f - cx, y + 0.5f - cy);
            if (r >= R) continue;
            const int ix = int(std::floor(x)), iy = int(std::floor(y));
            const float tx = x - ix, ty = y - iy;
            auto px = [&bg](int xx, int yy) { return bg.pixel(clampi(xx, 0, kW - 1), clampi(yy, 0, kH - 1)); };
            const Color c00 = px(ix, iy), c10 = px(ix + 1, iy), c01 = px(ix, iy + 1), c11 = px(ix + 1, iy + 1);
            const float fade = clampf((R - r) / 2.5f, 0.f, 1.f);
            Color* p = hd.b.row(Y) + X;
            int out[3];
            for (int c = 0; c < 3; c++) {
                const int sh = 16 - 8 * c;
                const float v = (ch(c00, sh) * (1 - tx) + ch(c10, sh) * tx) * (1 - ty) + (ch(c01, sh) * (1 - tx) + ch(c11, sh) * tx) * ty;
                const float ratio = 1 + (v / base[c] - 1) * fade;
                out[c] = clampi(int(std::lround(ch(*p, sh) * ratio)), 0, 255);
            }
            *p = rgb(out[0], out[1], out[2]);
        }
}

}  // namespace

void drawPanel(Hd& hd, float S, TextRenderer& text) {
    // the window around the panel (grain behind the menus, fading out to the right), the
    // panel's outline and frame, the red band, the two surfaces
    hd.fill(hd.clip, kOuter);  // (all of it: the window may be a fraction of a pixel larger)
    surface(hd, S, 0, 0, 300, 22, kOuter, 0.69f, 230, 300);
    surface(hd, S, 7, 22, 618, 409, kOutline, 0);
    surface(hd, S, 9, 23, 616, 24, kRedShadow, 0);
    surface(hd, S, 9, 24, 616, 407, kFrame, 1);
    surface(hd, S, 9, 24, 616, 33, kRed, 0.8f);
    surface(hd, S, 16, 33, 609, 81, kPanel, 1);
    surface(hd, S, 16, 88, 609, 398, kPanel, 1);
    // the displays
    bezel(hd, S, 23.2f, 41.2f, 271.4f, 69.4f);
    bezel(hd, S, 25.2f, 242.2f, 596.4f, 297.4f);
    // the signal flow
    for (const auto& l : kLines) hd.capsule(l.x0 * S, l.y0 * S, l.x1 * S, l.y1 * S, l.w / 2 * S, 1.f, kLine);
    hd.triangle(kArrow[0] * S, kArrow[1] * S, kArrow[2] * S, kArrow[3] * S, kArrow[4] * S, kArrow[5] * S, kLine);
    for (const auto& r : kLogoRects) hd.rect(r.x0 * S, r.y0 * S, r.x1 * S, r.y1 * S, kLogo);
    for (const auto& l : kLabels) {
        Font f;
        f.face = "Arial";
        f.height = l.style == kMenu ? -14 : l.style == kLabel ? -11 : -10;
        f.bold = l.style != kSmall;
        f.italic = true;
        f.color = l.style == kMenu ? kMenuColour : kLabelColour;
        // (the original's strokes are heavier: its text was hinted at 1x)
        text.drawTextScaled(hd.b, l.x * S, l.y * S, S, hd.clip, f, l.text, l.style == kMenu ? 0.33f : l.style == kLabel ? 0.5f : 0.45f);
    }
    for (const auto& s : {std::pair{12.92f, 11.89f}, {613.0f, 11.81f}, {30.26f, 205.92f}, {595.29f, 206.04f},
                          {12.89f, 418.66f}, {612.89f, 418.66f}})
        screw(hd, S, s.first, s.second);
}

}  // namespace sq8l::gui::hd
