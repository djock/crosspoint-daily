#include "DailyActivity.h"

#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <esp_http_client.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "DailyPassages.h"
#include "WifiCredentialStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr const char* CACHE = "/.crosspoint/daily-cache.json";
constexpr const char* BACKUP = "/.crosspoint/daily-cache-backup.json";
constexpr const char* NEXT = "/.crosspoint/daily-next.json";
constexpr const char* CONFIG = "/.crosspoint/daily.json";
constexpr const char* AUTOSTART = "/.crosspoint/daily-autostart";
constexpr const char* ATTEMPT = "/.crosspoint/daily-attempt";
constexpr unsigned long JOIN_TIMEOUT_MS = 7000;
constexpr unsigned long SYNC_BUDGET_MS = 20000;
constexpr unsigned long SOCKET_TIMEOUT_MS = 2500;
using daily_passages::KINDS;
using daily_passages::LABELS;
constexpr StrId MONTHS[] = {StrId::STR_DAILY_MONTH_JAN, StrId::STR_DAILY_MONTH_FEB, StrId::STR_DAILY_MONTH_MAR,
                            StrId::STR_DAILY_MONTH_APR, StrId::STR_DAILY_MONTH_MAY, StrId::STR_DAILY_MONTH_JUN,
                            StrId::STR_DAILY_MONTH_JUL, StrId::STR_DAILY_MONTH_AUG, StrId::STR_DAILY_MONTH_SEP,
                            StrId::STR_DAILY_MONTH_OCT, StrId::STR_DAILY_MONTH_NOV, StrId::STR_DAILY_MONTH_DEC};
constexpr StrId WEEKDAYS[] = {StrId::STR_DAILY_DAY_SUN, StrId::STR_DAILY_DAY_MON, StrId::STR_DAILY_DAY_TUE,
                              StrId::STR_DAILY_DAY_WED, StrId::STR_DAILY_DAY_THU, StrId::STR_DAILY_DAY_FRI,
                              StrId::STR_DAILY_DAY_SAT};
constexpr int WORDS_PER_MINUTE = 200;
constexpr const char* EM_DASH_SEPARATOR = " \u2014 ";

// 0 = Sunday. Sakamoto's method; valid for the Gregorian dates validDate accepts.
int weekday(int year, const int month, const int day) {
  constexpr int OFFSETS[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (month < 3) --year;
  return (year + year / 4 - year / 100 + year / 400 + OFFSETS[month - 1] + day) % 7;
}

// English ordinal suffix: 1st, 2nd, 3rd, 4th ... 11th-13th, 21st, 22nd, 23rd, 31st.
const char* ordinalSuffix(const int day) {
  if (day % 100 >= 11 && day % 100 <= 13) return "th";
  switch (day % 10) {
    case 1:
      return "st";
    case 2:
      return "nd";
    case 3:
      return "rd";
    default:
      return "th";
  }
}

struct JsonFileReader {
  HalFile& file;
  int read() { return file.read(); }
  size_t readBytes(char* buffer, size_t count) {
    const int received = file.read(buffer, count);
    return received > 0 ? static_cast<size_t>(received) : 0;
  }
};

bool validDate(const char* date) {
  if (!date || strlen(date) != 10) return false;
  for (int i = 0; i < 10; ++i) {
    if (i == 4 || i == 7) {
      if (date[i] != '-') return false;
    } else if (date[i] < '0' || date[i] > '9') {
      return false;
    }
  }
  constexpr uint8_t DAYS[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const int year = atoi(date);
  const int month = (date[5] - '0') * 10 + date[6] - '0';
  const int day = (date[8] - '0') * 10 + date[9] - '0';
  if (month < 1 || month > 12) return false;
  const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  return day >= 1 && day <= (month == 2 && leap ? 29 : DAYS[month - 1]);
}

// Accepts only http://a.b.c.d[:port][/]. A literal address avoids DNS lookups,
// which run outside the HTTP client's socket timeout and the sync budget.
bool validServerUrl(const char* url) {
  if (strncmp(url, "http://", 7) != 0) return false;
  const char* p = url + 7;
  for (int octet = 0; octet < 4; ++octet) {
    if (octet && *p++ != '.') return false;
    int value = 0;
    int digits = 0;
    while (*p >= '0' && *p <= '9' && digits < 3) {
      value = value * 10 + (*p++ - '0');
      ++digits;
    }
    if (!digits || value > 255) return false;
  }
  if (*p == ':') {
    ++p;
    long port = 0;
    int digits = 0;
    while (*p >= '0' && *p <= '9' && digits < 5) {
      port = port * 10 + (*p++ - '0');
      ++digits;
    }
    if (port < 1 || port > 65535) return false;
  }
  if (*p == '/') ++p;
  return *p == '\0';
}

int kindIndex(const char* kind) {
  if (!kind) return -1;
  for (int i = 0; i < 2; ++i) {
    if (strcmp(kind, KINDS[i]) == 0) return i;
  }
  return -1;
}
}  // namespace

bool DailyActivity::autostartEnabled() { return Storage.exists(CONFIG) && Storage.exists(AUTOSTART); }

bool DailyActivity::beginAutostartAttempt() {
  if (Storage.exists(ATTEMPT)) {
    LOG_ERR("DAILY", "Previous automatic sync did not finish; starting Home");
    return false;
  }
  HalFile marker;
  return Storage.openFileForWrite("DAILY", ATTEMPT, marker);
}

DailyActivity::DailyActivity(GfxRenderer& renderer, MappedInputManager& input) : Activity("Daily", renderer, input) {}

void DailyActivity::onEnter() {
  // Parse only on entry/sync with a 2KB input cap. ArduinoJson's fallible heap
  // pool avoids a document-sized allocation on the small task stack.
  JsonDocument config;
  if (readManifest(CONFIG, config)) {
    const char* configured = config["url"] | "";
    if (validServerUrl(configured) && strlen(configured) < sizeof(server)) {
      snprintf(server, sizeof(server), "%s", configured);
      const auto length = strlen(server);
      if (server[length - 1] == '/') server[length - 1] = '\0';
    } else {
      LOG_ERR("DAILY", "Feed URL must be http://<IPv4 address>[:port]");
    }
  }
  loadCache();
  Activity::onEnter();
  requestUpdate();
}

void DailyActivity::onExit() {
  stopWifi();
  // Leaving Today is an orderly finish, not a failed automatic start.
  Storage.remove(ATTEMPT);
  Activity::onExit();
}

bool DailyActivity::readManifest(const char* path, JsonDocument& doc) {
  HalFile file;
  if (!Storage.openFileForRead("DAILY", path, file) || file.size() > 2048) return false;
  JsonFileReader reader{file};
  return !deserializeJson(doc, reader);
}

bool DailyActivity::validateManifest(const JsonDocument& doc) const {
  const char* date = doc["date"] | "";
  const char* today = doc["today"] | "";
  if (doc["schema"].as<int>() != 1 || !validDate(date) || !validDate(today) || strcmp(date, today) > 0 ||
      !doc["entries"].is<JsonArrayConst>()) {
    return false;
  }
  auto items = doc["entries"].as<JsonArrayConst>();
  if (items.size() == 0 || items.size() > 2) return false;
  unsigned seen = 0;
  char expected[48];
  for (auto item : items) {
    const int index = kindIndex(item["kind"] | "");
    if (index < 0 || (seen & (1u << index))) return false;
    seen |= 1u << index;
    const char* title = item["title"] | "";
    if (!title[0] || strlen(title) >= sizeof(entries[0].title)) return false;
    snprintf(expected, sizeof(expected), "/daily/%s/%s.txt", date, KINDS[index]);
    if (strcmp(item["path"] | "", expected) != 0) return false;
  }
  return true;
}

bool DailyActivity::installManifest(const JsonDocument& doc) {
  if (!validateManifest(doc)) return false;
  RenderLock lock(*this);
  // Cleared in place: an Entry is too large for a stack temporary.
  for (auto& entry : entries) {
    entry.title[0] = entry.file[0] = entry.excerpt[0] = entry.author[0] = '\0';
    entry.titleOffset = 0;
    entry.quote = false;
    entry.read = false;
    entry.minutes = 0;
  }
  const char* date = doc["date"];
  snprintf(savedDate, sizeof(savedDate), "%s", date);
  // "Thursday, 1st October 2026"; validateManifest guarantees a valid calendar date.
  const int year = atoi(date);
  const int month = atoi(date + 5);
  const int day = atoi(date + 8);
  snprintf(dateLine, sizeof(dateLine), "%s, %d%s %s %d", I18N.get(WEEKDAYS[weekday(year, month, day)]), day,
           ordinalSuffix(day), I18N.get(MONTHS[month - 1]), year);
  for (auto item : doc["entries"].as<JsonArrayConst>()) {
    const int index = kindIndex(item["kind"]);
    snprintf(entries[index].file, sizeof(entries[index].file), "/Daily/%s-%s.txt", date, KINDS[index]);
    if (!Storage.exists(entries[index].file)) {
      entries[index].file[0] = '\0';
      continue;
    }
    auto& entry = entries[index];
    snprintf(entry.title, sizeof(entry.title), "%s", item["title"].as<const char*>());
    const char* separator = strstr(entry.title, EM_DASH_SEPARATOR);
    entry.titleOffset = separator ? static_cast<uint16_t>(separator - entry.title + strlen(EM_DASH_SEPARATOR)) : 0;
    loadDetails(entry);
  }
  // Open on the first unread passage.
  selected = entries[0].read && entries[1].file[0] && !entries[1].read ? 1 : 0;
  return true;
}

void DailyActivity::loadCache() {
  JsonDocument doc;
  if (readManifest(CACHE, doc) && installManifest(doc)) return;
  doc.clear();
  if (readManifest(BACKUP, doc)) installManifest(doc);
}

unsigned long DailyActivity::remainingMs() const {
  const unsigned long elapsed = millis() - syncStarted;
  return elapsed >= SYNC_BUDGET_MS ? 0 : SYNC_BUDGET_MS - elapsed;
}

// Never zero: esp_transport treats a zero timeout as a non-blocking poll.
int DailyActivity::socketTimeoutMs() const {
  return static_cast<int>(std::max(1UL, std::min(SOCKET_TIMEOUT_MS, remainingMs())));
}

bool DailyActivity::shouldAbort() {
  // Transfers block the activity loop, so pump input here as the upstream
  // download screens do; Back or Home leaves Today once the current step ends.
  mappedInput.update(true);
  if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasPressed(MappedInputManager::Button::Back) || mappedInput.wasHomeGesture()) {
    leaveRequested = true;
  }
  return leaveRequested || remainingMs() == 0;
}

// Downloads `url` to path. Every blocking call gets at most the remaining sync
// budget as its socket timeout, so the whole sync ends near SYNC_BUDGET_MS.
bool DailyActivity::download(const char* path, const size_t maximum) {
  if (shouldAbort()) return false;
  // Client configuration exceeds the stack budget: allocate it once per
  // transfer with an OOM check and free it when the transfer ends.
  auto config = makeUniqueNoThrow<esp_http_client_config_t>();
  if (!config) {
    LOG_ERR("DAILY", "OOM: HTTP configuration");
    return false;
  }
  config->url = url;
  config->timeout_ms = socketTimeoutMs();
  config->buffer_size = 512;
  config->disable_auto_redirect = true;
  auto client = esp_http_client_init(config.get());
  if (!client) {
    LOG_ERR("DAILY", "HTTP client init failed");
    return false;
  }
  bool ok = esp_http_client_open(client, 0) == ESP_OK && !shouldAbort();
  if (ok) esp_http_client_set_timeout_ms(client, socketTimeoutMs());
  int64_t length = ok ? esp_http_client_fetch_headers(client) : -1;
  ok = ok && esp_http_client_get_status_code(client) == 200 && length > 0 && length <= static_cast<int64_t>(maximum);
  HalFile file;
  if (ok) ok = Storage.openFileForWrite("DAILY", path, file);
  while (ok && length > 0) {
    if (shouldAbort()) {
      ok = false;
      break;
    }
    esp_http_client_set_timeout_ms(client, socketTimeoutMs());
    const int received = esp_http_client_read(client, chunk, std::min<int64_t>(sizeof(chunk), length));
    if (received <= 0 || file.write(chunk, received) != static_cast<size_t>(received)) {
      ok = false;
      break;
    }
    length -= received;
    delay(1);
  }
  // Never opened when the connection or headers failed; close() asserts on that.
  if (file) file.close();
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  if (!ok) {
    LOG_ERR("DAILY", "Download failed: %s", path);
    Storage.remove(path);
  }
  return ok;
}

void DailyActivity::stopWifi() {
  if (ownsWifi) {
    WiFi.disconnect(false);
    WiFi.mode(WIFI_OFF);
    ownsWifi = false;
  }
  connecting = false;
}

void DailyActivity::endSync(const StrId result) {
  stopWifi();
  Storage.remove(ATTEMPT);
  status = result;
  requestUpdate();
}

void DailyActivity::beginSync() {
  started = true;
  if (!server[0]) {
    endSync(StrId::STR_DAILY_SETUP);
    return;
  }
  if (powerManager.getBatteryPercentage() < 20) {
    endSync(StrId::STR_DAILY_LOW_BATTERY);
    return;
  }
  // The credential store is loaded lazily by the Wi-Fi screens, not at boot.
  {
    RenderLock lock(*this);
    WIFI_STORE.loadFromFile();
  }
  const auto credential = WIFI_STORE.findCredential(WIFI_STORE.getLastConnectedSsid());
  if (!credential) {
    endSync(StrId::STR_DAILY_WIFI_SETUP);
    return;
  }
  status = StrId::STR_DAILY_CONNECTING;
  requestUpdateAndWait();
  WiFi.mode(WIFI_STA);
  ownsWifi = true;
  WiFi.begin(credential->ssid.c_str(), credential->password.c_str());
  syncStarted = connectStarted = millis();
  connecting = true;
}

void DailyActivity::finishSync() {
  HalPowerManager::Lock powerLock;
  JsonDocument doc;
  snprintf(url, sizeof(url), "%s/daily/latest.json", server);
  bool ok = Storage.ensureDirectoryExists("/Daily") && download(NEXT, 2048) && readManifest(NEXT, doc) &&
            validateManifest(doc);
  if (ok) {
    // A failed sync leaves the previous manifest and dated passages intact.
    const char* date = doc["date"];
    for (auto item : doc["entries"].as<JsonArrayConst>()) {
      const int index = kindIndex(item["kind"]);
      snprintf(dest, sizeof(dest), "/Daily/%s-%s.txt", date, KINDS[index]);
      if (Storage.exists(dest)) continue;
      snprintf(temporary, sizeof(temporary), "%s.new", dest);
      snprintf(url, sizeof(url), "%s%s", server, item["path"].as<const char*>());
      if (!download(temporary, 50 * 1024) || !Storage.rename(temporary, dest)) {
        Storage.remove(temporary);
        ok = false;
        break;
      }
    }
  }
  // FAT rename does not overwrite: keep the previous metadata as a backup
  // across the promotion so an interrupted swap can fall back to it.
  if (ok && Storage.exists(CACHE)) {
    Storage.remove(BACKUP);
    ok = Storage.rename(CACHE, BACKUP);
  }
  if (ok && Storage.rename(NEXT, CACHE)) {
    installManifest(doc);
    endSync(strcmp(doc["date"] | "", doc["today"] | "") == 0 ? StrId::STR_DAILY_READY : StrId::STR_DAILY_OFFLINE);
  } else {
    Storage.remove(NEXT);
    endSync(StrId::STR_DAILY_FAILED);
  }
}

void DailyActivity::loop() {
  if (!started) beginSync();
  if (connecting) {
    if (WiFi.status() == WL_CONNECTED) {
      finishSync();
    } else if (millis() - connectStarted >= JOIN_TIMEOUT_MS) {
      endSync(StrId::STR_DAILY_FAILED);
    }
  }
  if (leaveRequested || mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goHome();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activate(selected);
    return;
  }
  // Two cards: next and previous both toggle the selection.
  buttonNavigator.onNext([this] {
    selected = 1 - selected;
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    selected = 1 - selected;
    requestUpdate();
  });
}

void DailyActivity::activate(const int index) {
  if (index < 0 || index >= 2 || !entries[index].file[0]) return;
  activityManager.goToReader(entries[index].file);
}

// Reads the saved passage once per manifest install: the first body paragraph
// becomes the card excerpt and the word count gives the read time. The file
// layout is "title\ndate\n\nbody..." as written by the Pi feed.
void DailyActivity::loadDetails(Entry& entry) {
  HalFile file;
  if (!Storage.openFileForRead("DAILY", entry.file, file)) return;
  int newlines = 0;
  int words = 0;
  bool inWord = false;
  size_t used = 0;
  size_t authorUsed = 0;
  // 0 = excerpt paragraph, 1 = the paragraph after it, 2 = rest of the body.
  int paragraph = 0;
  bool authorDone = false;
  int received;
  while ((received = file.read(chunk, sizeof(chunk))) > 0) {
    for (int i = 0; i < received; ++i) {
      const char c = chunk[i];
      const bool space = c == ' ' || c == '\n' || c == '\r' || c == '\t';
      if (!space && !inWord) ++words;
      inWord = !space;
      if (newlines < 2) {
        if (c == '\n') ++newlines;
        continue;
      }
      if (c == '\n') {
        if (paragraph == 0 && used > 0) {
          paragraph = 1;
        } else if (paragraph == 1 && authorUsed > 0) {
          paragraph = 2;
        }
        continue;
      }
      if (c == '\r') continue;
      if (paragraph == 0) {
        if (used + 1 < sizeof(entry.excerpt)) entry.excerpt[used++] = c;
      } else if (paragraph == 1 && !authorDone) {
        if (c == ',') {
          authorDone = true;
        } else if (authorUsed + 1 < sizeof(entry.author)) {
          entry.author[authorUsed++] = c;
        }
      }
    }
  }
  entry.excerpt[used] = '\0';
  entry.author[authorUsed] = '\0';
  // Quotes open with a curly double quote (U+201C); their attribution line
  // starts with an em dash (U+2014): keep only the name after it.
  entry.quote = strncmp(entry.excerpt, "\xE2\x80\x9C", 3) == 0;
  if (entry.quote && strncmp(entry.author, "\xE2\x80\x94", 3) == 0) {
    const char* name = entry.author + 3;
    while (*name == ' ') ++name;
    memmove(entry.author, name, strlen(name) + 1);
  } else {
    entry.author[0] = '\0';
  }
  entry.minutes = std::max(1, (words + WORDS_PER_MINUTE / 2) / WORDS_PER_MINUTE);
  entry.read = daily_passages::isRead(entry.file);
}

void DailyActivity::drawCard(const Entry& entry, const int index, const int x, const int y, const int width,
                             const int height) const {
  constexpr int PADDING = 16;
  const bool isSelected = index == selected;
  renderer.drawRect(x, y, width, height, isSelected ? 4 : 1, true);
  const int innerX = x + PADDING;
  const int innerWidth = width - 2 * PADDING;
  int cursor = y + PADDING;

  renderer.drawText(SMALL_FONT_ID, innerX, cursor, I18N.get(LABELS[index]), true, EpdFontFamily::BOLD);
  cursor += renderer.getLineHeight(SMALL_FONT_ID) + 6;

  if (!entry.file[0]) {
    renderer.drawText(NOTOSERIF_14_FONT_ID, innerX, cursor, tr(STR_DAILY_PENDING), true, EpdFontFamily::ITALIC);
    return;
  }

  const int titleHeight = renderer.getLineHeight(NOTOSERIF_16_FONT_ID);
  for (const auto& line : renderer.wrappedText(NOTOSERIF_16_FONT_ID, entry.title + entry.titleOffset, innerWidth, 2,
                                               EpdFontFamily::BOLD)) {
    renderer.drawText(NOTOSERIF_16_FONT_ID, innerX, cursor, line.c_str(), true, EpdFontFamily::BOLD);
    cursor += titleHeight;
  }
  cursor += 6;

  const int metaHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int metaTop = y + height - PADDING - metaHeight;
  const int excerptHeight = renderer.getLineHeight(NOTOSERIF_12_FONT_ID);
  const int excerptLines = std::max(0, (metaTop - 10 - cursor) / excerptHeight);
  const auto style = entry.quote ? EpdFontFamily::ITALIC : EpdFontFamily::REGULAR;
  if (excerptLines > 0 && entry.excerpt[0]) {
    for (const auto& line :
         renderer.wrappedText(NOTOSERIF_12_FONT_ID, entry.excerpt, innerWidth, excerptLines, style)) {
      renderer.drawText(NOTOSERIF_12_FONT_ID, innerX, cursor, line.c_str(), true, style);
      cursor += excerptHeight;
    }
  }

  renderer.drawLine(innerX, metaTop - 6, innerX + innerWidth, metaTop - 6, true);
  char meta[80];
  int length = entry.read ? snprintf(meta, sizeof(meta), "%s \u00B7 ", tr(STR_DAILY_FINISHED_SHORT)) : 0;
  if (length < 0 || static_cast<size_t>(length) >= sizeof(meta)) length = 0;
  length += snprintf(meta + length, sizeof(meta) - length, tr(STR_DAILY_MINUTES), entry.minutes);
  if (entry.author[0] && length > 0 && static_cast<size_t>(length) < sizeof(meta)) {
    snprintf(meta + length, sizeof(meta) - length, " \u00B7 %s", entry.author);
  }
  const char* read = tr(STR_DAILY_READ);
  const int readWidth = renderer.getTextWidth(UI_10_FONT_ID, read, EpdFontFamily::BOLD);
  // A long attribution ellipsizes before it reaches "Read ›".
  renderer.drawText(UI_10_FONT_ID, innerX, metaTop,
                    renderer.truncatedText(UI_10_FONT_ID, meta, innerWidth - readWidth - 12).c_str());
  renderer.drawText(UI_10_FONT_ID, innerX + innerWidth - readWidth, metaTop, read, true, EpdFontFamily::BOLD);
}

void DailyActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{safe.x, safe.y + metrics.topPadding, safe.width, metrics.headerHeight},
                 tr(STR_DAILY_TODAY), dateLine[0] ? dateLine : nullptr, false);

  constexpr int MARGIN = 18;
  constexpr int GAP = 14;
  const int statusHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int top = safe.y + metrics.topPadding + metrics.headerHeight + 10;
  const int statusTop = safe.y + safe.height - metrics.buttonHintsHeight - statusHeight - 10;
  const int cardHeight = (statusTop - 10 - top - GAP) / 2;
  const int cardWidth = safe.width - 2 * MARGIN;
  for (int i = 0; i < 2; ++i) {
    drawCard(entries[i], i, safe.x + MARGIN, top + i * (cardHeight + GAP), cardWidth, cardHeight);
  }
  // "Up to date · 1 of 2 read" once passages are saved.
  int saved = 0;
  int read = 0;
  for (const auto& entry : entries) {
    saved += entry.file[0] ? 1 : 0;
    read += entry.read ? 1 : 0;
  }
  char statusText[96];
  int length = snprintf(statusText, sizeof(statusText), "%s", I18N.get(status.load()));
  if (saved > 0 && length > 0 && static_cast<size_t>(length) < sizeof(statusText)) {
    length += snprintf(statusText + length, sizeof(statusText) - length, " \u00B7 ");
    if (static_cast<size_t>(length) < sizeof(statusText)) {
      snprintf(statusText + length, sizeof(statusText) - length, tr(STR_DAILY_READ_COUNT), read, saved);
    }
  }
  renderer.drawText(UI_10_FONT_ID, safe.x + (safe.width - renderer.getTextWidth(UI_10_FONT_ID, statusText)) / 2,
                    statusTop, statusText);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
