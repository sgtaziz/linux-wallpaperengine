#pragma once

#include <filesystem>
#include <string>
#include <optional>

namespace Steam::FileSystem {
std::filesystem::path workshopDirectory (int appID, const std::string& contentID);
std::optional<std::filesystem::path> findWorkshopDirectory (int appID, const std::string& contentID);
std::filesystem::path appDirectory (const std::string& appDirectory, const std::string& path);
} // namespace Steam::FileSystem
