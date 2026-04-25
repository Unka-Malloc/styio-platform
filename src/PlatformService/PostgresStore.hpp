#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace spio::platform
{

struct SqlMigration
{
  std::string id;
  std::string sql;
};

bool LooksLikePostgresDsn(std::string_view value);
std::vector<SqlMigration> CloudKernelMigrations();
std::string ClaimJobSql();
std::string CompleteJobSql();

}  // namespace spio::platform
