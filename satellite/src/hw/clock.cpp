// 4-digit 7-segment clock of the Station Midi (two aw21036/aw21012 chips on i2c 3-0037 / 3-0021,
// one sysfs LED per segment: /sys/class/leds/cucumber:white:clock-<seg><digit>).
//
// Segment mapping (derived from the live display driven by maind: at 21:17 local time the lit
// set was a1 b1 d1 e1 g1 | b2 c2 | b3 c3 | a4 b4 c4 plus p3 q3, i.e. "21:17"):
//   clock-<s><n>: n = digit position 1..4 left to right, s = standard 7-segment letter
//
//          aaa
//         f   b
//          ggg          digits: [1][2] : [3][4]
//         e   c
//          ddd
//
//   p3 + q3     colon (upper/lower dot) - the only extra LEDs maind lights for the time.
//   deg         degree sign; h1 h2, l1 l2, m1 m2, p1 p2 p4 p5, q1 q2 q4 q5: auxiliary dots/icons
//               whose position is not known (never lit by maind for the time); kept off here.
//   cucumber:sync:clock-0/1 are the chips' sync pseudo-LEDs (not segments).
//
// Brightness: like maind, segments are switched 0/255 and dimming uses the chips' global current
// attribute (/sys/bus/i2c/devices/{3-0037,3-0021}/current, 0..255; maind wrote 5 for its stored
// brightness 0.0196 = 5/255). The brightness_scale tables are left untouched.
//
// Time is local time via localtime_r, i.e. it honours the process TZ (the device has no
// /etc/localtime or zoneinfo, so the daemon must setenv("TZ", "<POSIX TZ>") + tzset(), e.g. from
// HA's GetTimeResponse.timezone; otherwise UTC is shown).
#include <fcntl.h>
#include <unistd.h>

#include <cmath>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "core/log.h"
#include "hw/ambient.h"
#include "hw/hw.h"
#include "hw/sysfs.h"

namespace cs {

namespace {

constexpr const char *kLedDir = "/sys/class/leds/cucumber:white:clock-";
constexpr int kDigits = 4;
constexpr int kSegs = 7;  // a..g
constexpr int64_t kTickMs = 250;
constexpr int64_t kFlashMs = 1500;
constexpr int64_t kCodePageMs = 2000;
constexpr int kMinCurrent = 2;  // lowest global current that still lights the segments

// Bit i = segment 'a'+i.
constexpr uint8_t kFont[10] = {
    0x3f,  // 0 abcdef
    0x06,  // 1 bc
    0x5b,  // 2 abdeg
    0x4f,  // 3 abcdg
    0x66,  // 4 bcfg
    0x6d,  // 5 acdfg
    0x7d,  // 6 acdefg
    0x07,  // 7 abc
    0x7f,  // 8 abcdefg
    0x6f,  // 9 abcdfg
};
constexpr uint8_t kMinus = 0x40;  // g

// Auxiliary LEDs we own but keep dark.
const char *const kAux[] = {"deg", "h1", "h2", "l1", "l2", "m1", "m2", "p1", "p2", "p4", "p5", "q1", "q2", "q4", "q5"};

}  // namespace

struct Clock::Impl {
  explicit Impl(Loop &l) : loop(l) {}
  Loop &loop;

  int seg_fd[kDigits][kSegs];
  int colon_fd[2] = {-1, -1};
  std::vector<int> current_fds;
  // last written state (-1 = unknown)
  int seg_last[kDigits][kSegs];
  int colon_last = -1;
  int current_last = -1;

  bool enabled = true;
  float user_bri = 1.f;
  float lux = -1.f;  // <0: unknown, no auto-dimming
  AmbientDimmer dimmer;
  int64_t timer_end = 0;  // monotonic ms; 0 = no timer
  int64_t flash_until = 0;
  int flash_value = 0;
  std::string code;  // pairing code digits shown instead of anything else (empty = none)
  int64_t code_since = 0;
  uint64_t tick = 0;
  unsigned refreshes = 0;

  ~Impl() {
    if (tick) loop.cancel_timer(tick);
    for (auto &d : seg_fd)
      for (int fd : d)
        if (fd >= 0) close(fd);
    for (int fd : colon_fd)
      if (fd >= 0) close(fd);
    for (int fd : current_fds) close(fd);
  }

  static int open_led(const std::string &name) {
    return ::open((std::string(kLedDir) + name + "/brightness").c_str(), O_WRONLY | O_CLOEXEC);
  }

  void set_seg(int d, int s, bool on) {
    int v = on ? 255 : 0;
    if (seg_last[d][s] == v || seg_fd[d][s] < 0) return;
    if (hwutil::write_int_fd(seg_fd[d][s], v)) seg_last[d][s] = v;
  }

  void set_colon(bool on) {
    int v = on ? 255 : 0;
    if (colon_last == v) return;
    for (int fd : colon_fd)
      if (fd >= 0) hwutil::write_int_fd(fd, v);
    colon_last = v;
  }

  float ambient() const { return dimmer.factor(); }  // 0.02..1, 1 while lux is unknown

  void apply_current() {
    int cur = int(std::lround(255.f * user_bri * ambient()));
    cur = user_bri <= 0 ? 0 : std::max(kMinCurrent, std::min(255, cur));
    if (cur == current_last) return;
    for (int fd : current_fds) hwutil::write_int_fd(fd, cur);
    current_last = cur;
    LOGD("clock: current=%d (user %.2f, lux %.1f)", cur, user_bri, lux);
  }

  void show(const uint8_t glyph[kDigits], bool colon, bool force = false) {
    const bool on = enabled || force;
    for (int d = 0; d < kDigits; d++)
      for (int s = 0; s < kSegs; s++) set_seg(d, s, on && (glyph[d] >> s & 1));
    set_colon(on && colon);
  }

  // Up to 4 digits shown as is; longer codes page through 3-digit groups ("123-", "-456"),
  // matching the 3-3 grouping the Sendspin spec asks for.
  void show_code(int64_t now) {
    uint8_t g[kDigits] = {0, 0, 0, 0};
    auto glyph = [](char c) -> uint8_t { return c >= '0' && c <= '9' ? kFont[c - '0'] : kMinus; };
    if (code.size() <= size_t(kDigits)) {
      for (size_t i = 0; i < code.size(); i++) g[kDigits - code.size() + i] = glyph(code[i]);
    } else {
      size_t groups = (code.size() + 2) / 3;
      size_t page = size_t((now - code_since) / kCodePageMs) % groups;
      std::string grp = code.substr(page * 3, 3);
      if (page == 0) {
        for (size_t i = 0; i < grp.size(); i++) g[i] = glyph(grp[i]);
        g[3] = kMinus;
      } else {
        g[0] = kMinus;
        for (size_t i = 0; i < grp.size(); i++) g[1 + i] = glyph(grp[i]);
      }
    }
    show(g, false, true);
  }

  static void two(uint8_t *g, int v, bool blank_lead) {
    g[0] = (blank_lead && v < 10) ? 0 : kFont[(v / 10) % 10];
    g[1] = kFont[v % 10];
  }

  void refresh() {
    int64_t now = Loop::now_ms();
    // Every ~5 s rewrite all segments even if unchanged (sysfs state may be changed by others).
    if (++refreshes % 20 == 0) {
      for (auto &d : seg_last)
        for (int &v : d) v = -1;
      colon_last = -1;
    }
    if (!code.empty()) return show_code(now);
    uint8_t g[kDigits] = {0, 0, 0, 0};
    bool colon = false;
    if (now < flash_until) {
      // Right-aligned number, '-' for negatives.
      int v = flash_value, n = std::abs(v);
      int pos = kDigits - 1;
      do {
        g[pos--] = kFont[n % 10];
        n /= 10;
      } while (n && pos >= 0);
      if (v < 0 && pos >= 0) g[pos] = kMinus;
    } else if (timer_end) {
      int64_t left = std::max<int64_t>(0, (timer_end - now + 999) / 1000);
      if (left >= 3600) {  // h:mm (hours >= 10 show as hh:mm)
        two(g, int(std::min<int64_t>(left / 3600, 99)), true);
        two(g + 2, int(left / 60 % 60), false);
      } else {
        two(g, int(left / 60), true);
        two(g + 2, int(left % 60), false);
      }
      colon = left > 0 || (now / 500) % 2 == 0;  // blink at 00:00
      if (left == 0 && (now / 500) % 2) memset(g, 0, sizeof g);
    } else {
      time_t t = time(nullptr);
      struct tm tm;
      localtime_r(&t, &tm);
      two(g, tm.tm_hour, false);
      two(g + 2, tm.tm_min, false);
      timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      colon = ts.tv_nsec < 500000000;  // on for the first half of every second
    }
    show(g, colon);
  }
};

Clock::Clock(Loop &loop) : impl_(new Impl(loop)) {
  for (auto &d : impl_->seg_fd)
    for (int &fd : d) fd = -1;
  for (auto &d : impl_->seg_last)
    for (int &v : d) v = -1;
}

Clock::~Clock() = default;

bool Clock::open() {
  auto &d = *impl_;
  int found = 0;
  for (int dig = 0; dig < kDigits; dig++)
    for (int s = 0; s < kSegs; s++) {
      char name[8];
      snprintf(name, sizeof name, "%c%d", 'a' + s, dig + 1);
      d.seg_fd[dig][s] = Impl::open_led(name);
      if (d.seg_fd[dig][s] >= 0) found++;
      else LOGW("clock: missing segment %s", name);
    }
  if (!found) {
    LOGE("clock: no clock segments found");
    return false;
  }
  d.colon_fd[0] = Impl::open_led("p3");
  d.colon_fd[1] = Impl::open_led("q3");
  for (const char *aux : kAux) {
    int fd = Impl::open_led(aux);
    if (fd >= 0) {
      hwutil::write_int_fd(fd, 0);
      close(fd);
    }
  }
  // Global current of both clock driver chips, found through the LED device links.
  for (const char *seg : {"a1", "a4"}) {
    std::string dev = std::string(kLedDir) + seg + "/device/current";
    char real[256];
    if (!realpath(dev.c_str(), real)) continue;
    bool dup = false;
    for (int fd : d.current_fds) {
      char p[64], r2[256];
      snprintf(p, sizeof p, "/proc/self/fd/%d", fd);
      ssize_t n = readlink(p, r2, sizeof r2 - 1);
      if (n > 0 && std::string(r2, size_t(n)) == real) dup = true;
    }
    if (dup) continue;
    int fd = ::open(real, O_WRONLY | O_CLOEXEC);
    if (fd >= 0) d.current_fds.push_back(fd);
  }
  LOGI("clock: %d segments, %zu driver chips", found, d.current_fds.size());
  d.apply_current();
  d.refresh();
  d.tick = d.loop.add_timer(kTickMs, [&d] { d.refresh(); }, kTickMs);
  return true;
}

void Clock::set_enabled(bool on) {
  impl_->enabled = on;
  impl_->refresh();
}

void Clock::set_brightness(float b) {
  impl_->user_bri = std::max(0.f, std::min(1.f, b));
  impl_->apply_current();
}

void Clock::set_ambient_lux(float lux) {
  impl_->lux = std::max(0.f, lux);
  impl_->dimmer.update(impl_->lux);
  impl_->apply_current();
}

void Clock::set_timer(uint32_t seconds_left) {
  impl_->timer_end = seconds_left ? Loop::now_ms() + int64_t(seconds_left) * 1000 : 0;
  impl_->refresh();
}

void Clock::flash_number(int value) {
  impl_->flash_value = std::max(-999, std::min(9999, value));
  impl_->flash_until = Loop::now_ms() + kFlashMs;
  impl_->refresh();
}

void Clock::show_code(const std::string &digits) {
  impl_->code = digits;
  impl_->code_since = Loop::now_ms();
  impl_->refresh();
}

void Clock::clear_code() {
  impl_->code.clear();
  impl_->refresh();
}

}  // namespace cs
