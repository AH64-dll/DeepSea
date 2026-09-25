#pragma once

#include <memory>

class BootSessionData;

namespace moderngekko::detail {
void SetExternalUICommon(bool external);
void SetBootSessionData(std::unique_ptr<BootSessionData> boot_session_data);
} // namespace moderngekko::detail
