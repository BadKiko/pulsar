#include "hw/bluetooth.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/log.h"

namespace cs {

namespace {

constexpr const char *kOn =
    "bluetoothctl power on; bluetoothctl discoverable-timeout 180; bluetoothctl pairable on; "
    "bluetoothctl discoverable on";
constexpr const char *kOff =
    "bluetoothctl discoverable off; bluetoothctl pairable off; "
    "for m in $(bluetoothctl devices Connected | cut -d' ' -f2); do bluetoothctl disconnect $m; done";
// bluetoothd resets the class on its own now and then (main.conf: loudspeaker), so it is set again
// with every window; paired devices are trusted so that they reconnect without a new pairing.
constexpr const char *kTrust = "for m in $(bluetoothctl devices Paired | cut -d' ' -f2); do bluetoothctl trust $m; done";
constexpr const char *kClassHeadphones = "hciconfig hci0 class 0x4c0418";  // Audio/Video, headphones
constexpr const char *kClassSpeaker = "hciconfig hci0 class 0x4c0414";     // Audio/Video, loudspeaker
constexpr const char *kExpired = "bluetoothctl pairable off";  // window timed out: no more new pairings

}  // namespace

Bluetooth::Bluetooth(Loop &loop, std::function<void()> changed) : loop_(loop), changed_(std::move(changed)) {}

Bluetooth::~Bluetooth() {
  *alive_ = false;
  {
    std::lock_guard<std::mutex> lk(mu_);
    stop_ = true;
  }
  cv_.notify_all();
  if (th_.joinable()) th_.join();
}

void Bluetooth::set_headphones(bool on) {
  headphones_ = on;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (want_ < 0) want_ = 3;  // a pending on / off sets the class itself
  }
  cv_.notify_all();
}

void Bluetooth::start() { th_ = std::thread([this] { run(); }); }

void Bluetooth::set_pairing(bool on) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    want_ = on ? 1 : 0;
  }
  cv_.notify_all();
}

bool Bluetooth::pairing() const {
  std::lock_guard<std::mutex> lk(mu_);
  return pairing_;
}

bool Bluetooth::query_discoverable() {
  FILE *p = popen("bluetoothctl show 2>/dev/null", "r");
  if (!p) return false;
  char line[256];
  bool on = false;
  while (fgets(line, sizeof line, p))
    if (strstr(line, "Discoverable: yes")) on = true;
  pclose(p);
  return on;
}

void Bluetooth::run() {
  std::unique_lock<std::mutex> lk(mu_);
  for (;;) {
    // while the window is open, look every 10 s whether it timed out
    if (pairing_) cv_.wait_for(lk, std::chrono::seconds(10), [this] { return stop_ || want_ >= 0; });
    else cv_.wait(lk, [this] { return stop_ || want_ >= 0; });
    if (stop_) return;
    const int want = want_;
    want_ = -1;
    lk.unlock();
    const char *cls = headphones_ ? kClassHeadphones : kClassSpeaker;
    if (want == 3) {
      system(cls);
    } else if (want == 1) {
      system(cls);
      if (system(kOn) != 0) LOGW("bluetooth: bluetoothctl failed");
    } else if (want == 0) {
      if (system(kOff) != 0) LOGW("bluetooth: bluetoothctl failed");
    }
    const bool now = query_discoverable();
    if (now) {
      system(cls);
      system(kTrust);
    }
    lk.lock();
    const bool was = pairing_;
    pairing_ = now;
    lk.unlock();
    if (was && !now && want != 0) system(kExpired);
    if (was != now) {
      LOGI("bluetooth: pairing %s", now ? "open" : "closed");
      auto alive = alive_;
      loop_.post([this, alive] {
        if (*alive && changed_) changed_();
      });
    }
    lk.lock();
  }
}

}  // namespace cs
