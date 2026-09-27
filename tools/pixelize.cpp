// pixelize: turns a photo texture (CC0, from ambientCG or Poly Haven) into a
// tileable pixel-art ground texture for the game, or generates one.
//
//   pixelize photo <in.jpg|png> <out.png> [options]
//   pixelize water <out.png> [options]
//
// photo: crop to a square, make it seamless (both ways), shrink it, move
// its average colour to the terrain's (--match), calm the photo's contrast,
// optionally add puddles, and cut it down to a small palette.
// water: tileable noise with ripples in the given colour.
//
// Options:
//   --size N          output side in pixels (default 256: 8 tiles of 32)
//   --colors N        palette size (default 16)
//   --match R,G,B     the average colour to end up with
//   --gain F          brighten the target by F (the game shades the ground
//                     down to 220/255 on level ground: 1.16 cancels it)
//   --contrast F      scale of the deviations from the average (default 0.8)
//   --puddles R,G,B,A water in the low spots: its colour and share (0..1)
//   --seed N          for the noise (default 1)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"

namespace {

struct Rgb {
    float r = 0, g = 0, b = 0;
};

struct Image {
    int size = 0;
    std::vector<Rgb> px;
    Rgb& at(int x, int y) { return px[static_cast<size_t>(((y % size + size) % size) * size + (x % size + size) % size)]; }
    const Rgb& at(int x, int y) const {
        return px[static_cast<size_t>(((y % size + size) % size) * size + (x % size + size) % size)];
    }
};

struct Options {
    int size = 256;
    int colors = 16;
    bool match = false;
    Rgb target{};
    float gain = 1.0f;
    float contrast = 0.8f;
    bool puddles = false;
    Rgb puddle{};
    float puddle_share = 0.0f;
    uint32_t seed = 1;
};

bool parse_rgb(const char* s, Rgb& out, float* extra = nullptr) {
    float a = 0;
    const int n = std::sscanf(s, "%f,%f,%f,%f", &out.r, &out.g, &out.b, &a);
    if (extra) *extra = a;
    return n >= 3;
}

float smooth(float t) { return t * t * (3.0f - 2.0f * t); }

// Tileable value noise: a lattice of `period` x `period` random values,
// smoothly interpolated, wrapping around.
struct Noise {
    int period;
    std::vector<float> lattice;
    Noise(int p, uint32_t seed) : period(p), lattice(static_cast<size_t>(p * p)) {
        uint32_t state = seed * 747796405u + 2891336453u;
        for (float& v : lattice) {
            state = state * 1664525u + 1013904223u;
            v = static_cast<float>(state >> 8) / static_cast<float>(1u << 24);
        }
    }
    float value(int x, int y) const {
        return lattice[static_cast<size_t>(((y % period + period) % period) * period + (x % period + period) % period)];
    }
    // u, v in [0, 1): one full period across the texture.
    float at(float u, float v) const {
        const float x = u * static_cast<float>(period);
        const float y = v * static_cast<float>(period);
        const int x0 = static_cast<int>(std::floor(x));
        const int y0 = static_cast<int>(std::floor(y));
        const float fx = smooth(x - static_cast<float>(x0));
        const float fy = smooth(y - static_cast<float>(y0));
        const float top = value(x0, y0) + (value(x0 + 1, y0) - value(x0, y0)) * fx;
        const float bottom = value(x0, y0 + 1) + (value(x0 + 1, y0 + 1) - value(x0, y0 + 1)) * fx;
        return top + (bottom - top) * fy;
    }
};

float fractal(float u, float v, uint32_t seed) {
    static std::vector<Noise> octaves;
    static uint32_t made_for = 0;
    if (octaves.empty() || made_for != seed) {
        octaves = {Noise(4, seed), Noise(8, seed + 1), Noise(16, seed + 2), Noise(32, seed + 3)};
        made_for = seed;
    }
    float sum = 0, weight = 0, amp = 1;
    for (const Noise& n : octaves) {
        sum += n.at(u, v) * amp;
        weight += amp;
        amp *= 0.5f;
    }
    return sum / weight;
}

// Makes the image tile: first across (each column blended with the one half a
// width away, near the left and right edges), then the same down.
Image seamless(const Image& in) {
    const int n = in.size;
    auto weight = [n](int i) {
        const float d = std::fabs(static_cast<float>(i) + 0.5f - static_cast<float>(n) * 0.5f) / (static_cast<float>(n) * 0.5f);
        return smooth(std::clamp((d - 0.5f) / 0.5f, 0.0f, 1.0f));
    };
    Image across = in;
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const float w = weight(x);
            const Rgb a = in.at(x, y);
            const Rgb b = in.at(x + n / 2, y);
            across.at(x, y) = {a.r + (b.r - a.r) * w, a.g + (b.g - a.g) * w, a.b + (b.b - a.b) * w};
        }
    }
    Image both = across;
    for (int y = 0; y < n; ++y) {
        const float w = weight(y);
        for (int x = 0; x < n; ++x) {
            const Rgb a = across.at(x, y);
            const Rgb b = across.at(x, y + n / 2);
            both.at(x, y) = {a.r + (b.r - a.r) * w, a.g + (b.g - a.g) * w, a.b + (b.b - a.b) * w};
        }
    }
    return both;
}

// Moves the average colour to the target and scales the deviations from it.
void match_colour(Image& img, Rgb target, float contrast) {
    Rgb mean{};
    for (const Rgb& c : img.px) {
        mean.r += c.r;
        mean.g += c.g;
        mean.b += c.b;
    }
    const auto count = static_cast<float>(img.px.size());
    mean = {mean.r / count, mean.g / count, mean.b / count};
    for (Rgb& c : img.px) {
        c = {target.r + (c.r - mean.r) * contrast, target.g + (c.g - mean.g) * contrast,
             target.b + (c.b - mean.b) * contrast};
    }
}

// Water standing in the low spots: where the noise dips, the puddle colour.
void add_puddles(Image& img, Rgb puddle, float share, uint32_t seed) {
    const int n = img.size;
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const float v = fractal(static_cast<float>(x) / static_cast<float>(n), static_cast<float>(y) / static_cast<float>(n), seed);
            const float w = std::clamp((share - v) * 8.0f + 0.5f, 0.0f, 1.0f);
            Rgb& c = img.at(x, y);
            c = {c.r + (puddle.r - c.r) * w, c.g + (puddle.g - c.g) * w, c.b + (puddle.b - c.b) * w};
        }
    }
}

// Water: slow swells and ripples running across, a few glints.
Image water(const Options& o) {
    Image img{o.size, std::vector<Rgb>(static_cast<size_t>(o.size * o.size))};
    const float pi = 3.14159265f;
    for (int y = 0; y < o.size; ++y) {
        for (int x = 0; x < o.size; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(o.size);
            const float v = static_cast<float>(y) / static_cast<float>(o.size);
            const float swell = fractal(u, v, o.seed) - 0.5f;
            const float ripple = std::sin((u * 6.0f + v * 2.0f) * 2.0f * pi + swell * 6.0f);
            // Glints on the crests, here and there along them.
            const float patch = fractal(std::fmod(u * 2.0f, 1.0f), std::fmod(v * 2.0f, 1.0f), o.seed + 11);
            const float k = swell * 36.0f + (ripple > 0.93f && patch > 0.5f ? 12.0f : 0.0f);
            img.at(x, y) = {o.target.r + k, o.target.g + k, o.target.b + k * 1.2f};
        }
    }
    return img;
}

// k-means down to a small palette, the same every run (seeded from the
// brightness quantiles), then every pixel takes its nearest colour.
void quantize(Image& img, int k) {
    std::vector<Rgb> sorted = img.px;
    auto luma = [](const Rgb& c) { return c.r * 0.3f + c.g * 0.59f + c.b * 0.11f; };
    std::sort(sorted.begin(), sorted.end(), [&](const Rgb& a, const Rgb& b) { return luma(a) < luma(b); });
    std::vector<Rgb> centre(static_cast<size_t>(k));
    for (int i = 0; i < k; ++i) centre[static_cast<size_t>(i)] = sorted[(sorted.size() - 1) * (2 * i + 1) / (2 * k)];
    std::vector<int> nearest(img.px.size());
    auto dist = [](const Rgb& a, const Rgb& b) {
        return (a.r - b.r) * (a.r - b.r) + (a.g - b.g) * (a.g - b.g) + (a.b - b.b) * (a.b - b.b);
    };
    for (int round = 0; round < 12; ++round) {
        for (size_t p = 0; p < img.px.size(); ++p) {
            int best = 0;
            for (int i = 1; i < k; ++i) {
                if (dist(img.px[p], centre[static_cast<size_t>(i)]) < dist(img.px[p], centre[static_cast<size_t>(best)])) best = i;
            }
            nearest[p] = best;
        }
        std::vector<Rgb> sum(static_cast<size_t>(k));
        std::vector<int> count(static_cast<size_t>(k), 0);
        for (size_t p = 0; p < img.px.size(); ++p) {
            Rgb& s = sum[static_cast<size_t>(nearest[p])];
            s.r += img.px[p].r;
            s.g += img.px[p].g;
            s.b += img.px[p].b;
            ++count[static_cast<size_t>(nearest[p])];
        }
        for (int i = 0; i < k; ++i) {
            const int c = count[static_cast<size_t>(i)];
            if (c > 0) centre[static_cast<size_t>(i)] = {sum[i].r / c, sum[i].g / c, sum[i].b / c};
        }
    }
    for (size_t p = 0; p < img.px.size(); ++p) img.px[p] = centre[static_cast<size_t>(nearest[p])];
}

bool write_png(const Image& img, const std::string& path) {
    std::vector<unsigned char> bytes(img.px.size() * 3);
    for (size_t p = 0; p < img.px.size(); ++p) {
        bytes[p * 3 + 0] = static_cast<unsigned char>(std::clamp(std::lround(img.px[p].r), 0L, 255L));
        bytes[p * 3 + 1] = static_cast<unsigned char>(std::clamp(std::lround(img.px[p].g), 0L, 255L));
        bytes[p * 3 + 2] = static_cast<unsigned char>(std::clamp(std::lround(img.px[p].b), 0L, 255L));
    }
    return stbi_write_png(path.c_str(), img.size, img.size, 3, bytes.data(), img.size * 3) != 0;
}

// The photo: a centred square, shrunk to `size` (the seams are hidden at full
// resolution first, where the blend is gentlest).
bool load_photo(const std::string& path, int size, Image& out) {
    int w = 0, h = 0, channels = 0;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &channels, 3);
    if (!data) return false;
    const int side = std::min(w, h);
    const int x0 = (w - side) / 2;
    const int y0 = (h - side) / 2;
    Image full{side, std::vector<Rgb>(static_cast<size_t>(side * side))};
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            const unsigned char* p = data + (static_cast<size_t>(y0 + y) * w + x0 + x) * 3;
            full.at(x, y) = {static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2])};
        }
    }
    stbi_image_free(data);
    full = seamless(full);

    std::vector<float> src(full.px.size() * 3);
    for (size_t p = 0; p < full.px.size(); ++p) {
        src[p * 3 + 0] = full.px[p].r;
        src[p * 3 + 1] = full.px[p].g;
        src[p * 3 + 2] = full.px[p].b;
    }
    std::vector<float> dst(static_cast<size_t>(size * size * 3));
    stbir_resize_float_linear(src.data(), side, side, 0, dst.data(), size, size, 0, STBIR_RGB);
    out = {size, std::vector<Rgb>(static_cast<size_t>(size * size))};
    for (size_t p = 0; p < out.px.size(); ++p) out.px[p] = {dst[p * 3 + 0], dst[p * 3 + 1], dst[p * 3 + 2]};
    return true;
}

int usage() {
    std::fprintf(stderr,
                 "usage: pixelize photo <in> <out.png> [options]\n"
                 "       pixelize water <out.png> [options]\n"
                 "options: --size N --colors N --match R,G,B --gain F --contrast F --puddles R,G,B,A --seed N\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    const std::string mode = argv[1];
    const bool photo = mode == "photo";
    if (!photo && mode != "water") return usage();
    if (photo && argc < 4) return usage();
    const std::string in = photo ? argv[2] : "";
    const std::string out = photo ? argv[3] : argv[2];

    Options o;
    for (int i = photo ? 4 : 3; i + 1 < argc; i += 2) {
        const std::string key = argv[i];
        const char* value = argv[i + 1];
        if (key == "--size") o.size = std::atoi(value);
        else if (key == "--colors") o.colors = std::atoi(value);
        else if (key == "--match") o.match = parse_rgb(value, o.target);
        else if (key == "--gain") o.gain = static_cast<float>(std::atof(value));
        else if (key == "--contrast") o.contrast = static_cast<float>(std::atof(value));
        else if (key == "--puddles") o.puddles = parse_rgb(value, o.puddle, &o.puddle_share);
        else if (key == "--seed") o.seed = static_cast<uint32_t>(std::atoi(value));
        else return usage();
    }
    if (o.size < 8 || o.size % 2 != 0 || o.colors < 2 || o.colors > 64) return usage();
    o.target = {o.target.r * o.gain, o.target.g * o.gain, o.target.b * o.gain};

    Image img;
    if (photo) {
        if (!load_photo(in, o.size, img)) {
            std::fprintf(stderr, "pixelize: can't read %s\n", in.c_str());
            return 1;
        }
        if (o.match) match_colour(img, o.target, o.contrast);
        if (o.puddles) {
            const Rgb p{o.puddle.r * o.gain, o.puddle.g * o.gain, o.puddle.b * o.gain};
            add_puddles(img, p, o.puddle_share, o.seed);
        }
    } else {
        img = water(o);
    }
    quantize(img, o.colors);
    if (!write_png(img, out)) {
        std::fprintf(stderr, "pixelize: can't write %s\n", out.c_str());
        return 1;
    }
    std::printf("%s: %dx%d, %d colours\n", out.c_str(), img.size, img.size, o.colors);
    return 0;
}
