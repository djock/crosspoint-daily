#pragma once

#include <I18n.h>

#include <cstddef>

// Saved Today passages: /Daily/YYYY-MM-DD-<kind>.txt. A passage is read once
// its reader turns past the last page, recorded as an empty sibling marker
// /Daily/YYYY-MM-DD-<kind>.done (new dates start unread by construction).
namespace daily_passages {

constexpr int KIND_COUNT = 2;
constexpr const char* KINDS[KIND_COUNT] = {"daily_dad", "daily_stoic"};
constexpr StrId LABELS[KIND_COUNT] = {StrId::STR_DAILY_DAD, StrId::STR_DAILY_STOIC};

// Kind index of a saved passage path, or -1 when the path is not one.
int kindOf(const char* path);
// Writes the passage path for the same date and another kind into out.
bool siblingPath(const char* path, int kind, char* out, size_t outSize);
bool isRead(const char* path);
void markRead(const char* path);

}  // namespace daily_passages
