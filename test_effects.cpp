// Tests for effects.h (no Kinect needed). Build and run with ./build.sh test
#include "effects.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int g_failed = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      ++g_failed;                                                         \
    }                                                                     \
  } while (0)

static const int W = 320, H = 180;

// Synthetic BGRX frame: horizontal hue ramp, vertical brightness ramp, a skin-ish patch.
static std::vector<uint8_t> makeFrame(int w, int h, bool bgr, int level = 255) {
  std::vector<uint8_t> f(static_cast<size_t>(w) * h * 4);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      int r = (x * 255 / (w - 1)) * level / 255;
      int g = (y * 255 / (h - 1)) * level / 255;
      int b = (255 - x * 255 / (w - 1)) * level / 255;
      if (x > w / 3 && x < w / 2 && y > h / 3 && y < h / 2) {  // skin patch
        r = 224 * level / 255; g = 172 * level / 255; b = 148 * level / 255;
      }
      uint8_t *p = &f[(static_cast<size_t>(y) * w + x) * 4];
      p[0] = bgr ? b : r; p[1] = g; p[2] = bgr ? r : b; p[3] = 0;
    }
  return f;
}

static std::vector<uint8_t> flatFrame(int w, int h, int v) {
  std::vector<uint8_t> f(static_cast<size_t>(w) * h * 4, static_cast<uint8_t>(v));
  return f;
}

// Reference: the original toYuyv from before effects existed (byte-for-byte).
static uint8_t clampRef(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }
static void referenceYuyv(const uint8_t *in, int w, int h, bool bgr, bool mirror, float gamma, uint8_t *dst) {
  uint8_t lut[256];
  for (int i = 0; i < 256; ++i) {
    const double n = i <= 16 ? 0.0 : (i >= 235 ? 1.0 : (i - 16) / 219.0);
    lut[i] = clampRef(static_cast<int>(16 + 219.0 * std::pow(n, gamma) + 0.5));
  }
  for (int y = 0; y < h; ++y) {
    const uint8_t *row = in + static_cast<size_t>(y) * w * 4;
    uint8_t *out = dst + static_cast<size_t>(y) * w * 2;
    for (int x = 0; x < w; x += 2) {
      int r[2], g[2], b[2];
      for (int k = 0; k < 2; ++k) {
        const int sx = mirror ? (w - 1 - (x + k)) : (x + k);
        const uint8_t *p = row + sx * 4;
        b[k] = bgr ? p[0] : p[2];
        g[k] = p[1];
        r[k] = bgr ? p[2] : p[0];
      }
      for (int k = 0; k < 2; ++k)
        out[k * 2] = lut[clampRef(((66 * r[k] + 129 * g[k] + 25 * b[k] + 128) >> 8) + 16)];
      const int rr = (r[0] + r[1]) / 2, gg = (g[0] + g[1]) / 2, bb = (b[0] + b[1]) / 2;
      out[1] = clampRef(((-38 * rr - 74 * gg + 112 * bb + 128) >> 8) + 128);
      out[3] = clampRef(((112 * rr - 94 * gg - 18 * bb + 128) >> 8) + 128);
      out += 4;
    }
  }
}

static std::vector<uint8_t> run(const fx::Settings &s, const std::vector<uint8_t> &src, bool bgr = true,
                                int frames = 1, int w = W, int h = H) {
  fx::Pipeline p;
  p.configure(s);
  std::vector<uint8_t> out(static_cast<size_t>(w) * h * 2);
  for (int i = 0; i < frames; ++i) p.process(src.data(), w, h, bgr, out.data());
  return out;
}

static double meanY(const std::vector<uint8_t> &yuyv) {
  double sum = 0;
  size_t n = 0;
  for (size_t i = 0; i < yuyv.size(); i += 2, ++n) sum += yuyv[i];
  return sum / n;
}

static double meanAbsDiff(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) {
  double sum = 0;
  for (size_t i = 0; i < a.size(); ++i) sum += std::abs(int(a[i]) - int(b[i]));
  return sum / a.size();
}

static void testPlainMatchesOriginal() {
  for (bool bgr : {true, false})
    for (bool mirror : {true, false}) {
      fx::Settings s;
      s.gamma = 1.1f;
      s.mirror = mirror;
      const auto src = makeFrame(W, H, bgr);
      std::vector<uint8_t> ref(static_cast<size_t>(W) * H * 2);
      referenceYuyv(src.data(), W, H, bgr, mirror, s.gamma, ref.data());
      CHECK(run(s, src, bgr) == ref);
    }
}

static void testNeutralSettingsStayPlain() {
  fx::Settings s;
  const auto src = makeFrame(W, H, true);
  const auto plain = run(s, src);
  fx::Settings off = s;
  off.look = "polaroid";
  off.lookStrength = 0.0f;  // look selected but at zero strength
  CHECK(run(off, src) == plain);
  fx::Settings none = s;
  none.look = "none";
  none.lookStrength = 1.0f;
  CHECK(run(none, src) == plain);
  fx::Settings unknown = s;
  unknown.look = "does-not-exist";
  CHECK(run(unknown, src) == plain);
}

static void testConfigParsing() {
  fx::Settings s;
  CHECK(fx::parseLine(s, "exposure = -2.00"));
  CHECK(s.exposure == -2.0f);
  CHECK(fx::parseLine(s, "exposure = 9"));
  CHECK(s.exposure == 2.0f);  // clamped
  CHECK(fx::parseLine(s, "gamma=0.1"));
  CHECK(s.gamma == 0.3f);
  CHECK(fx::parseLine(s, "mirror = 0"));
  CHECK(!s.mirror);
  CHECK(fx::parseLine(s, "look = cinema_teal"));
  CHECK(s.look == "cinema_teal");
  CHECK(fx::parseLine(s, "lut = /home/user/my luts/film.cube"));
  CHECK(s.lutPath == "/home/user/my luts/film.cube");
  CHECK(fx::parseLine(s, "auto_brightness = 1"));
  CHECK(s.autoBrightness);
  CHECK(fx::parseLine(s, "look_strength = 3"));
  CHECK(s.lookStrength == 1.0f);
  CHECK(fx::parseLine(s, "lut_preset = sepia"));
  CHECK(s.lutPreset == "sepia");
  CHECK(fx::parseLine(s, "lut_strength = 0.4"));
  CHECK(s.lutStrength == 0.4f);
  fx::Settings legacy;  // configs written before LUTs became their own stage
  CHECK(fx::parseLine(legacy, "look = lut"));
  CHECK(legacy.look == "none" && legacy.lutPreset == "file");
  CHECK(!fx::parseLine(s, "unknown_key = 5"));
  CHECK(!fx::parseLine(s, "# comment"));
  CHECK(!fx::parseLine(s, ""));
  CHECK(!fx::parseLine(s, "garbage"));
}

static void testAdjustments() {
  const auto src = makeFrame(W, H, true);
  fx::Settings base;
  const double m0 = meanY(run(base, src));

  fx::Settings bright = base;
  bright.brightness = 0.2f;
  CHECK(meanY(run(bright, src)) > m0 + 10);

  fx::Settings dark = base;
  dark.brightness = -0.2f;
  CHECK(meanY(run(dark, src)) < m0 - 10);

  fx::Settings gray = base;
  gray.saturation = 0.0f;
  const auto g = run(gray, src);
  for (size_t i = 1; i < g.size(); i += 2) CHECK(std::abs(int(g[i]) - 128) <= 2);

  // Warm temperature: more red than blue, i.e. Cr above and Cb below the neutral result.
  fx::Settings warm = base;
  warm.temperature = 1.0f;
  const auto flat = flatFrame(W, H, 128);
  const auto neutral = run(base, flat);
  const auto w = run(warm, flat);
  CHECK(w[3] > neutral[3]);  // Cr up
  CHECK(w[1] < neutral[1]);  // Cb down
}

static void testLooks() {
  const auto src = makeFrame(W, H, true);
  fx::Settings base;
  const auto plain = run(base, src);
  for (const auto &look : fx::looks()) {
    if (std::string(look.id) == "none" || std::string(look.id) == "lut") continue;
    fx::Settings s = base;
    s.look = look.id;
    const auto out = run(s, src);
    CHECK(out.size() == plain.size());
    CHECK(meanAbsDiff(out, plain) > 1.0);  // the look visibly changes the image
    fx::Settings half = s;
    half.lookStrength = 0.5f;
    const double dHalf = meanAbsDiff(run(half, src), plain);
    CHECK(dHalf < meanAbsDiff(out, plain));  // lower strength stays closer to the original
  }
  fx::Settings bw = base;
  bw.look = "bw";
  const auto b = run(bw, src);
  for (size_t i = 1; i < b.size(); i += 2) CHECK(std::abs(int(b[i]) - 128) <= 2);
}

static void testLetterbox() {
  fx::Settings s;
  s.letterbox = true;
  const auto src = makeFrame(W, H, true);
  const auto out = run(s, src);
  CHECK(out[0] == 16 && out[1] == 128 && out[3] == 128);                       // first row is black
  const size_t mid = (static_cast<size_t>(H / 2) * W + W / 2) * 2;
  CHECK(out[mid] > 16 || out[mid + 2] > 16);                                      // centre is picture
  const size_t last = (static_cast<size_t>(H - 1) * W) * 2;
  CHECK(out[last] == 16);
}

static void testAutoBrightness() {
  fx::Settings s;
  s.autoBrightness = true;
  s.autoTarget = 0.45f;
  const auto dark = flatFrame(W, H, 40);
  const auto bright = flatFrame(W, H, 230);
  fx::Settings off;
  const double darkOff = meanY(run(off, dark)), brightOff = meanY(run(off, bright));
  const double darkOn = meanY(run(s, dark, true, 150)), brightOn = meanY(run(s, bright, true, 150));
  CHECK(darkOn > darkOff + 15);      // dark scene gets lifted
  CHECK(brightOn < brightOff - 15);  // bright scene gets pulled down
  // A scene already at the target is left alone.
  const auto mid = flatFrame(W, H, 115);
  const double midOff = meanY(run(off, mid)), midOn = meanY(run(s, mid, true, 150));
  CHECK(std::abs(midOn - midOff) < 6);
  // A single frame must not jump to the target: the change is gradual.
  const double darkOne = meanY(run(s, dark, true, 1));
  CHECK(darkOne - darkOff < (darkOn - darkOff) * 0.5);
}

static void testCubeLut() {
  const char *idPath = "/tmp/fx-test-identity.cube";
  const char *invPath = "/tmp/fx-test-invert.cube";
  for (int inv = 0; inv < 2; ++inv) {
    FILE *f = std::fopen(inv ? invPath : idPath, "w");
    CHECK(f != nullptr);
    if (!f) return;
    std::fprintf(f, "# test\nTITLE \"t\"\nLUT_3D_SIZE 2\n");
    for (int b = 0; b < 2; ++b)
      for (int g = 0; g < 2; ++g)
        for (int r = 0; r < 2; ++r)
          std::fprintf(f, "%d %d %d\n", inv ? 1 - r : r, inv ? 1 - g : g, inv ? 1 - b : b);
    std::fclose(f);
  }
  const auto src = makeFrame(W, H, true);
  fx::Settings base;
  const auto plain = run(base, src);

  fx::Settings id = base;
  id.lutPreset = "file";
  id.lutPath = idPath;
  CHECK(meanAbsDiff(run(id, src), plain) < 3.0);  // identity LUT: nearly the same image

  fx::Settings inv = base;
  inv.lutPreset = "file";
  inv.lutPath = invPath;
  CHECK(meanAbsDiff(run(inv, src), plain) > 40.0);  // inverting LUT changes it a lot

  fx::Settings missing = base;
  missing.lutPreset = "file";
  missing.lutPath = "/nonexistent/file.cube";
  CHECK(run(missing, src) == plain);  // unreadable LUT falls back to the plain image
  std::remove(idPath);
  std::remove(invPath);
}

static void testPreview() {
  const int w = 64, h = 32, pw = 16, ph = 8;
  std::vector<uint8_t> yuyv(static_cast<size_t>(w) * h * 2);
  std::vector<uint8_t> rgb(static_cast<size_t>(pw) * ph * 3, 7);
  // Mid grey (Y=126, U=V=128) must come out as grey.
  for (size_t i = 0; i < yuyv.size(); i += 4) { yuyv[i] = 126; yuyv[i + 1] = 128; yuyv[i + 2] = 126; yuyv[i + 3] = 128; }
  fx::makePreview(yuyv.data(), w, h, pw, ph, rgb.data());
  for (uint8_t c : rgb) CHECK(std::abs(int(c) - 128) <= 2);
  // BT.601 pure red (Y=81, U=90, V=240).
  for (size_t i = 0; i < yuyv.size(); i += 4) { yuyv[i] = 81; yuyv[i + 1] = 90; yuyv[i + 2] = 81; yuyv[i + 3] = 240; }
  fx::makePreview(yuyv.data(), w, h, pw, ph, rgb.data());
  CHECK(rgb[0] > 240 && rgb[1] < 20 && rgb[2] < 20);
  CHECK(rgb[rgb.size() - 3] > 240);  // last pixel is filled too
}


static void testLutPresets() {
  const auto &presets = fx::lutPresets();
  CHECK(presets.size() == 20);
  for (size_t i = 0; i < presets.size(); ++i)
    for (size_t j = i + 1; j < presets.size(); ++j) CHECK(std::string(presets[i].id) != presets[j].id);

  const auto src = makeFrame(W, H, true);
  fx::Settings base;
  const auto plain = run(base, src);
  for (const auto &preset : presets) {
    fx::Settings s = base;
    s.lutPreset = preset.id;
    const auto out = run(s, src);
    CHECK(meanAbsDiff(out, plain) > 1.0);  // every LUT visibly changes the picture
    fx::Settings half = s;
    half.lutStrength = 0.5f;
    CHECK(meanAbsDiff(run(half, src), plain) < meanAbsDiff(out, plain));
    fx::Settings zero = s;
    zero.lutStrength = 0.0f;
    CHECK(run(zero, src) == plain);  // zero strength is the untouched picture
  }

  const auto grey = flatFrame(W, H, 128);
  const auto neutral = run(base, grey);
  fx::Settings sepia = base;
  sepia.lutPreset = "sepia";
  const auto sp = run(sepia, grey);
  CHECK(sp[3] > neutral[3] && sp[1] < neutral[1]);  // warm: more red (Cr up), less blue (Cb down)
  fx::Settings cyan = base;
  cyan.lutPreset = "cyanotype";
  const auto cy = run(cyan, grey);
  CHECK(cy[1] > neutral[1] && cy[3] < neutral[3]);  // blue duotone
  fx::Settings noir = base;
  noir.lutPreset = "noir_contrast";
  const auto nr = run(noir, makeFrame(W, H, true));
  for (size_t i = 1; i < nr.size(); i += 2) CHECK(std::abs(int(nr[i]) - 128) <= 2);  // truly monochrome

  // A LUT and a look stack: the result differs from either one alone.
  fx::Settings both = base;
  both.lutPreset = "teal_orange";
  both.look = "crt";
  fx::Settings lutOnly = base;
  lutOnly.lutPreset = "teal_orange";
  fx::Settings lookOnly = base;
  lookOnly.look = "crt";
  const auto b = run(both, src);
  CHECK(meanAbsDiff(b, run(lutOnly, src)) > 1.0);
  CHECK(meanAbsDiff(b, run(lookOnly, src)) > 1.0);
}

int main() {
  testLutPresets();
  testPreview();
  testPlainMatchesOriginal();
  testNeutralSettingsStayPlain();
  testConfigParsing();
  testAdjustments();
  testLooks();
  testLetterbox();
  testAutoBrightness();
  testCubeLut();
  if (g_failed) {
    std::fprintf(stderr, "%d check(s) failed\n", g_failed);
    return 1;
  }
  std::puts("all effects tests passed");
  return 0;
}
