#include <aurora/aurora.h>
#include <dolphin/os.h>

namespace aurora {

AuroraConfig g_config{};

void log_internal(AuroraLogLevel, const char*, const char*, unsigned int) noexcept {}

} // namespace aurora
