#pragma once

#include "metadata/sqlite_database.h"

namespace raid::metadata {
std::string initializeSchema(Database& db);
}  // namespace raid::metadata
