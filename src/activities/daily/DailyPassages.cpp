#include "DailyPassages.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>

namespace daily_passages {
namespace {
constexpr const char* PREFIX = "/Daily/";
constexpr size_t PREFIX_LENGTH = 7;
constexpr size_t DATE_LENGTH = 10;
constexpr const char* EXTENSION = ".txt";
constexpr size_t EXTENSION_LENGTH = 4;

// "/Daily/YYYY-MM-DD-<kind>.done" for a valid passage path.
bool markerPath(const char* path, char* out, const size_t outSize) {
  if (kindOf(path) < 0) return false;
  const size_t stem = strlen(path) - EXTENSION_LENGTH;
  const int written = snprintf(out, outSize, "%.*s.done", static_cast<int>(stem), path);
  return written > 0 && static_cast<size_t>(written) < outSize;
}
}  // namespace

int kindOf(const char* path) {
  if (!path || strncmp(path, PREFIX, PREFIX_LENGTH) != 0) return -1;
  const char* rest = path + PREFIX_LENGTH;
  if (strlen(rest) <= DATE_LENGTH + 1 || rest[DATE_LENGTH] != '-') return -1;
  const char* kind = rest + DATE_LENGTH + 1;
  for (int i = 0; i < KIND_COUNT; ++i) {
    const size_t length = strlen(KINDS[i]);
    if (strncmp(kind, KINDS[i], length) == 0 && strcmp(kind + length, EXTENSION) == 0) return i;
  }
  return -1;
}

bool siblingPath(const char* path, const int kind, char* out, const size_t outSize) {
  if (kindOf(path) < 0 || kind < 0 || kind >= KIND_COUNT) return false;
  const int written = snprintf(out, outSize, "%.*s%s%s", static_cast<int>(PREFIX_LENGTH + DATE_LENGTH + 1), path,
                               KINDS[kind], EXTENSION);
  return written > 0 && static_cast<size_t>(written) < outSize;
}

bool isRead(const char* path) {
  char marker[96];
  return markerPath(path, marker, sizeof(marker)) && Storage.exists(marker);
}

void markRead(const char* path) {
  char marker[96];
  if (!markerPath(path, marker, sizeof(marker)) || Storage.exists(marker)) return;
  HalFile file;
  if (!Storage.openFileForWrite("DAILY", marker, file)) LOG_ERR("DAILY", "Could not mark read: %s", path);
}

}  // namespace daily_passages
