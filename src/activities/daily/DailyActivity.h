#pragma once

#include <ArduinoJson.h>
#include <I18n.h>

#include <array>
#include <atomic>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class DailyActivity final : public Activity {
 public:
  DailyActivity(GfxRenderer& renderer, MappedInputManager& input);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return connecting; }

  // Startup routing: true when both the feed config and the opt-in marker exist.
  static bool autostartEnabled();
  // Records an automatic attempt before Today opens at startup. Returns false
  // when the previous attempt never finished (reset/crash during sync) or the
  // marker cannot be written; startup then uses ordinary Home. Any completed
  // sync, manual or automatic, clears the marker.
  static bool beginAutostartAttempt();

 private:
  struct Entry {
    char title[257] = {};
    // Card title starts here: the manifest title after its "Book — " prefix.
    uint16_t titleOffset = 0;
    char file[96] = {};
    char excerpt[240] = {};
    // Quote attribution's name ("— Seneca, On…" -> "Seneca"), empty otherwise.
    char author[48] = {};
    bool quote = false;
    bool read = false;
    int minutes = 0;
  };
  // Fixed screen-lifetime storage avoids allocation in the render and sync paths.
  std::array<Entry, 2> entries;
  char savedDate[11] = {};
  char dateLine[64] = {};
  int selected = 0;
  ButtonNavigator buttonNavigator;
  char server[160] = {};
  char url[208] = {};
  char dest[96] = {};
  char temporary[100] = {};
  char chunk[192] = {};
  bool started = false;
  bool connecting = false;
  bool ownsWifi = false;
  bool leaveRequested = false;
  unsigned long connectStarted = 0;
  // Whole-sync budget, from Wi-Fi join to the last byte written.
  unsigned long syncStarted = 0;
  std::atomic<StrId> status{StrId::STR_DAILY_OFFLINE};

  void activate(int index);
  void loadDetails(Entry& entry);
  void drawCard(const Entry& entry, int index, int x, int y, int width, int height) const;
  void beginSync();
  void finishSync();
  void endSync(StrId result);
  void stopWifi();
  unsigned long remainingMs() const;
  int socketTimeoutMs() const;
  bool shouldAbort();
  bool download(const char* path, size_t maximum);
  static bool readManifest(const char* path, JsonDocument& doc);
  bool validateManifest(const JsonDocument& doc) const;
  bool installManifest(const JsonDocument& doc);
  void loadCache();
};
