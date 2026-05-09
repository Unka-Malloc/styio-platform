#pragma once

#include <filesystem>
#include <string>

namespace spio
{

std::filesystem::path ProjectRoot();
std::filesystem::path CanonicalAbsolutePath(const std::filesystem::path &path);

}  // namespace spio
