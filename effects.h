// Image pipeline for the Kinect webcam: camera-style adjustments, looks and .cube LUTs.
//
// Header-only and free of libfreenect2 so it can be tested without a Kinect
// (see test_effects.cpp). With every setting at its default the output is
// byte-identical to the original BGRX/RGBX -> YUYV conversion.
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace fx {

inline uint8_t clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : static_cast<uint8_t>(v)); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// User settings, read from ~/.config/kinect2-webcam.conf (key = value lines).
struct Settings {
  // Camera
  float exposure = 0.0f;      // hardware auto-exposure compensation, [-2, 2]; negative is darker
  bool manualExposure = false;  // use exposureMs/gain instead of auto exposure
  float exposureMs = 20.0f;   // manual integration time, [1, 66]
  float gain = 1.0f;          // manual analog gain, [1, 4]
  bool mirror = true;
  // Image
  float gamma = 1.0f;         // luma curve, [0.3, 3]; above 1 darkens midtones
  bool autoBrightness = false;  // software auto-brightness that follows the scene
  float autoTarget = 0.45f;   // mean brightness the auto mode aims for, [0.25, 0.7]
  float brightness = 0.0f;    // [-0.3, 0.3]
  float contrast = 1.0f;      // [0.5, 1.5]
  float saturation = 1.0f;    // [0, 2]
  float temperature = 0.0f;   // [-1, 1]; positive is warmer
  // Look
  std::string look = "none";
  float lookStrength = 1.0f;  // [0, 1]
  bool letterbox = false;     // 2.39:1 cinema bars
  // Colour LUT: a built-in preset id, "file" for lutPath, or "none". Applied before the look.
  std::string lutPreset = "none";
  float lutStrength = 1.0f;   // [0, 1]
  std::string lutPath;        // .cube file used when lutPreset is "file"
};

inline std::string trim(const std::string &s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

// Apply one "key = value" line. Returns false for blank, comment, malformed and unknown lines.
inline bool parseLine(Settings &s, const std::string &rawLine) {
  const std::string line = trim(rawLine);
  if (line.empty() || line[0] == '#') return false;
  const size_t eq = line.find('=');
  if (eq == std::string::npos) return false;
  const std::string key = trim(line.substr(0, eq));
  const std::string text = trim(line.substr(eq + 1));
  if (key == "look") {
    if (text == "lut") { s.look = "none"; s.lutPreset = "file"; }  // configs from before LUTs had their own stage
    else s.look = text;
    return true;
  }
  if (key == "lut") { s.lutPath = text; return true; }
  if (key == "lut_preset") { s.lutPreset = text; return true; }

  char *end = nullptr;
  const float v = std::strtof(text.c_str(), &end);
  if (end == text.c_str()) return false;
  if (key == "exposure") s.exposure = clampf(v, -2.0f, 2.0f);
  else if (key == "gamma") s.gamma = clampf(v, 0.3f, 3.0f);
  else if (key == "mirror") s.mirror = v != 0.0f;
  else if (key == "manual_exposure") s.manualExposure = v != 0.0f;
  else if (key == "exposure_ms") s.exposureMs = clampf(v, 1.0f, 66.0f);
  else if (key == "gain") s.gain = clampf(v, 1.0f, 4.0f);
  else if (key == "auto_brightness") s.autoBrightness = v != 0.0f;
  else if (key == "auto_target") s.autoTarget = clampf(v, 0.25f, 0.7f);
  else if (key == "brightness") s.brightness = clampf(v, -0.3f, 0.3f);
  else if (key == "contrast") s.contrast = clampf(v, 0.5f, 1.5f);
  else if (key == "saturation") s.saturation = clampf(v, 0.0f, 2.0f);
  else if (key == "temperature") s.temperature = clampf(v, -1.0f, 1.0f);
  else if (key == "look_strength") s.lookStrength = clampf(v, 0.0f, 1.0f);
  else if (key == "lut_strength") s.lutStrength = clampf(v, 0.0f, 1.0f);
  else if (key == "letterbox") s.letterbox = v != 0.0f;
  else return false;
  return true;
}

inline void loadSettings(Settings &s, const std::string &path) {
  if (FILE *f = std::fopen(path.c_str(), "r")) {
    char buf[1024];
    while (std::fgets(buf, sizeof(buf), f)) parseLine(s, buf);
    std::fclose(f);
  }
}

// A look is a colour grade plus optional film/video artefacts. Tint values are added to
// shadows/highlights of each channel (r, g, b); everything scales with the look strength.
struct LookParams {
  const char *id;
  const char *label;
  float sat;          // saturation multiplier
  float contrast;     // around mid-grey
  float lift;         // raises the blacks (faded film)
  float shadow[3];    // tint added to shadows
  float highlight[3]; // tint added to highlights
  int mono;           // 0 colour, 1 black & white, 2 green phosphor
  float vignette;     // corner darkening, 0..1
  float grain;        // film/video noise, 0..1
  float scanlines;    // dark horizontal lines, 0..1
  float mask;         // RGB phosphor mask, 0..1
  float chromaShift;  // pixels the colour lags behind the brightness (video bleed)
  float chromaBlur;   // pixels of horizontal colour smearing
};

inline const std::vector<LookParams> &looks() {
  static const std::vector<LookParams> table = {
      {"none", "Natural", 1, 1, 0, {0, 0, 0}, {0, 0, 0}, 0, 0, 0, 0, 0, 0, 0},
      {"polaroid", "Polaroid", 0.85f, 0.90f, 0.08f, {0.00f, 0.04f, 0.05f}, {0.09f, 0.05f, -0.04f}, 0, 0.38f, 0.10f, 0, 0, 0, 0},
      {"analog", "Analog film", 0.90f, 1.06f, 0.03f, {0.01f, 0.02f, 0.01f}, {0.05f, 0.02f, -0.02f}, 0, 0.28f, 0.22f, 0, 0, 0, 0},
      {"bw", "Black & white", 1, 1.15f, 0, {0, 0, 0}, {0, 0, 0}, 1, 0.20f, 0.12f, 0, 0, 0, 0},
      {"crt", "CRT monitor", 1.20f, 1.10f, 0, {0, 0, 0}, {0, 0, 0}, 0, 0.50f, 0.04f, 0.35f, 0.25f, 1, 0},
      {"tv80", "80s TV", 0.85f, 0.95f, 0.05f, {0.02f, 0.00f, 0.04f}, {0.04f, 0.00f, 0.02f}, 0, 0.30f, 0.18f, 0.18f, 0, 5, 6},
      {"green", "Green monitor", 1, 1.10f, 0.02f, {0, 0, 0}, {0, 0, 0}, 2, 0.45f, 0.08f, 0.30f, 0, 0, 0},
      {"cinema_teal", "Cinema: Teal & orange", 1.10f, 1.12f, 0, {-0.03f, 0.03f, 0.06f}, {0.07f, 0.02f, -0.05f}, 0, 0.25f, 0.08f, 0, 0, 0, 0},
      {"cinema_warm", "Cinema: Warm", 0.95f, 1.05f, 0.03f, {0.02f, 0.01f, 0.00f}, {0.06f, 0.03f, -0.04f}, 0, 0.22f, 0.12f, 0, 0, 0, 0},
      {"cinema_cold", "Cinema: Cold", 0.72f, 1.12f, 0, {-0.02f, 0.02f, 0.06f}, {-0.02f, 0.02f, 0.05f}, 0, 0.30f, 0.10f, 0, 0, 0, 0},
      {"noir", "Cinema: Noir", 1, 1.25f, 0.02f, {0, 0, 0}, {0, 0, 0}, 1, 0.50f, 0.22f, 0, 0, 0, 0},
  };
  return table;
}

inline const LookParams *findLook(const std::string &id) {
  for (const auto &l : looks())
    if (id == l.id) return &l;
  return nullptr;
}


// A built-in colour LUT is a grade described by a few intuitive controls. It is baked into a
// 3D LUT when selected, so it costs the same per pixel as a LUT loaded from a .cube file.
struct LutPreset {
  const char *id;
  const char *label;
  float mix[9];         // channel mixer, rows give the new r, g, b from the old r, g, b
  float lift[3];        // raises the blacks of each channel
  float gain[3];        // per-channel multiplier
  float gamma[3];       // per-channel midtones, above 1 is brighter
  float contrast;       // S-curve amount, 0..1
  float sat;            // saturation (relative to luma)
  float shadow[3];      // tint added mostly to the shadows
  float highlight[3];   // tint added mostly to the highlights
  float mapAmount;      // gradient map (duotone) blend, 0..1
  float mapDark[3], mapLight[3];
};

inline const std::vector<LutPreset> &lutPresets() {
  static const std::vector<LutPreset> table = {
      {"warm_portrait", "Warm Portrait", {1, 0, 0, 0, 1, 0, 0, 0, 1}, {0.03f, 0.03f, 0.03f}, {1.04f, 1.0f, 0.94f}, {1.05f, 1.03f, 1.0f}, 0.25f, 0.95f, {0.01f, 0, 0}, {0.03f, 0.02f, -0.02f}, 0, {0, 0, 0}, {1, 1, 1}},
      {"golden_hour", "Golden Hour", {1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 0, 0}, {1.10f, 1.02f, 0.84f}, {1.05f, 1.05f, 1.05f}, 0.30f, 1.15f, {0.02f, 0, -0.02f}, {0.06f, 0.03f, -0.05f}, 0, {0, 0, 0}, {1, 1, 1}},
      {"cool_morning", "Cool Morning", {1, 0, 0, 0, 1, 0, 0, 0, 1}, {0.02f, 0.02f, 0.02f}, {0.93f, 0.99f, 1.08f}, {1, 1, 1}, 0.20f, 0.88f, {-0.01f, 0.01f, 0.04f}, {-0.02f, 0, 0.03f}, 0, {0, 0, 0}, {1, 1, 1}},
      {"vivid_pop", "Vivid Pop", {1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 0, 0}, {1, 1, 1}, {1.02f, 1.02f, 1.02f}, 0.45f, 1.45f, {0, 0, 0}, {0, 0, 0}, 0, {0, 0, 0}, {1, 1, 1}},
      {"soft_pastel", "Soft Pastel", {1, 0, 0, 0, 1, 0, 0, 0, 1}, {0.10f, 0.10f, 0.10f}, {0.96f, 0.96f, 0.96f}, {1.12f, 1.12f, 1.12f}, 0, 0.82f, {0.03f, 0, 0.02f}, {0.02f, 0, 0.03f}, 0, {0, 0, 0}, {1, 1, 1}},
      {"faded_vintage", "Faded Vintage", {1, 0, 0, 0, 1, 0, 0, 0, 1}, {0.12f, 0.12f, 0.12f}, {1.0f, 0.97f, 0.90f}, {1, 1, 1}, 0.20f, 0.78f, {0, 0.03f, 0.02f}, {0.05f, 0.03f, -0.03f}, 0, {0, 0, 0}, {1, 1, 1}},
      {"cross_process", "Cross Process", {1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 0, 0}, {1.08f, 1.0f, 0.85f}, {1.0f, 0.95f, 1.1f}, 0.50f, 1.25f, {-0.04f, 0.05f, 0.07f}, {0.07f, 0.05f, -0.10f}, 0, {0, 0, 0}, {1, 1, 1}},
      {"bleach_bypass", "Bleach Bypass", {1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 0, 0}, {1, 1, 1}, {0.95f, 0.95f, 0.95f}, 0.70f, 0.45f, {0, 0, 0}, {0, 0, 0}, 0, {0, 0, 0}, {1, 1, 1}},
      {"teal_orange", "Teal & Orange", {1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 0, 0}, {1, 1, 1}, {1, 1, 1}, 0.35f, 1.15f, {-0.06f, 0.03f, 0.09f}, {0.10f, 0.03f, -0.08f}, 0, {0, 0, 0}, {1, 1, 1}},
      {"moody_teal", "Moody Teal", {1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 0, 0}, {0.92f, 0.92f, 0.92f}, {0.92f, 0.92f, 0.92f}, 0.40f, 0.85f, {-0.05f, 0.04f, 0.07f}, {0, 0.02f, 0.02f}, 0, {0, 0, 0}, {1, 1, 1}},
      {"sepia", "Sepia", {1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 0, 0}, {1, 1, 1}, {1, 1, 1}, 0.20f, 0.0f, {0, 0, 0}, {0, 0, 0}, 1.0f, {0.10f, 0.06f, 0.03f}, {1.0f, 0.89f, 0.72f}},
      {"cyanotype", "Cyanotype", {1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 0, 0}, {1, 1, 1}, {1, 1, 1}, 0.30f, 0.0f, {0, 0, 0}, {0, 0, 0}, 1.0f, {0.02f, 0.08f, 0.18f}, {0.85f, 0.95f, 1.0f}},
      {"noir_contrast", "Noir Contrast", {1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 0, 0}, {1, 1, 1}, {0.95f, 0.95f, 0.95f}, 0.80f, 0.0f, {0, 0, 0}, {0, 0, 0}, 0, {0, 0, 0}, {1, 1, 1}},
      {"silver_gelatin", "Silver Gelatin", {1, 0, 0, 0, 1, 0, 0, 0, 1}, {0.04f, 0.04f, 0.04f}, {0.98f, 0.98f, 0.98f}, {1, 1, 1}, 0.35f, 0.0f, {0, 0.005f, 0.015f}, {0, 0, 0}, 0, {0, 0, 0}, {1, 1, 1}},
      {"day_for_night", "Day for Night", {1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 0, 0}, {0.60f, 0.75f, 1.0f}, {0.85f, 0.85f, 0.85f}, 0.35f, 0.60f, {-0.02f, 0, 0.05f}, {0, 0, 0}, 0, {0, 0, 0}, {1, 1, 1}},
      {"sunset_magenta", "Sunset Magenta", {1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 0, 0}, {1.10f, 0.90f, 0.95f}, {1, 1, 1}, 0.30f, 1.20f, {0.05f, -0.02f, 0.08f}, {0.09f, 0, 0.03f}, 0, {0, 0, 0}, {1, 1, 1}},
      {"matrix_green", "Matrix Green", {1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 0, 0}, {0.82f, 1.05f, 0.85f}, {1, 1, 1}, 0.50f, 0.80f, {-0.02f, 0.04f, 0.02f}, {-0.02f, 0.05f, 0}, 0, {0, 0, 0}, {1, 1, 1}},
      {"cyberpunk_neon", "Cyberpunk Neon", {1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 0, 0}, {1, 1, 1}, {1, 1, 1}, 0.50f, 1.40f, {-0.03f, 0.04f, 0.10f}, {0.10f, -0.04f, 0.07f}, 0, {0, 0, 0}, {1, 1, 1}},
      {"two_strip", "Two-Strip Cinema", {1.0f, 0.1f, 0, 0, 0.55f, 0.45f, 0, 0.55f, 0.45f}, {0, 0, 0}, {1.05f, 1, 1}, {1, 1, 1}, 0.30f, 1.0f, {0, 0, 0}, {0, 0, 0}, 0, {0, 0, 0}, {1, 1, 1}},
      {"natural_boost", "Natural Boost", {1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 0, 0}, {1, 1, 1}, {1.03f, 1.03f, 1.03f}, 0.25f, 1.12f, {0, 0, 0}, {0.015f, 0.01f, -0.01f}, 0, {0, 0, 0}, {1, 1, 1}},
  };
  return table;
}

inline const LutPreset *findLutPreset(const std::string &id) {
  for (const auto &p : lutPresets())
    if (id == p.id) return &p;
  return nullptr;
}

// The grade itself: input and output are 0..1 RGB.
inline void gradeColor(const LutPreset &p, float r, float g, float b, float out[3]) {
  float c[3] = {p.mix[0] * r + p.mix[1] * g + p.mix[2] * b, p.mix[3] * r + p.mix[4] * g + p.mix[5] * b,
                p.mix[6] * r + p.mix[7] * g + p.mix[8] * b};
  for (int k = 0; k < 3; ++k) {
    float v = clampf(c[k], 0.0f, 1.0f);
    v = p.lift[k] + v * (1.0f - p.lift[k]);
    v = clampf(v * p.gain[k], 0.0f, 1.0f);
    v = std::pow(v, 1.0f / p.gamma[k]);
    const float s = v * v * (3.0f - 2.0f * v);  // smoothstep S-curve
    c[k] = v + (s - v) * p.contrast;
  }
  float y = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
  const float ws = (1.0f - y) * (1.0f - y), wh = y * y;
  for (int k = 0; k < 3; ++k) c[k] += p.shadow[k] * ws + p.highlight[k] * wh;
  y = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
  for (int k = 0; k < 3; ++k) c[k] = y + (c[k] - y) * p.sat;
  if (p.mapAmount > 0.0f) {
    const float yg = clampf(y, 0.0f, 1.0f);
    for (int k = 0; k < 3; ++k)
      c[k] = c[k] * (1.0f - p.mapAmount) + (p.mapDark[k] + (p.mapLight[k] - p.mapDark[k]) * yg) * p.mapAmount;
  }
  for (int k = 0; k < 3; ++k) out[k] = clampf(c[k], 0.0f, 1.0f);
}

// 3D colour LUT in the Adobe/Resolve .cube format (red varies fastest).
struct Cube {
  int size = 0;
  std::vector<float> data;  // size^3 * 3

  bool load(const std::string &path) {
    size = 0;
    data.clear();
    FILE *f = std::fopen(path.c_str(), "r");
    if (!f) {
      std::fprintf(stderr, "LUT: cannot open %s\n", path.c_str());
      return false;
    }
    char buf[512];
    int n = 0;
    bool ok = true;
    while (std::fgets(buf, sizeof(buf), f)) {
      const std::string line = trim(buf);
      if (line.empty() || line[0] == '#') continue;
      if (line.rfind("LUT_3D_SIZE", 0) == 0) {
        n = std::atoi(line.c_str() + 11);
        if (n < 2 || n > 129) { ok = false; break; }
        data.reserve(static_cast<size_t>(n) * n * n * 3);
      } else if (line.rfind("LUT_1D_SIZE", 0) == 0) {
        std::fprintf(stderr, "LUT: 1D LUTs are not supported (%s)\n", path.c_str());
        ok = false;
        break;
      } else if (std::isdigit(static_cast<unsigned char>(line[0])) || line[0] == '-' || line[0] == '.') {
        float r, g, b;
        if (std::sscanf(line.c_str(), "%f %f %f", &r, &g, &b) != 3) { ok = false; break; }
        data.push_back(r); data.push_back(g); data.push_back(b);
      }  // TITLE, DOMAIN_MIN/MAX and other keywords are ignored (domain is assumed 0..1)
    }
    std::fclose(f);
    if (!ok || n == 0 || data.size() != static_cast<size_t>(n) * n * n * 3) {
      std::fprintf(stderr, "LUT: invalid or incomplete .cube file %s\n", path.c_str());
      data.clear();
      return false;
    }
    size = n;
    return true;
  }

  bool valid() const { return size >= 2; }

  // Bake a built-in preset into a size^3 LUT.
  void bake(const LutPreset &preset, int n = 33) {
    size = n;
    data.assign(static_cast<size_t>(n) * n * n * 3, 0.0f);
    for (int b = 0; b < n; ++b)
      for (int g = 0; g < n; ++g)
        for (int r = 0; r < n; ++r)
          gradeColor(preset, r / (n - 1.0f), g / (n - 1.0f), b / (n - 1.0f),
                     &data[((static_cast<size_t>(b) * n + g) * n + r) * 3]);
  }

  // Trilinear lookup; inputs and outputs are 0..255.
  void apply(int &r, int &g, int &b) const {
    const float scale = (size - 1) / 255.0f;
    const float fr = r * scale, fg = g * scale, fb = b * scale;
    const int r0 = std::min(static_cast<int>(fr), size - 2);
    const int g0 = std::min(static_cast<int>(fg), size - 2);
    const int b0 = std::min(static_cast<int>(fb), size - 2);
    const float tr = fr - r0, tg = fg - g0, tb = fb - b0;
    float out[3] = {0, 0, 0};
    for (int db = 0; db < 2; ++db)
      for (int dg = 0; dg < 2; ++dg)
        for (int dr = 0; dr < 2; ++dr) {
          const float w = (dr ? tr : 1 - tr) * (dg ? tg : 1 - tg) * (db ? tb : 1 - tb);
          const float *p = &data[((static_cast<size_t>(b0 + db) * size + (g0 + dg)) * size + (r0 + dr)) * 3];
          out[0] += w * p[0]; out[1] += w * p[1]; out[2] += w * p[2];
        }
    r = clamp8(static_cast<int>(out[0] * 255.0f + 0.5f));
    g = clamp8(static_cast<int>(out[1] * 255.0f + 0.5f));
    b = clamp8(static_cast<int>(out[2] * 255.0f + 0.5f));
  }
};

// Convert a finished YUYV frame to RGB24 (BT.601) for the GUI preview.
inline void makePreview(const uint8_t *yuyv, int w, int h, int pw, int ph, uint8_t *rgb) {
  for (int py = 0; py < ph; ++py) {
    const uint8_t *row = yuyv + static_cast<size_t>(py * h / ph) * w * 2;
    for (int px = 0; px < pw; ++px) {
      const int sx = px * w / pw;
      const int c = row[sx * 2] - 16, d = row[(sx / 2) * 4 + 1] - 128, e = row[(sx / 2) * 4 + 3] - 128;
      uint8_t *out = rgb + (static_cast<size_t>(py) * pw + px) * 3;
      out[0] = clamp8((298 * c + 409 * e + 128) >> 8);
      out[1] = clamp8((298 * c - 100 * d - 208 * e + 128) >> 8);
      out[2] = clamp8((298 * c + 516 * d + 128) >> 8);
    }
  }
}

class Pipeline {
 public:
  Pipeline() { buildNoise(); }

  void configure(const Settings &s) {
    const bool autoWasOn = s_.autoBrightness;
    s_ = s;
    if (!s_.autoBrightness || !autoWasOn) autoExp_ = 1.0f;
    look_ = findLook(s_.look);
    strength_ = clampf(s_.lookStrength, 0.0f, 1.0f);
    lutActive_ = false;
    lutQ_ = static_cast<int>(std::lround(clampf(s_.lutStrength, 0.0f, 1.0f) * 256.0f));
    if (lutQ_ > 0) {
      if (s_.lutPreset == "file") {
        lutActive_ = !s_.lutPath.empty() && cube_.load(s_.lutPath);
      } else if (const LutPreset *preset = findLutPreset(s_.lutPreset)) {
        cube_.bake(*preset);
        lutActive_ = true;
      }
    }
    lookActive_ = look_ && std::string(look_->id) != "none" && strength_ > 0.001f;
    plain_ = !lookActive_ && !lutActive_ && s_.brightness == 0.0f && s_.contrast == 1.0f && s_.saturation == 1.0f &&
             s_.temperature == 0.0f && !s_.autoBrightness && !s_.letterbox;
    for (int i = 0; i < 256; ++i) {
      const double n = i <= 16 ? 0.0 : (i >= 235 ? 1.0 : (i - 16) / 219.0);
      gammaLut_[i] = clamp8(static_cast<int>(16 + 219.0 * std::pow(n, s_.gamma) + 0.5));
    }
    buildLookTables();
    geomW_ = geomH_ = 0;
#ifdef _OPENMP
    threads_ = std::max(1, std::min(4, omp_get_max_threads()));
#endif
  }

  float autoExponent() const { return autoExp_; }

  // BGRX/RGBX (4 bytes per pixel) -> YUYV.
  void process(const uint8_t *src, int w, int h, bool bgr, uint8_t *dst) {
    if (plain_) {
      convertPlain(src, w, h, bgr, dst);
      return;
    }
    ensureGeometry(w, h);
    if (s_.autoBrightness) updateAutoExposure(src, w, h, bgr);
    buildUserLut();
    ++frame_;
    const uint32_t hash = frame_ * 2654435761u;
    const int ox = static_cast<int>(hash >> 16) & 0xFF, oy = static_cast<int>(hash >> 8) & 0xFF;
    const int bandTop = letterboxBand(w, h);
    const int pairs = w / 2;
    const int shiftPairs = static_cast<int>(std::lround(chromaShift_ / 2.0f));
    const int blurPairs = static_cast<int>(std::lround(chromaBlur_ / 2.0f));

#pragma omp parallel num_threads(threads_)
    {
      std::vector<uint8_t> rgb(static_cast<size_t>(w) * 3);
      std::vector<uint8_t> cb(pairs), cr(pairs);
#pragma omp for schedule(static)
      for (int y = 0; y < h; ++y) {
        uint8_t *out = dst + static_cast<size_t>(y) * w * 2;
        if (y < bandTop || y >= h - bandTop) {
          for (int x = 0; x < pairs; ++x) { out[4 * x] = 16; out[4 * x + 1] = 128; out[4 * x + 2] = 16; out[4 * x + 3] = 128; }
          continue;
        }
        shadeRow(src + static_cast<size_t>(y) * w * 4, w, y, bgr, ox, oy, rgb.data());
        toChroma(rgb.data(), pairs, cb.data(), cr.data());
        for (int p = 0; p < pairs; ++p) {
          const uint8_t *a = &rgb[p * 6];
          int cbv = cb[p], crv = cr[p];
          if (shiftPairs || blurPairs) {
            int sumB = 0, sumR = 0;
            for (int k = -blurPairs; k <= blurPairs; ++k) {
              const int idx = std::max(0, std::min(pairs - 1, p - shiftPairs + k));
              sumB += cb[idx]; sumR += cr[idx];
            }
            cbv = sumB / (2 * blurPairs + 1);
            crv = sumR / (2 * blurPairs + 1);
          }
          out[4 * p] = gammaLut_[luma(a[0], a[1], a[2])];
          out[4 * p + 1] = static_cast<uint8_t>(cbv);
          out[4 * p + 2] = gammaLut_[luma(a[3], a[4], a[5])];
          out[4 * p + 3] = static_cast<uint8_t>(crv);
        }
      }
    }
  }

 private:
  static int luma(int r, int g, int b) { return clamp8(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16); }

  // Original conversion, kept as-is so default settings do not change the picture.
  void convertPlain(const uint8_t *in, int w, int h, bool bgr, uint8_t *dst) const {
    for (int y = 0; y < h; ++y) {
      const uint8_t *row = in + static_cast<size_t>(y) * w * 4;
      uint8_t *out = dst + static_cast<size_t>(y) * w * 2;
      for (int x = 0; x < w; x += 2) {
        int r[2], g[2], b[2];
        for (int k = 0; k < 2; ++k) {
          const int sx = s_.mirror ? (w - 1 - (x + k)) : (x + k);
          const uint8_t *p = row + sx * 4;
          b[k] = bgr ? p[0] : p[2];
          g[k] = p[1];
          r[k] = bgr ? p[2] : p[0];
        }
        for (int k = 0; k < 2; ++k)
          out[k * 2] = gammaLut_[clamp8(((66 * r[k] + 129 * g[k] + 25 * b[k] + 128) >> 8) + 16)];
        const int rr = (r[0] + r[1]) / 2, gg = (g[0] + g[1]) / 2, bb = (b[0] + b[1]) / 2;
        out[1] = clamp8(((-38 * rr - 74 * gg + 112 * bb + 128) >> 8) + 128);
        out[3] = clamp8(((112 * rr - 94 * gg - 18 * bb + 128) >> 8) + 128);
        out += 4;
      }
    }
  }

  static void toChroma(const uint8_t *rgb, int pairs, uint8_t *cb, uint8_t *cr) {
    for (int p = 0; p < pairs; ++p) {
      const uint8_t *a = &rgb[p * 6];
      const int rr = (a[0] + a[3]) / 2, gg = (a[1] + a[4]) / 2, bb = (a[2] + a[5]) / 2;
      cb[p] = clamp8(((-38 * rr - 74 * gg + 112 * bb + 128) >> 8) + 128);
      cr[p] = clamp8(((112 * rr - 94 * gg - 18 * bb + 128) >> 8) + 128);
    }
  }

  int letterboxBand(int w, int h) const {
    if (!s_.letterbox) return 0;
    const int content = static_cast<int>(w / 2.39f) & ~1;
    return content >= h ? 0 : (h - content) / 2;
  }

  // Colour correction, look, spatial effects for one source row -> RGB bytes.
  void shadeRow(const uint8_t *row, int w, int y, bool bgr, int ox, int oy, uint8_t *rgb) const {
    const int rowMul = rowMul_[y];
    const int dy = dy2_[y];
    const int noiseRow = ((y + oy) & 0xFF) << 8;
    for (int x = 0; x < w; ++x) {
      const int sx = s_.mirror ? (w - 1 - x) : x;
      const uint8_t *p = row + sx * 4;
      int r = userLut_[0][bgr ? p[2] : p[0]];
      int g = userLut_[1][p[1]];
      int b = userLut_[2][bgr ? p[0] : p[2]];

      if (satQ_ != 256) {
        const int y0 = (77 * r + 150 * g + 29 * b) >> 8;
        r = clamp8(y0 + (((r - y0) * satQ_) >> 8));
        g = clamp8(y0 + (((g - y0) * satQ_) >> 8));
        b = clamp8(y0 + (((b - y0) * satQ_) >> 8));
      }
      if (lutActive_) {
        int lr = r, lg = g, lb = b;
        cube_.apply(lr, lg, lb);
        r += ((lr - r) * lutQ_) >> 8;
        g += ((lg - g) * lutQ_) >> 8;
        b += ((lb - b) * lutQ_) >> 8;
      }
      if (lookActive_) {
        if (monoQ_) {
          const int y0 = (77 * r + 150 * g + 29 * b) >> 8;
          r += (((y0 * monoMul_[0]) >> 8) - r) * monoQ_ >> 8;
          g += (((y0 * monoMul_[1]) >> 8) - g) * monoQ_ >> 8;
          b += (((y0 * monoMul_[2]) >> 8) - b) * monoQ_ >> 8;
        }
        r = lookLut_[0][clamp8(r)];
        g = lookLut_[1][clamp8(g)];
        b = lookLut_[2][clamp8(b)];
        const int pm = (vig_[dx2_[x] + dy] * rowMul) >> 8;
        const int *cm = colMask_[x % 3];
        r = clamp8((r * ((pm * cm[0]) >> 8)) >> 8);
        g = clamp8((g * ((pm * cm[1]) >> 8)) >> 8);
        b = clamp8((b * ((pm * cm[2]) >> 8)) >> 8);
        if (grainQ_) {
          const int n = (noise_[(x + ox + noiseRow) & 0xFFFF] * grainQ_) >> 8;
          r = clamp8(r + n); g = clamp8(g + n); b = clamp8(b + n);
        }
      }
      rgb[x * 3] = static_cast<uint8_t>(r);
      rgb[x * 3 + 1] = static_cast<uint8_t>(g);
      rgb[x * 3 + 2] = static_cast<uint8_t>(b);
    }
  }

  // Per-channel curve for brightness/contrast/temperature/auto-brightness.
  void buildUserLut() {
    const float tempGain[3] = {1.0f + 0.15f * s_.temperature, 1.0f + 0.02f * s_.temperature,
                               1.0f - 0.15f * s_.temperature};
    for (int c = 0; c < 3; ++c)
      for (int i = 0; i < 256; ++i) {
        float v = i / 255.0f;
        if (autoExp_ != 1.0f) v = std::pow(v, autoExp_);
        v = (v - 0.5f) * s_.contrast + 0.5f + s_.brightness;
        v *= tempGain[c];
        userLut_[c][i] = clamp8(static_cast<int>(clampf(v, 0.0f, 1.0f) * 255.0f + 0.5f));
      }
    const float lookSat = lookActive_ ? lookSat_ : 1.0f;
    satQ_ = static_cast<int>(std::lround(clampf(s_.saturation * lookSat, 0.0f, 3.0f) * 256.0f));
  }

  void buildLookTables() {
    const float s = strength_;
    const LookParams none{"none", "", 1, 1, 0, {0, 0, 0}, {0, 0, 0}, 0, 0, 0, 0, 0, 0, 0};
    const LookParams &l = look_ ? *look_ : none;
    strengthQ_ = static_cast<int>(std::lround(s * 256.0f));
    monoQ_ = l.mono ? strengthQ_ : 0;
    if (l.mono == 2) { monoMul_[0] = 40; monoMul_[1] = 232; monoMul_[2] = 70; }
    else { monoMul_[0] = monoMul_[1] = monoMul_[2] = 256; }
    // Saturation mixes channels, so it cannot live in the per-channel curves: buildUserLut()
    // multiplies it into the user saturation.
    lookSat_ = 1.0f + (l.sat - 1.0f) * s;
    const float contrast = 1.0f + (l.contrast - 1.0f) * s;
    const float lift = l.lift * s;
    for (int c = 0; c < 3; ++c)
      for (int i = 0; i < 256; ++i) {
        float v = i / 255.0f;
        v = lift + v * (1.0f - lift);
        v = (v - 0.5f) * contrast + 0.5f;
        v += l.shadow[c] * s * (1.0f - v) * (1.0f - v);
        v += l.highlight[c] * s * v * v;
        lookLut_[c][i] = clamp8(static_cast<int>(clampf(v, 0.0f, 1.0f) * 255.0f + 0.5f));
      }
    vignette_ = l.vignette * s;
    scanlines_ = l.scanlines * s;
    mask_ = l.mask * s;
    grainQ_ = static_cast<int>(std::lround(l.grain * s * 256.0f * 1.5f));
    chromaShift_ = l.chromaShift * s;
    chromaBlur_ = l.chromaBlur * s;
  }

  void ensureGeometry(int w, int h) {
    if (w == geomW_ && h == geomH_) return;
    geomW_ = w; geomH_ = h;
    dx2_.assign(w, 0); dy2_.assign(h, 0); rowMul_.assign(h, 256);
    const float cx = (w - 1) / 2.0f, cy = (h - 1) / 2.0f;
    for (int x = 0; x < w; ++x) { const float d = (x - cx) / cx; dx2_[x] = static_cast<int>(d * d * 512.0f); }
    for (int y = 0; y < h; ++y) { const float d = (y - cy) / cy; dy2_[y] = static_cast<int>(d * d * 512.0f); }
    vig_.assign(1100, 256);
    for (int i = 0; i < 1100; ++i) {
      const float t = std::min(1.0f, i / 1024.0f);
      vig_[i] = static_cast<int>(256.0f * (1.0f - vignette_ * std::pow(t, 1.5f)));
    }
    const float period = std::max(2.0f, h / 240.0f);
    const float comp = 1.0f + 0.5f * scanlines_;  // scanlines darken the picture; win some back
    for (int y = 0; y < h; ++y) {
      const float m = 1.0f - scanlines_ * 0.5f * (1.0f + std::cos(6.2831853f * y / period));
      rowMul_[y] = static_cast<int>(256.0f * m * comp);
    }
    for (int k = 0; k < 3; ++k)
      for (int c = 0; c < 3; ++c) colMask_[k][c] = static_cast<int>(256.0f * (k == c ? 1.0f : 1.0f - mask_));
  }

  // Mean scene brightness -> exponent that moves it toward the target, eased frame by frame.
  void updateAutoExposure(const uint8_t *src, int w, int h, bool bgr) {
    const int stepX = std::max(1, w / 64), stepY = std::max(1, h / 48);
    long sum = 0, n = 0;
    for (int y = 0; y < h; y += stepY)
      for (int x = 0; x < w; x += stepX) {
        const uint8_t *p = src + (static_cast<size_t>(y) * w + x) * 4;
        const int r = bgr ? p[2] : p[0], b = bgr ? p[0] : p[2];
        sum += (77 * r + 150 * p[1] + 29 * b) >> 8;
        ++n;
      }
    const float mean = clampf(static_cast<float>(sum) / n / 255.0f, 0.02f, 0.98f);
    const float target = clampf(s_.autoTarget, 0.05f, 0.95f);
    const float desired = clampf(std::log(target) / std::log(mean), 0.45f, 2.2f);
    if (std::fabs(desired - autoExp_) > 0.03f) autoExp_ += (desired - autoExp_) * 0.05f;
  }

  void buildNoise() {
    uint32_t state = 0x9E3779B9u;
    auto next = [&state]() { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return state; };
    for (auto &v : noise_) {
      int sum = 0;
      for (int i = 0; i < 4; ++i) sum += static_cast<int>(next() & 0xFF) - 128;
      v = static_cast<int8_t>(std::max(-127, std::min(127, sum / 8)));  // ~bell-shaped, std ~ 18
    }
  }

  Settings s_;
  const LookParams *look_ = nullptr;
  Cube cube_;
  bool lutActive_ = false, lookActive_ = false, plain_ = true;
  float strength_ = 1.0f, autoExp_ = 1.0f, lookSat_ = 1.0f;
  float vignette_ = 0, scanlines_ = 0, mask_ = 0, chromaShift_ = 0, chromaBlur_ = 0;
  int strengthQ_ = 256, lutQ_ = 256, monoQ_ = 0, grainQ_ = 0, satQ_ = 256;
  int monoMul_[3] = {256, 256, 256};
  int threads_ = 1;
  uint32_t frame_ = 0;
  uint8_t gammaLut_[256];
  uint8_t userLut_[3][256];
  uint8_t lookLut_[3][256];
  int8_t noise_[65536];
  int geomW_ = 0, geomH_ = 0;
  std::vector<int> dx2_, dy2_, rowMul_, vig_;
  int colMask_[3][3] = {};
};

}  // namespace fx
