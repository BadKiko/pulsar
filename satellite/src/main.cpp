// cucumber-satellite: Home Assistant voice satellite for Yandex Station Midi.
// usage: cucumber-satellite [state_file]
#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include "controller.h"
#include "core/log.h"
#include "core/loop.h"

int main(int argc, char **argv) {
  const char *state = argc > 1 ? argv[1] : "/data/cucumber-satellite/state.conf";
  signal(SIGPIPE, SIG_IGN);

  cs::Loop loop;
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &mask, nullptr);  // before any thread starts: workers inherit the mask
  int sfd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
  loop.add_fd(sfd, 0x001 /* EPOLLIN */, [&](uint32_t) {
    signalfd_siginfo si;
    while (read(sfd, &si, sizeof si) == sizeof si) {
      LOGI("signal %u, exiting", si.ssi_signo);
      loop.stop();
    }
  });

  cs::Controller ctl(loop, state);
  if (!ctl.start()) return 1;
  LOGI("cucumber-satellite running");
  loop.run();
  return 0;
}
