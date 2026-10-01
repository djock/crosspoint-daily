#include "DailyTheme.h"

#include <GfxRenderer.h>

#include <string>
#include <utility>
#include <vector>

#include "components/icons/blocks.h"
#include "components/icons/book.h"
#include "components/icons/bookmark.h"
#include "components/icons/folder.h"
#include "components/icons/hotspot.h"
#include "components/icons/library.h"
#include "components/icons/recent.h"
#include "components/icons/settings2.h"
#include "components/icons/transfer.h"
#include "components/icons/wifi.h"
#include "fontIds.h"

namespace {
constexpr int MENU_ICON_SIZE = 32;
constexpr int MENU_TEXT_INSET = 16;
constexpr int MENU_ICON_GAP = 12;

// Same icon set as Lyra's menu (its mapping is file-local there).
const uint8_t* iconForName(const UIIcon icon) {
  switch (icon) {
    case UIIcon::Folder:
      return FolderIcon;
    case UIIcon::Book:
      return BookIcon;
    case UIIcon::Recent:
      return RecentIcon;
    case UIIcon::Settings:
      return Settings2Icon;
    case UIIcon::Transfer:
      return TransferIcon;
    case UIIcon::Library:
      return LibraryIcon;
    case UIIcon::Wifi:
      return WifiIcon;
    case UIIcon::Hotspot:
      return HotspotIcon;
    case UIIcon::Bookmark:
      return BookmarkIcon;
    case UIIcon::Blocks:
      return BlocksIcon;
    default:
      return nullptr;
  }
}
}  // namespace

void DailyTheme::drawRecentBookCover(GfxRenderer& renderer, const Rect rect, const std::vector<RecentBook>& recentBooks,
                                     const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                     bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  // Lyra draws the cover and text; drawn as unselected it skips its gray
  // selection wash, and the card frame below marks the selection instead.
  LyraTheme::drawRecentBookCover(renderer, rect, recentBooks, -1, coverRendered, coverBufferStored, bufferRestored,
                                 std::move(storeCoverBuffer));
  if (recentBooks.empty()) return;
  const auto& metrics = DailyMetrics::values;
  const bool selected = selectorIndex == 0;
  renderer.drawRect(rect.x + metrics.contentSidePadding, rect.y, rect.width - 2 * metrics.contentSidePadding,
                    rect.height, selected ? LIST_SELECTION_OUTLINE_WIDTH : 1, true);
}

void DailyTheme::drawButtonMenu(GfxRenderer& renderer, const Rect rect, const int buttonCount, const int selectedIndex,
                                const std::function<std::string(int index)>& buttonLabel,
                                const std::function<UIIcon(int index)>& rowIcon) const {
  const auto& metrics = DailyMetrics::values;
  const int rowWidth = rect.width - metrics.contentSidePadding * 2;
  const int lineHeight = renderer.getLineHeight(NOTOSANS_16_FONT_ID);
  for (int i = 0; i < buttonCount; ++i) {
    const int rowX = rect.x + metrics.contentSidePadding;
    const int rowY = rect.y + i * (metrics.menuRowHeight + metrics.menuSpacing);
    const bool selected = selectedIndex == i;
    renderer.drawRect(rowX, rowY, rowWidth, metrics.menuRowHeight, selected ? LIST_SELECTION_OUTLINE_WIDTH : 1, true);

    int textX = rowX + MENU_TEXT_INSET;
    if (rowIcon != nullptr) {
      if (const uint8_t* icon = iconForName(rowIcon(i))) {
        renderer.drawIcon(icon, textX, rowY + (metrics.menuRowHeight - MENU_ICON_SIZE) / 2, MENU_ICON_SIZE);
        textX += MENU_ICON_SIZE + MENU_ICON_GAP;
      }
    }
    const std::string label = buttonLabel(i);
    renderer.drawText(NOTOSANS_16_FONT_ID, textX, rowY + (metrics.menuRowHeight - lineHeight) / 2, label.c_str(), true,
                      EpdFontFamily::BOLD);
  }
}
