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
    if (want == 1) {
      if (system(kOn) != 0) LOGW("bluetooth: bluetoothctl failed");
    } else if (want == 0) {
      if (system(kOff) != 0) LOGW("bluetooth: bluetoothctl failed");
    }
    const bool now = query_discoverable();
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
