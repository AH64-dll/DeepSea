#pragma once

#include "moderngekko/game.hpp"
#include "moderngekko/runtime.hpp"

#include <string>

namespace moderngekko::frontend {
std::string CompatibilityFingerprint(const RuntimeConfig &config,
                                     const GameMetadata &game);
}
