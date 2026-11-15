#pragma once

#include <filesystem>

namespace bwara {

void publishRevision(const std::filesystem::path& marker, unsigned long long revision);
}
