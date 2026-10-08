#include "HdPanel.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sq8l::gui::hd {

namespace {

constexpr int kW = 626, kH = 430;  // the form

// ---------------------------------------------------------------- grain
// The original's surfaces have a fine grey grain: about 1.65 levels of standard deviation,
// a form pixel in size. Here: white noise on a grid of kCells x kCells points per form
// pixel (fixed seed), a little correlated with its neighbours, smoothly interpolated at the
// window's resolution: as strong as the original's at 1x (the grid's points fall on the
// pixel centres there), finer and sharper than the original's pixels when enlarged.
constexpr int kCells = 2;
constexpr float kNeighbour = 0.1f;  // [k 1 k] filter across and down
constexpr int kGW = kW * kCells + 1, kGH = kH * kCells + 1;

const std::vector<float>& grainLattice() {
    static const std::vector<float> lattice = [] {
        const size_t n = size_t(kGW) * kGH;
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
        auto at = [](const std::vector<float>& v, int x, int y) {
            return v[size_t(clampi(y, 0, kGH - 1)) * kGW + size_t(clampi(x, 0, kGW - 1))];
        };
        for (int y = 0; y < kGH; y++)
            for (int x = 0; x < kGW; x++)
                t[size_t(y) * kGW + x] = at(w, x, y) + kNeighbour * (at(w, x - 1, y) + at(w, x + 1, y));
        double sum2 = 0;
        for (int y = 0; y < kGH; y++)
            for (int x = 0; x < kGW; x++) {
                const float v = at(t, x, y) + kNeighbour * (at(t, x, y - 1) + at(t, x, y + 1));
                g[size_t(y) * kGW + x] = v;
                sum2 += double(v) * v;
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
    // grid point i at form x = (i + off) / kCells: at 1x the pixel centres fall on grid points
    constexpr float off = kCells % 2 ? 0.5f : 0.f;
    const float u = x * kCells - off, v = y * kCells - off;
    const float u0 = std::floor(u), v0 = std::floor(v);
    float tx = u - u0, ty = v - v0;
    tx = tx * tx * (3 - 2 * tx), ty = ty * ty * (3 - 2 * ty);
    const int x0 = clampi(int(u0), 0, kGW - 1), x1 = clampi(int(u0) + 1, 0, kGW - 1);
    const int y0 = clampi(int(v0), 0, kGH - 1), y1 = clampi(int(v0) + 1, 0, kGH - 1);
    auto at = [&g](int xx, int yy) { return g[size_t(yy) * kGW + xx]; };
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
    {"FILE", 26.375f, 16.750f, kMenu}, {"OPTIONS", 72.375f, 16.750f, kMenu}, {"INFO", 150.375f, 16.750f, kMenu},
    {"PANIC", 199.375f, 16.875f, kMenu}, {"BANK", 323.375f, 46.750f, kLabel}, {"WRITE", 388.500f, 46.750f, kLabel},
    {"INIT", 444.250f, 46.750f, kLabel}, {"SEND", 504.250f, 46.750f, kLabel}, {"REQ", 555.375f, 46.750f, kLabel},
    {"OSC 1-3", 159.375f, 104.875f, kLabel}, {"DCA 1-3", 279.125f, 104.875f, kLabel},
    {"MONO", 557.375f, 104.750f, kLabel},
    {"SYNC", 108.250f, 122.875f, kLabel}, {"MIX", 40.250f, 132.875f, kLabel}, {"FILTER", 378.875f, 132.750f, kLabel},
    {"DCA 4", 451.250f, 132.750f, kLabel}, {"AM", 255.250f, 137.750f, kLabel}, {"MODES", 554.375f, 143.875f, kLabel},
    {"/ EMU", 558.250f, 154.750f, kLabel}, {"LFO 1", 56.375f, 363.750f, kLabel}, {"2", 113.250f, 363.875f, kLabel},
    {"3", 160.250f, 363.750f, kLabel}, {"4", 205.250f, 363.875f, kLabel}, {"ENV 1", 250.250f, 363.875f, kLabel},
    {"2", 308.250f, 363.875f, kLabel}, {"3", 355.250f, 363.750f, kLabel}, {"4", 400.250f, 363.875f, kLabel},
    {"MAT 1", 447.125f, 363.750f, kLabel}, {"2", 505.250f, 363.875f, kLabel}, {"3", 551.250f, 363.750f, kLabel},
    {"Left", 502.000f, 117.000f, kSmall}, {"pan", 473.000f, 168.000f, kSmall}, {"Right", 499.000f, 185.875f, kSmall},
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
    hd.roundRect(hx0 * S, hy0 * S, (x1 + 1.6f) * S, (y1 + 0.6f) * S, (r + 1.f) * S,
                 [](float, float) { return kBezelRing; });
    hd.roundRect(x0 * S, y0 * S, x1 * S, y1 * S, r * S, [](float, float) { return Color(0); });
}

// A screw (all six are the same picture): a black head in a soft shadow, the recess's rim as
// a ring lit along its upper half, brightest at the upper left, and a dark grey centre.
// Measured from the original (form pixels; the ring's brightness at 22.5 + 45k degrees from
// the right, clockwise).
void screw(Hd& hd, float S, float cx, float cy) {
    constexpr float kShadow = 8.6f, kHead = 6.0f, kRing = 3.25f, kRingHalf = 0.75f, kCentre = 2.4f;
    constexpr float kRingLight[8] = {27, 25, 33, 36, 50, 33, 62, 60};
    const Rect k = hd.area((cx - kShadow) * S, (cy - kShadow) * S, (cx + kShadow) * S, (cy + kShadow) * S, 0);
    const Color head = rgb(13, 13, 15), centre = rgb(16, 16, 18);
    const float edge = std::max(S * 0.3f, 1.f);  // the head's edge: a little soft, as in the original
    for (int Y = k.top; Y < k.bottom; Y++)
        for (int X = k.left; X < k.right; X++) {
            const float dx = (X + 0.5f) / S - cx, dy = (Y + 0.5f) / S - cy, r = std::hypot(dx, dy);
            if (r >= kShadow) continue;
            Color* p = hd.b.row(Y) + X;
            Color c = mix(*p, 0, 0.58f * clampf((kShadow - r) / (kShadow - kHead), 0.f, 1.f));
            c = mix(c, head, clampf((kHead - r) * S / edge + 0.5f, 0.f, 1.f));
            c = mix(c, centre, clampf((kCentre - r) * S + 0.5f, 0.f, 1.f));
            const float ring = clampf((kRingHalf - std::fabs(r - kRing)) * S + 0.5f, 0.f, 1.f);
            if (ring > 0) {
                float a = std::atan2(dy, dx) * 57.29578f;  // 0 = right, 90 = down
                if (a < 0) a += 360;
                const float u = (a + 337.5f) / 45;  // (a - 22.5) / 45, wrapped
                const int i0 = int(u) % 8, i1 = (i0 + 1) % 8;
                const float f = u - std::floor(u);
                float d = std::fabs(a - 215);  // the highlight at the upper left
                d = std::min(d, 360 - d);
                const float l =
                    kRingLight[i0] + (kRingLight[i1] - kRingLight[i0]) * f + 40 * std::exp(-(d / 18) * (d / 18));
                c = mix(c, rgb(int(l), int(l), int(l * 1.06f)), ring);
            }
            *p = c;
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
        const float embolden = l.style == kMenu ? 0.33f : l.style == kLabel ? 0.5f : 0.45f;
        text.drawTextScaled(hd.b, l.x * S, l.y * S, S, hd.clip, f, l.text, embolden);
    }
    for (const auto& s : {std::pair{12.86f, 11.88f}, {612.88f, 11.88f}, {30.40f, 206.04f}, {595.40f, 206.07f},
                          {12.80f, 418.96f}, {612.80f, 418.97f}})
        screw(hd, S, s.first, s.second);
}

}  // namespace sq8l::gui::hd
