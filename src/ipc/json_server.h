#pragma once

#include <filesystem>

namespace raid::ipc {
int serve(const std::filesystem::path& root);
}  // namespace raid::ipc
