// MenuSystem.cpp — navigation + rendering over a static nested MenuItem tree.
#include "MenuSystem.h"
#include <cstdio>

namespace mmi {

MenuSystem::MenuSystem(const MenuItem& root) {
  stack_[0] = &root;
  index_[0] = 0;
  depth_ = 0;
}

void MenuSystem::reset() { depth_ = 0; index_[0] = 0; }
void MenuSystem::toRoot() { depth_ = 0; }

const MenuItem& MenuSystem::level() const { return *stack_[depth_]; }

const MenuItem& MenuSystem::current() const {
  const MenuItem& lvl = level();
  return lvl.children[index_[depth_]];
}

void MenuSystem::scrollDown() {
  const MenuItem& lvl = level();
  if (lvl.childCount == 0) return;
  index_[depth_] = static_cast<uint8_t>((index_[depth_] + 1) % lvl.childCount);
}

void MenuSystem::scrollUp() {
  const MenuItem& lvl = level();
  if (lvl.childCount == 0) return;
  index_[depth_] = static_cast<uint8_t>((index_[depth_] + lvl.childCount - 1) % lvl.childCount);
}

void MenuSystem::back() {
  if (depth_ > 0) depth_--;
}

void MenuSystem::select() {
  const MenuItem& item = current();
  if (item.children && item.childCount > 0) {
    if (depth_ + 1 < kMaxDepth) {
      depth_++;
      stack_[depth_] = &item;
      index_[depth_] = 0;
    }
  } else if (onSelect_) {
    onSelect_(item.id);
  }
}

void MenuSystem::openTopLevel(uint8_t index) {
  depth_ = 0;
  const MenuItem& root = *stack_[0];
  if (index < root.childCount) {
    index_[0] = index;
    select(); // descend into it
  }
}

void MenuSystem::render(IDisplay& d) const {
  d.beginFullScreen(true);
  const MenuItem& lvl = level();
  // Font byte (VAGFISWriter): 0x21 = standard, positive, centered.
  d.drawText(0, 0, kFontCentered, lvl.label);
  // Simple scrolling window of items around the highlighted one.
  const int visible = 8;
  int count = lvl.childCount;
  int sel = index_[depth_];
  int start = sel - visible / 2;
  if (start < 0) start = 0;
  if (start > count - visible) start = count - visible < 0 ? 0 : count - visible;
  for (int row = 0; row < visible && start + row < count; ++row) {
    int i = start + row;
    char line[24];
    std::snprintf(line, sizeof(line), "%c%s", i == sel ? '>' : ' ', lvl.children[i].label);
    // Compressed font so long labels (+ the '>' marker) fit the 64px width.
    // The cluster won't render an inverse highlight, so the '>' marks selection.
    d.drawText(0, static_cast<uint8_t>(16 + row * 8),
               kFontCompressedLeft, line);
  }
}

} // namespace mmi
