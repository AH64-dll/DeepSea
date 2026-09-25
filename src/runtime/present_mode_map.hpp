#pragma once

// Explicit translation between the three present-mode enums in play:
//
//   moderngekko::PresentMode        {Fifo=0,     Mailbox=1,     Immediate=2}
//   moderngekko::frontend::PresentMode {Fifo=0,  Mailbox=1,     Immediate=2}
//   Config::PresentMode (Dolphin)   {Immediate=0,Fifo=1,        Mailbox=2}
//
// The Dolphin enum has a DIFFERENT ordering, so raw static_cast<int> hops
// across silently request the wrong swapchain mode (every CLI value landed
// one permutation off; see phase3-presentmode-g2g4.md §2). Always go through
// these helpers; the static_asserts fail the build if any ordering changes.

#include "Core/Config/GraphicsSettings.h"
#include "moderngekko/runtime.hpp"

namespace moderngekko::detail
{
static_assert(static_cast<int>(PresentMode::Fifo) == 0 &&
                  static_cast<int>(PresentMode::Mailbox) == 1 &&
                  static_cast<int>(PresentMode::Immediate) == 2,
              "moderngekko::PresentMode ordering changed; revisit mapping");
static_assert(static_cast<int>(Config::PresentMode::Immediate) == 0 &&
                  static_cast<int>(Config::PresentMode::Fifo) == 1 &&
                  static_cast<int>(Config::PresentMode::Mailbox) == 2,
              "Config::PresentMode values changed; revisit mapping");

inline Config::PresentMode ToDolphinPresentMode(PresentMode mode)
{
  switch (mode)
  {
  case PresentMode::Fifo:
    return Config::PresentMode::Fifo;
  case PresentMode::Mailbox:
    return Config::PresentMode::Mailbox;
  case PresentMode::Immediate:
    return Config::PresentMode::Immediate;
  }
  return Config::PresentMode::Fifo;
}
}  // namespace moderngekko::detail
