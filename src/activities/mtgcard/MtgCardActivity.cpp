#include "MtgCardActivity.h"

#include <ArduinoJson.h>
#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <PngToBmpConverter.h>
#include <WiFi.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/util/DownloadWatchdog.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "activities/util/WifiConnectHelper.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace {

constexpr const char* APP_DIR = "/apps/mtgcard";

std::string sanitizeFilename(const std::string& name) {
  std::string filename;
  filename.reserve(name.size());
  for (char c : name) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_') {
      filename += c;
    }
  }
  return filename;
}

std::string urlEncode(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (char c : value) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '~') {
      escaped += c;
    } else if (c == ' ') {
      escaped += "%20";
    } else {
      char hex[4];
      snprintf(hex, sizeof(hex), "%%%02X", static_cast<unsigned char>(c));
      escaped += hex;
    }
  }
  return escaped;
}

std::string cardBmpPath(const std::string& sanitizedName) {
  return std::string(APP_DIR) + "/" + sanitizedName + ".bmp";
}

static void mtgFetchTaskFunc(void* param) {
  auto* activity = static_cast<MtgCardActivity*>(param);
  activity->runBackgroundFetch();
  vTaskDelete(nullptr);
}

}  // namespace

void MtgCardActivity::loadCachedCardsList() {
  cachedCards.clear();
  std::vector<String> files = Storage.listFiles(APP_DIR);
  for (const auto& file : files) {
    std::string filename = file.c_str();
    if (filename.length() > 4 && filename.substr(filename.length() - 4) == ".bmp") {
      // Filter out secondary face files from showing up as duplicate entries in the menu
      if (filename.find("_face") == std::string::npos) {
        cachedCards.push_back(filename.substr(0, filename.length() - 4));
      }
    }
  }
  std::sort(cachedCards.begin(), cachedCards.end());
}

void MtgCardActivity::onEnter() {
  Activity::onEnter();
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists(APP_DIR);

  errorMessage.clear();
  loadCachedCardsList();
  state = MtgCardState::CardList;
  selectedIndex = 0;
  currentFaceIndex = 0;

  fetchTaskHandle = nullptr;
  pendingFetch = false;
  pendingUpdate = false;
  backgroundFetchFailed = false;

  requestUpdate();
}

void MtgCardActivity::onExit() {
  Activity::onExit();
  if (fetchTaskHandle != nullptr) {
    TaskHandle_t tempHandle = static_cast<TaskHandle_t>(fetchTaskHandle);
    fetchTaskHandle = nullptr;
    vTaskDelete(tempHandle);
  }
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
  if (wifiWasUsed) {
    silentRestart();
  }
}

void MtgCardActivity::performBackgroundFetch() {
  if (fetchTaskHandle != nullptr) return;
  state = MtgCardState::Loading;
  backgroundFetchFailed = false;
  pendingFetch = false;
  pendingUpdate = false;
  requestUpdate();

  xTaskCreate(mtgFetchTaskFunc, "mtg_fetch", 8192, this, 5, (TaskHandle_t*)&fetchTaskHandle);
}

bool MtgCardActivity::fetchCardData() {
  const std::string jsonTempPath = std::string(APP_DIR) + "/.search.tmp";
  const std::string url = "https://api.scryfall.com/cards/named?fuzzy=" + urlEncode(searchQuery);

  auto result = HttpDownloader::downloadToFile(url, jsonTempPath, nullptr, nullptr, "", "", nullptr, nullptr, nullptr,
                                               nullptr, "*/*");
  if (result != HttpDownloader::OK) {
    LOG_ERR("MTG", "Scryfall request failed (%d) for %s", static_cast<int>(result), url.c_str());
    if (result == HttpDownloader::FILE_ERROR) {
      errorMessage = "Could not save the Scryfall response.";
    } else if (result == HttpDownloader::ABORTED) {
      errorMessage = "Scryfall request was cancelled.";
    } else {
      errorMessage = "Scryfall request failed. Check WiFi and try again.";
    }
    Storage.remove(jsonTempPath.c_str());
    return false;
  }

  std::vector<std::string> imageUrls;
  std::string cardName;
  {
    HalFile jsonFile;
    if (!Storage.openFileForRead("MTG", jsonTempPath, jsonFile)) {
      Storage.remove(jsonTempPath.c_str());
      errorMessage = "Failed to read card data.";
      return false;
    }

    JsonDocument filter;
    filter["name"] = true;
    filter["image_uris"]["png"] = true;
    filter["card_faces"][0]["image_uris"]["png"] = true;
    filter["card_faces"][1]["image_uris"]["png"] = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, jsonFile, DeserializationOption::Filter(filter));
    jsonFile.close();
    Storage.remove(jsonTempPath.c_str());

    if (err) {
      errorMessage = "Failed to parse card data.";
      return false;
    }

    if (doc["name"].is<const char*>()) {
      cardName = doc["name"].as<std::string>();
    }

    if (doc["card_faces"].is<JsonArray>()) {
      for (JsonObject face : doc["card_faces"].as<JsonArray>()) {
        if (face["image_uris"]["png"].is<const char*>()) {
          imageUrls.push_back(face["image_uris"]["png"].as<std::string>());
        }
      }
    }
    if (imageUrls.empty() && doc["image_uris"]["png"].is<const char*>()) {
      imageUrls.push_back(doc["image_uris"]["png"].as<std::string>());
    }
  }

  if (imageUrls.empty()) {
    errorMessage = "That card has no image available.";
    return false;
  }
  if (cardName.empty()) {
    cardName = searchQuery;
  }

  const std::string sanitizedName = sanitizeFilename(cardName);
  bgBmpPaths.clear();

  // Check if all faces are already cached
  bool allCached = true;
  for (size_t i = 0; i < imageUrls.size(); ++i) {
    std::string bmpPath = (i == 0) ? cardBmpPath(sanitizedName)
                                   : std::string(APP_DIR) + "/" + sanitizedName + "_face" + std::to_string(i + 1) + ".bmp";
    if (!Storage.exists(bmpPath.c_str())) {
      allCached = false;
      break;
    }
  }

  if (allCached) {
    bgCardName = cardName;
    for (size_t i = 0; i < imageUrls.size(); ++i) {
      bgBmpPaths.push_back((i == 0) ? cardBmpPath(sanitizedName)
                                    : std::string(APP_DIR) + "/" + sanitizedName + "_face" + std::to_string(i + 1) + ".bmp");
    }
    return true;
  }

  // Download and process each face image
  for (size_t i = 0; i < imageUrls.size(); ++i) {
    std::string bmpPath = (i == 0) ? cardBmpPath(sanitizedName)
                                   : std::string(APP_DIR) + "/" + sanitizedName + "_face" + std::to_string(i + 1) + ".bmp";
    const std::string pngTempPath = std::string(APP_DIR) + "/.card_temp.png";

    result = HttpDownloader::downloadToFile(imageUrls[i], pngTempPath, nullptr, nullptr, "", "", nullptr, nullptr, nullptr,
                                            nullptr, "*/*");
    if (result != HttpDownloader::OK) {
      LOG_ERR("MTG", "Card image download failed (%d): %s", static_cast<int>(result), imageUrls[i].c_str());
      errorMessage = "Failed to download the card image.";
      Storage.remove(pngTempPath.c_str());
      return false;
    }

    bool success = false;
    {
      HalFile pngFile;
      if (Storage.openFileForRead("MTG", pngTempPath, pngFile)) {
        HalFile bmpFile;
        if (Storage.openFileForWrite("MTG", bmpPath, bmpFile)) {
          success = PngToBmpConverter::pngFileToBmpStreamWithSize(pngFile, bmpFile, renderer.getScreenWidth(),
                                                                  renderer.getScreenHeight());
          bmpFile.close();
        }
        pngFile.close();
      }
    }
    Storage.remove(pngTempPath.c_str());

    if (!success) {
      Storage.remove(bmpPath.c_str());
      errorMessage = "Failed to process the card image.";
      return false;
    }

    bgBmpPaths.push_back(bmpPath);
  }

  bgCardName = cardName;
  return true;
}

void MtgCardActivity::runBackgroundFetch() {
  DownloadWatchdog::start(120000);
  errorMessage.clear();

  bool success = false;
  if (WiFi.status() == WL_CONNECTED) {
    if (!WifiConnectHelper::waitForTimeSync()) {
      errorMessage = "Clock sync failed. HTTPS requests may fail.";
    }
    int retries = 2;
    while (retries-- > 0) {
      success = fetchCardData();
      if (success) break;
      if (retries > 0) delay(1500);
    }
  } else {
    errorMessage = "WiFi disconnected during fetch.";
  }

  DownloadWatchdog::stop();

  if (DownloadWatchdog::gotTimeout) {
    LOG_ERR("MTG", "Background fetch timed out!");
    backgroundFetchFailed = true;
    errorMessage = "Request timed out.";
  } else if (!success) {
    backgroundFetchFailed = true;
    if (errorMessage.empty()) errorMessage = "Card lookup failed.";
  } else {
    backgroundFetchFailed = false;
  }

  pendingUpdate = true;
}

void MtgCardActivity::loop() {
  if (pendingFetch) {
    performBackgroundFetch();
    return;
  }

  if (pendingUpdate) {
    pendingUpdate = false;
    fetchTaskHandle = nullptr;
    if (DownloadWatchdog::gotTimeout) {
      LOG_ERR("MTG", "Watchdog timeout! Crashing to home screen.");
      activityManager.goHome();
      return;
    }
    if (!backgroundFetchFailed) {
      currentCardName = bgCardName;
      currentBmpPaths = bgBmpPaths;
      currentFaceIndex = 0;
      errorMessage.clear();
      state = MtgCardState::CardView;
    } else {
      state = MtgCardState::CardList;
      loadCachedCardsList();
      selectedIndex = 0;
    }
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (state == MtgCardState::Loading) {
      if (fetchTaskHandle != nullptr) {
        TaskHandle_t tempHandle = static_cast<TaskHandle_t>(fetchTaskHandle);
        fetchTaskHandle = nullptr;
        vTaskDelete(tempHandle);
      }
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      state = MtgCardState::CardList;
      loadCachedCardsList();
      requestUpdate();
    } else if (state == MtgCardState::CardView) {
      state = MtgCardState::CardList;
      loadCachedCardsList();
      requestUpdate();
    } else {
      finish();
    }
    return;
  }

  if (state == MtgCardState::CardView) {
    if (currentBmpPaths.size() > 1) {
      if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
        currentFaceIndex = (currentFaceIndex - 1 + currentBmpPaths.size()) % currentBmpPaths.size();
        requestUpdate();
        return;
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
        currentFaceIndex = (currentFaceIndex + 1) % currentBmpPaths.size();
        requestUpdate();
        return;
      }
    }
  }

  if (state == MtgCardState::CardList) {
    if (!errorMessage.empty()) {
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        errorMessage.clear();
        requestUpdate();
      }
      return;
    }

    int totalItems = static_cast<int>(cachedCards.size()) + 1;
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      selectedIndex = (selectedIndex - 1 + totalItems) % totalItems;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      selectedIndex = (selectedIndex + 1) % totalItems;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (selectedIndex == 0) {
        auto keyboard = std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "MTG Card Name", "", 60);
        startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
          if (!result.isCancelled) {
            auto keyboardResult = std::get_if<KeyboardResult>(&result.data);
            if (keyboardResult && !keyboardResult->text.empty()) {
              searchQuery = keyboardResult->text;
              state = MtgCardState::Loading;
              requestUpdate();
              ensureWifiConnected(
                  [this]() {
                    wifiWasUsed = true;
                    pendingFetch = true;
                    requestUpdate();
                  },
                  [this]() {
                    state = MtgCardState::CardList;
                    requestUpdate();
                  });
            }
          } else {
            requestUpdate();
          }
        });
      } else {
        const std::string& sanitizedName = cachedCards[selectedIndex - 1];
        currentCardName = sanitizedName;
        currentBmpPaths.clear();
        currentBmpPaths.push_back(cardBmpPath(sanitizedName));

        // Load any additional cached face files for this card
        for (int i = 2; i <= 5; ++i) {
          std::string facePath = std::string(APP_DIR) + "/" + sanitizedName + "_face" + std::to_string(i) + ".bmp";
          if (Storage.exists(facePath.c_str())) {
            currentBmpPaths.push_back(facePath);
          } else {
            break;
          }
        }
        currentFaceIndex = 0;
        state = MtgCardState::CardView;
        requestUpdate();
      }
    }
    return;
  }
}

void MtgCardActivity::render(RenderLock&&) {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  if (state == MtgCardState::CardView) {
    renderer.clearScreen();
    if (!currentBmpPaths.empty() && currentFaceIndex < currentBmpPaths.size()) {
      HalFile file;
      if (Storage.openFileForRead("MTG", currentBmpPaths[currentFaceIndex], file)) {
        Bitmap bitmap(file, true);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          LOG_DBG("MTG", "Card BMP dimensions: %dx%d (screen %dx%d)", bitmap.getWidth(), bitmap.getHeight(), pageWidth,
                  pageHeight);
          int x, y;
          if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
            float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
            const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);
            if (ratio > screenRatio) {
              x = 0;
              y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
            } else {
              x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
              y = 0;
            }
          } else {
            x = (pageWidth - bitmap.getWidth()) / 2;
            y = (pageHeight - bitmap.getHeight()) / 2;
          }
          renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0);
        } else {
          renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, "Invalid card image.");
        }
        file.close();
      } else {
        renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, "Could not open card image.");
      }
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), nullptr,
                                              (currentBmpPaths.size() > 1 ? "< Prev" : nullptr),
                                              (currentBmpPaths.size() > 1 ? "Next >" : nullptr));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return;
  }

  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "MTG Card Search");

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int contentHeight = contentBottom - contentTop;

  if (state == MtgCardState::Loading) {
    int textY = contentTop + contentHeight / 2 - 20;
    renderer.drawCenteredText(UI_12_FONT_ID, textY, "Looking up card on Scryfall...");
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), nullptr, nullptr, nullptr);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    // CardList
    if (!errorMessage.empty()) {
      int textY = contentTop + contentHeight / 2 - 20;
      renderer.drawCenteredText(UI_12_FONT_ID, textY, errorMessage.c_str(), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(SMALL_FONT_ID, textY + 30, "Check spelling and try again.");
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), nullptr, nullptr);
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    } else {
      GUI.drawButtonMenu(
          renderer, Rect{0, contentTop, pageWidth, contentHeight}, cachedCards.size() + 1, selectedIndex,
          [this](int index) {
            if (index == 0) return std::string("[+ Search for a card]");
            return cachedCards[index - 1];
          },
          [](int index) { return UIIcon::Book; }, 9);

      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
  }

  renderer.displayBuffer();
}
