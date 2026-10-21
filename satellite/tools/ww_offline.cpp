// Offline evaluation of a stock spotter model on a recording:
// 8ch/96k S16 WAV (4 mics + 4 speaker refs, as captured from hw:0,2) -> Yandex VQE -> spotter.
// The spotter channel layout is derived from the model like the daemon does (audio/frontend.cpp).
// usage: ww_offline <wav> [model_dir]   (default: .../spotter_models/activation/alisa)
// Prints every spotter event with its time; "HIT" lines are confirmations (events 2 and 5).
#include <dlfcn.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

typedef void *P;
static void *lib;
template <class F>
F sym(const char *n) {
  void *p = dlsym(lib, n);
  if (!p) {
    printf("missing %s\n", n);
    exit(1);
  }
  return (F)p;
}
static const char *serr(P e) { return e ? sym<const char *(*)(P)>("spotter_error_get_message")(e) : "ok"; }
static const char *verr(P e) { return e ? sym<const char *(*)(P)>("YandexVqeC_Error_GetMessage")(e) : "ok"; }

// Same mapping as audio/frontend.cpp: spotter channel type -> VQE output type.
static const int kSpotterToVqe[6] = {9, 2, 9, 2, 6, 6};

int main(int argc, char **argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 2) {
    fprintf(stderr, "usage: %s wav [model_dir]\n", argv[0]);
    return 2;
  }
  const char *model = argc > 2 ? argv[2] : "/system/vendor/quasar/spotter_models/activation/alisa";
  dlopen("/system/vendor/quasar/libs/libtflite.so", RTLD_NOW | RTLD_GLOBAL);
  lib = dlopen("/system/vendor/quasar/libs/libyandex_evil.so", RTLD_NOW | RTLD_GLOBAL);
  if (!lib) return printf("dlopen %s\n", dlerror()), 1;
  FILE *f = fopen(argv[1], "rb");
  if (!f) return perror("wav"), 1;
  fseek(f, 44, SEEK_SET);

  P cfg = nullptr, vqe = nullptr;
  P e = sym<P (*)(const char *, P *)>("YandexVqeC_Config_CreateFromPreset")("cucumber", &cfg);
  if (e) return printf("cfg: %s\n", verr(e)), 1;
  e = sym<P (*)(P, const char *, size_t, const void *, P *)>("YandexVqeC_Create")(cfg, "/system/vendor/quasar/vqe_models",
                                                                                 0, nullptr, &vqe);
  if (e) return printf("vqe: %s\n", verr(e)), 1;
  auto process = sym<P (*)(P, const float *, size_t, const float *, size_t)>("YandexVqeC_Process");
  auto out = sym<P (*)(P, int, size_t, const float **, size_t *)>("YandexVqeC_GetOutputChannelData");

  P se = nullptr;
  P conf = sym<P (*)(const char *, P *)>("spotter_conf_read")(model, &se);
  if (!conf) return printf("conf: %s\n", serr(se)), 1;
  std::vector<int> types, vt;
  for (int t = 0; t < 6; t++) {
    long n = sym<long (*)(P, long, P *)>("spotter_conf_get_required_channel_count")(conf, t, &se);
    for (long i = 0; i < n && n < 8; i++) {
      types.push_back(t);
      vt.push_back(getenv("VT") ? atoi(getenv("VT")) : kSpotterToVqe[t]);  // VT=<type>: override for experiments
    }
  }
  P sp = sym<P (*)(P, int, int, const int *, bool, P *)>("spotter_create_multichannel")(conf, 16000, (int)types.size(),
                                                                                        types.data(), false, &se);
  if (!sp) return printf("spotter: %s\n", serr(se)), 1;
  printf("model %s: %zu channels\n", model, types.size());
  auto submit = sym<void (*)(P, const void *, size_t, P *)>("spotter_submit_raw_data");
  auto next = sym<int (*)(P, P *)>("spotter_get_next_event");
  auto evname = sym<const char *(*)(int)>("event_id_to_str");
  auto phrase = sym<const char *(*)(P, P *)>("spotter_get_current_phrase");

  const size_t frames = 3072, nch = types.size();
  std::vector<int16_t> in(frames * 8), sbuf(512 * nch);
  std::vector<float> mics(frames * 4), spk(frames * 4);
  double t = 0;
  int hits = 0;
  while (fread(in.data(), 2, in.size(), f) == in.size()) {
    for (size_t i = 0; i < frames; i++)
      for (int c = 0; c < 4; c++) {
        mics[i * 4 + c] = in[i * 8 + c] / 32768.f;
        spk[i * 4 + c] = in[i * 8 + 4 + c] / 32768.f;
      }
    if ((e = process(vqe, mics.data(), mics.size(), spk.data(), spk.size()))) return printf("process: %s\n", verr(e)), 1;
    for (size_t k = 0; k < nch; k++) {
      const float *d;
      size_t n;
      if ((e = out(vqe, vt[k], 0, &d, &n))) return printf("out %d: %s\n", vt[k], verr(e)), 1;
      for (size_t i = 0; i < 512 && i < n; i++) {
        float v = d[i] * 32768.f;
        sbuf[i * nch + k] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
      }
    }
    submit(sp, sbuf.data(), sbuf.size() * 2, &se);
    if (se) return printf("submit: %s\n", serr(se)), 1;
    for (int guard = 0; guard < 16; guard++) {
      int ev = next(sp, &se);
      if (se || ev == 0 || ev == 4 || ev == 7 || ev == 9) break;
      const char *ph = phrase(sp, &se);
      bool hit = ev == 2 || ev == 5;
      hits += hit;
      printf("t=%6.2fs event %d %s phrase=%s%s\n", t, ev, evname(ev), ph ? ph : "", hit ? "  HIT" : "");
    }
    t += frames / 96000.0;
  }
  printf("done: %.1fs, hits=%d\n", t, hits);
  return 0;
}
