#pragma once

#include "components/themes/lyra/LyraTheme.h"

// Lyra's layout with a print-like finish: square corners, a 2px header rule,
// thick black outlines instead of grey fills for the selection, and a
// single-column outlined Home menu. Matches the Today screen's cards.
namespace DailyMetrics {
constexpr ThemeMetrics values = [] {
  ThemeMetrics m = LyraMetrics::values;
  m.listRowRadius = 0;
  m.listSelectionStyle = LIST_SELECTION_OUTLINE;
  m.headerUnderlineSize = 2;
  m.menuRowHeight = 60;
  m.menuSpacing = 10;
  m.popupFrameThickness = 4;
  m.popupCornerRadius = 0;
  m.controlRadius = 0;
  m.sheetRadius = 0;
  m.capsuleRadius = 0;
  return m;
}();
}  // namespace DailyMetrics

class DailyTheme : public LyraTheme {
 public:
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<std::string(int index)>& buttonLabel,
                      const std::function<UIIcon(int index)>& rowIcon) const override;
};
