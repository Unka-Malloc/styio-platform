#pragma once

#include <filesystem>
#include <string>

namespace pafio
{

std::filesystem::path ProjectRoot();
std::filesystem::path CanonicalAbsolutePath(const std::filesystem::path &path);

}  // namespace pafio
