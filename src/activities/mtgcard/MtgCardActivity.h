#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "activities/Activity.h"

// A small offline-first app: search Scryfall for a Magic: The Gathering
// card by name, download its image, convert it to a display-ready BMP,
// and cache it on the SD card so it can be re-viewed without WiFi.
enum class MtgCardState { CardList, Loading, CardView };

class MtgCardActivity final : public Activity {
 private:
  MtgCardState state = MtgCardState::CardList;

  // Sanitized names (without extension) of cards already cached on the SD card.
  std::vector<std::string> cachedCards;
  int selectedIndex = 0;

  std::string searchQuery;
  std::string currentCardName;
  std::vector<std::string> currentBmpPaths;
  int currentFaceIndex = 0;
  std::string errorMessage;

  bool pendingFetch = false;
  bool pendingUpdate = false;
  bool backgroundFetchFailed = false;
  bool wifiWasUsed = false;

  void* fetchTaskHandle = nullptr;
  std::atomic<bool> cancelFetch{false};

  // Results written by the background fetch task, applied on the next loop().
  std::string bgCardName;
  std::vector<std::string> bgBmpPaths;

  void loadCachedCardsList();
  void performBackgroundFetch();

 public:
  explicit MtgCardActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("MtgCard", renderer, mappedInput) {}

  // Runs on a background FreeRTOS task - do not call from the UI thread.
  void runBackgroundFetch();
  bool fetchCardData();

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
