#pragma once

#include <cstdint>
#include <functional>

#include "activities/Activity.h"
#include "components/OptionPopup.h"

class ClearCacheActivity final : public Activity {
 public:
  explicit ClearCacheActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ClearCache", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool skipLoopDelay() override { return true; }  // Prevent power-saving mode
  void render(RenderLock&&) override;

 private:
  enum State { WARNING, CLEARING, SUCCESS, FAILED };

  State state = WARNING;

  void goBack() { finish(); }

  int clearedCount = 0;
  int failedCount = 0;
  uint64_t musicCacheBytes_ = 0;  // online music cache size shown in WARNING
  OptionPopup confirmPopup;
  void beginClear();
  void clearCache();
};
