// LED ring (24 RGB) + logo (6 RGB) of the Station Midi.
//
// How the hardware is driven: the ring LEDs (two aw21036 chips, i2c 1-0034 "ringA" / 1-0035
// "ringB") and the logo LEDs (aw21018, 1-0020) sit behind the kernel "ledfb-ring"/"ledfb-logo"
// LED triggers, which expose them as framebuffers:
//   /dev/fb0 "ring": 24x1, 24 bpp, rgba 8/16,8/8,8/0 -> bytes per pixel are B,G,R; pixel i = ring{i}
//   /dev/fb1 "logo":  6x1, 24 bpp, same layout;       pixel i = logo{i}
// A 72-byte pwrite() fills the ring framebuffer, but the ledfb driver pushes the framebuffer to
// the LEDs at the *start* of each write/pan, i.e. a write is only shown on the next write or
// FBIOPAN_DISPLAY (measured: sysfs brightness lags one write behind; a pan latches immediately).
// So every frame is pwrite() + FBIOPAN_DISPLAY. Cost ~2.5 ms per ring update (i2c), far cheaper
// than 72 sysfs writes. If the framebuffers are missing or the
// triggers are not ledfb-*, we fall back to the per-channel sysfs brightness files kept open.
//
// Ring index order: Yandex patterns (ledpatterns/ring/*.led, 24 RRGGBB columns per frame) anchor
// their animations at ring0 and grow symmetrically to ring1/ring23 (mute.led, alice_thinking.led),
// so ring0 is the "home" position; all animations here are relative to ring0 and go towards
// increasing indices.
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cmath>
#include <cstring>
#include <vector>

#include "core/log.h"
#include "hw/ambient.h"
#include "hw/hw.h"
#include "hw/sysfs.h"

namespace cs {

namespace {

constexpr int kRing = 24;
constexpr int kLogo = 6;
constexpr int kFrameMs = 33;          // ~30 fps while animating
constexpr float kMaxScale = 0.8f;     // global cap: never drive LEDs above 80 % PWM
constexpr int64_t kVolumeShowMs = 1500;
constexpr int64_t kWarnEveryMs = 10000;  // rate limit for repeated per-frame write warnings
constexpr const char *kLedDir = "/sys/class/leds/";

struct Col {
  float r = 0, g = 0, b = 0;
};

Col operator*(Col c, float k) { return {c.r * k, c.g * k, c.b * k}; }
Col rgb(int r, int g, int b) { return {r / 255.f, g / 255.f, b / 255.f}; }

Col hsv(float h, float s, float v) {
  h = std::fmod(h, 1.f);
  if (h < 0) h += 1.f;
  float i = std::floor(h * 6), f = h * 6 - i, p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
  switch (int(i) % 6) {
    case 0: return {v, t, p};
    case 1: return {q, v, p};
    case 2: return {p, v, t};
    case 3: return {p, q, v};
    case 4: return {t, p, v};
    default: return {v, p, q};
  }
}

// Smooth 0..1 oscillation with period ms, starting at minimum.
float wave(int64_t t, int64_t period) { return 0.5f - 0.5f * std::cos(float(t % period) / float(period) * 6.2831853f); }

uint8_t to8(float v, float dim) {
  v = std::max(0.f, std::min(1.f, v)) * kMaxScale * dim;
  // Gamma 2.0 so that perceived brightness is roughly linear in v.
  return uint8_t(std::lround(v * v * 255.f));
}

// Palette
const Col kCyan = rgb(0, 190, 255);
const Col kBlue = rgb(55, 15, 255);  // Yandex "alice" violet-blue (0x370fff)
const Col kWhite = rgb(255, 255, 255);
const Col kRed = rgb(255, 0, 0);
const Col kOrange = rgb(255, 90, 0);

const Col kWake = rgb(120, 230, 255);
}  // namespace

struct Leds::Impl {
  explicit Impl(Loop &l) : loop(l) {}
  Loop &loop;

  // output
  int fb_ring = -1, fb_logo = -1;
  fb_var_screeninfo var_ring{}, var_logo{};
  std::vector<int> sysfs_fds;  // fallback: [ring r,g,b x24][logo r,g,b x6]
  uint8_t last[(kRing + kLogo) * 3];
  bool have_last = false;

  // state
  UiState state = UiState::IDLE;
  int64_t state_t0 = 0;
  bool user_on = false;
  Col user_col = rgb(255, 255, 255);
  float user_bri = 1.f;
  std::string effect;
  float volume = 0;
  int64_t volume_until = 0;
  uint64_t timer = 0;
  AmbientDimmer dimmer;
  int64_t warn_t[2] = {0, 0};  // last fb0/fb1 write warning
  unsigned warn_suppressed[2] = {0, 0};

  ~Impl() {
    if (timer) loop.cancel_timer(timer);
    clear();  // do not leave the last frame lit after the daemon exits
    if (fb_ring >= 0) close(fb_ring);
    if (fb_logo >= 0) close(fb_logo);
    for (int fd : sysfs_fds)
      if (fd >= 0) close(fd);
  }

  static bool trigger_is(const std::string &led, const char *trig) {
    std::string t;
    if (!hwutil::read_file(std::string(kLedDir) + led + "/trigger", t)) return false;
    return t.find(std::string("[") + trig + "]") != std::string::npos;
  }

  bool open_fb() {
    if (!trigger_is("cucumber:red:ring0", "ledfb-ring") || !trigger_is("cucumber:red:logo0", "ledfb-logo")) {
      LOGW("leds: ledfb triggers not active, using sysfs");
      return false;
    }
    fb_ring = ::open("/dev/fb0", O_RDWR | O_CLOEXEC);
    fb_logo = ::open("/dev/fb1", O_RDWR | O_CLOEXEC);
    std::string n0, n1;
    hwutil::read_file("/sys/class/graphics/fb0/name", n0);
    hwutil::read_file("/sys/class/graphics/fb1/name", n1);
    if (fb_ring < 0 || fb_logo < 0 || hwutil::trim(n0) != "ring" || hwutil::trim(n1) != "logo") {
      LOGW("leds: framebuffers unavailable (fb0=%s fb1=%s)", hwutil::trim(n0).c_str(), hwutil::trim(n1).c_str());
      if (fb_ring >= 0) close(fb_ring);
      if (fb_logo >= 0) close(fb_logo);
      fb_ring = fb_logo = -1;
      return false;
    }
    ioctl(fb_ring, FBIOGET_VSCREENINFO, &var_ring);
    ioctl(fb_logo, FBIOGET_VSCREENINFO, &var_logo);
    return true;
  }

  bool open_sysfs() {
    static const char *cols[] = {"red", "green", "blue"};
    for (int i = 0; i < kRing + kLogo; i++)
      for (const char *c : cols) {
        char path[128];
        if (i < kRing)
          snprintf(path, sizeof path, "%scucumber:%s:ring%d/brightness", kLedDir, c, i);
        else
          snprintf(path, sizeof path, "%scucumber:%s:logo%d/brightness", kLedDir, c, i - kRing);
        int fd = ::open(path, O_WRONLY | O_CLOEXEC);
        if (fd < 0) {
          LOGE("leds: cannot open %s", path);
          return false;
        }
        sysfs_fds.push_back(fd);
      }
    return true;
  }

  // Ring/logo brightness factor from ambient light: 0.3 in the dark .. 1.
  float dim() const { return 0.3f + 0.7f * (dimmer.factor() - 0.02f) / 0.98f; }

  void clear() {
    Col off[kRing] = {};
    have_last = false;
    output(off, off);
  }

  void warn_write(int which, const char *what) {
    int64_t now = Loop::now_ms();
    if (warn_t[which] && now - warn_t[which] < kWarnEveryMs) {
      warn_suppressed[which]++;
      return;
    }
    LOGW("leds: %s write: %s (%u similar suppressed)", what, strerror(errno), warn_suppressed[which]);
    warn_t[which] = now;
    warn_suppressed[which] = 0;
  }

  void output(const Col *ring, const Col *logo) {
    uint8_t px[(kRing + kLogo) * 3];
    const float d = dim();
    for (int i = 0; i < kRing; i++) {
      px[i * 3 + 0] = to8(ring[i].r, d);
      px[i * 3 + 1] = to8(ring[i].g, d);
      px[i * 3 + 2] = to8(ring[i].b, d);
    }
    for (int i = 0; i < kLogo; i++) {
      px[(kRing + i) * 3 + 0] = to8(logo[i].r, d);
      px[(kRing + i) * 3 + 1] = to8(logo[i].g, d);
      px[(kRing + i) * 3 + 2] = to8(logo[i].b, d);
    }
    if (fb_ring >= 0) {
      bool ring_changed = !have_last || memcmp(px, last, kRing * 3) != 0;
      bool logo_changed = !have_last || memcmp(px + kRing * 3, last + kRing * 3, kLogo * 3) != 0;
      uint8_t bgr[kRing * 3];
      if (ring_changed) {
        for (int i = 0; i < kRing; i++) {
          bgr[i * 3] = px[i * 3 + 2];
          bgr[i * 3 + 1] = px[i * 3 + 1];
          bgr[i * 3 + 2] = px[i * 3];
        }
        if (pwrite(fb_ring, bgr, kRing * 3, 0) != kRing * 3) warn_write(0, "fb0");
        ioctl(fb_ring, FBIOPAN_DISPLAY, &var_ring);  // latch
      }
      if (logo_changed) {
        for (int i = 0; i < kLogo; i++) {
          bgr[i * 3] = px[(kRing + i) * 3 + 2];
          bgr[i * 3 + 1] = px[(kRing + i) * 3 + 1];
          bgr[i * 3 + 2] = px[(kRing + i) * 3];
        }
        if (pwrite(fb_logo, bgr, kLogo * 3, 0) != kLogo * 3) warn_write(1, "fb1");
        ioctl(fb_logo, FBIOPAN_DISPLAY, &var_logo);
      }
    } else {
      for (size_t k = 0; k < sysfs_fds.size(); k++)
        if (!have_last || px[k] != last[k]) hwutil::write_int_fd(sysfs_fds[k], px[k]);
    }
    memcpy(last, px, sizeof px);
    have_last = true;
  }

  // Renders the current frame; returns true if the content is animated (needs further frames).
  bool render(int64_t now, Col *ring) {
    int64_t t = now - state_t0;
    for (int i = 0; i < kRing; i++) ring[i] = {};
    bool animated = true;
    switch (state) {
      case UiState::IDLE:
        animated = render_user(now, ring);
        break;
      case UiState::WAKE: {
        // Sweep from ring0 outward in both directions in 250 ms, then hold bright ice-blue.
        float reach = std::min(1.f, float(t) / 250.f) * (kRing / 2 + 1);
        for (int i = 0; i <= kRing / 2; i++) {
          float k = std::max(0.f, std::min(1.f, reach - i));
          ring[i] = kWake * k;
          ring[(kRing - i) % kRing] = ring[i];
        }
        animated = t < 300;
        break;
      }
      case UiState::LISTENING: {
        float k = 0.55f + 0.45f * wave(t, 2000);
        for (int i = 0; i < kRing; i++) ring[i] = kCyan * k;
        break;
      }
      case UiState::THINKING: {
        // Two comets rotating (one turn per 1.2 s).
        float head = float(t % 1200) / 1200.f * kRing;
        for (int i = 0; i < kRing; i++) {
          for (int c = 0; c < 2; c++) {
            float d = std::fmod(head + c * (kRing / 2) - i + kRing * 2, float(kRing));  // distance behind head
            if (d < 7) {
              Col v = kBlue * (1.f - d / 7.f);
              ring[i] = {std::max(ring[i].r, v.r), std::max(ring[i].g, v.g), std::max(ring[i].b, v.b)};
            }
          }
          ring[i] = {std::max(ring[i].r, kBlue.r * 0.12f), std::max(ring[i].g, kBlue.g * 0.12f),
                     std::max(ring[i].b, kBlue.b * 0.12f)};
        }
        break;
      }
      case UiState::SPEAKING: {
        float k = 0.45f + 0.55f * wave(t, 700);
        for (int i = 0; i < kRing; i++) ring[i] = kBlue * k;
        break;
      }
      case UiState::ERROR: {
        // 3 blinks (200 ms on / 200 ms off), then back to IDLE.
        if (t >= 1200) {
          set_state(UiState::IDLE, now);
          return render(now, ring);
        }
        float k = (t % 400) < 200 ? 0.9f : 0.f;
        for (int i = 0; i < kRing; i++) ring[i] = kRed * k;
        break;
      }
      case UiState::MUTED:
        for (int i = 0; i < kRing; i++) ring[i] = kRed * 0.25f;
        animated = false;
        break;
      case UiState::TIMER_RING: {
        float k = 0.15f + 0.85f * wave(t, 1000);
        for (int i = 0; i < kRing; i++) ring[i] = kOrange * k;
        break;
      }
    }
    if (now < volume_until) {
      // Volume arc overlay: v*24 LEDs from ring0, partial last LED, remaining ring dark.
      float n = volume * kRing;
      for (int i = 0; i < kRing; i++) {
        float k = std::max(0.f, std::min(1.f, n - i));
        ring[i] = kWhite * (0.15f + 0.65f * k) * (k > 0 ? 1.f : 0.f);
      }
      animated = true;  // need a frame when the overlay expires
    }
    return animated;
  }

  bool render_user(int64_t now, Col *ring) {
    if (!user_on || user_bri <= 0) return false;
    Col base = user_col * user_bri;
    if (effect == "Rainbow") {
      float off = float(now % 6000) / 6000.f;
      for (int i = 0; i < kRing; i++) ring[i] = hsv(off + float(i) / kRing, 1.f, user_bri);
      return true;
    }
    if (effect == "Breathe") {
      float k = 0.1f + 0.9f * wave(now, 4000);
      for (int i = 0; i < kRing; i++) ring[i] = base * k;
      return true;
    }
    for (int i = 0; i < kRing; i++) ring[i] = base;
    return false;
  }

  void set_state(UiState s, int64_t now) {
    state = s;
    state_t0 = now;
  }

  void frame() {
    int64_t now = Loop::now_ms();
    Col ring[kRing];
    bool animated = render(now, ring);
    // Logo mirrors the ring's average colour.
    Col avg;
    for (auto &c : ring) avg = {avg.r + c.r / kRing, avg.g + c.g / kRing, avg.b + c.b / kRing};
    Col logo[kLogo];
    for (auto &c : logo) c = avg;
    output(ring, logo);
    if (animated && !timer) {
      timer = loop.add_timer(kFrameMs, [this] { frame(); }, kFrameMs);
    } else if (!animated && timer) {
      loop.cancel_timer(timer);
      timer = 0;
    }
  }
};

Leds::Leds(Loop &loop) : impl_(new Impl(loop)) {}
Leds::~Leds() = default;

bool Leds::open() {
  auto &d = *impl_;
  if (d.open_fb()) {
    LOGI("leds: driving ring/logo via /dev/fb0,/dev/fb1");
  } else if (d.open_sysfs()) {
    LOGI("leds: driving ring/logo via sysfs brightness");
  } else {
    return false;
  }
  d.state_t0 = Loop::now_ms();
  d.frame();
  return true;
}

void Leds::set_ui_state(UiState s) {
  auto &d = *impl_;
  if (s == d.state) return;
  d.set_state(s, Loop::now_ms());
  d.frame();
}

void Leds::set_user_light(bool on, Rgb color, float brightness, const std::string &effect) {
  auto &d = *impl_;
  d.user_on = on;
  d.user_col = rgb(color.r, color.g, color.b);
  d.user_bri = std::max(0.f, std::min(1.f, brightness));
  d.effect = effect;
  d.frame();
}

void Leds::set_ambient_lux(float lux) {
  auto &d = *impl_;
  if (d.dimmer.update(lux)) d.frame();
}

void Leds::show_volume(float v) {
  auto &d = *impl_;
  d.volume = std::max(0.f, std::min(1.f, v));
  d.volume_until = Loop::now_ms() + kVolumeShowMs;
  d.frame();
}

}  // namespace cs
