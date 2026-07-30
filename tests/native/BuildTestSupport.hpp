#pragma once

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

namespace fs = std::filesystem;

namespace spio::testsupport
{

inline fs::path MakeTempDir(const std::string &label)
{
  const fs::path root = fs::temp_directory_path() / "spio-native-build-tests" / label;
  fs::remove_all(root);
  fs::create_directories(root);
  return root;
}

inline void WriteFile(const fs::path &path, const std::string &content)
{
  fs::create_directories(path.parent_path());
  std::ofstream out(path);
  ASSERT_TRUE(out.good());
  out << content;
  ASSERT_TRUE(out.good());
}

inline std::string ReadFile(const fs::path &path)
{
  std::ifstream in(path);
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

}  // namespace spio::testsupport
