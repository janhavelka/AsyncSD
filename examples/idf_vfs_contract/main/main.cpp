#include "AsyncSD/AsyncSD.h"

#include "esp_log.h"
#include "esp_timer.h"

namespace {

constexpr char TAG[] = "asyncsd_idf";

uint32_t nowMs(void* /*user*/) {
  return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

bool isMounted(void* /*user*/) {
  // Replace with an application-owned FatFS/VFS mount check.
  return false;
}

}  // namespace

extern "C" void app_main(void) {
  AsyncSD::SdCardConfig cfg;
  cfg.backend = AsyncSD::Backend::IDF_VFS;
  cfg.mountPoint = "/sdcard";
  cfg.useWorkerTask = false;
  cfg.idfVfs.mountPoint = "/sdcard";
  cfg.idfVfs.nowMs = nowMs;
  cfg.idfVfs.isMounted = isMounted;

  AsyncSD::SdCardManager sd;
  const bool started = sd.begin(cfg);
  ESP_LOGI(TAG, "begin=%d status=%u error=%u",
           started ? 1 : 0,
           static_cast<unsigned>(sd.status()),
           static_cast<unsigned>(sd.lastError()));
  ESP_LOGW(TAG, "ESP-IDF VFS backend contract is public, implementation is pending");
}
