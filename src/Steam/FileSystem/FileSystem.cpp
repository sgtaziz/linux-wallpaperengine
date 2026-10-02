#include "FileSystem.h"
#include "WallpaperEngine/Logging/Log.h"
#include <climits>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <fstream>
#include <regex>
#include <set>
#include <sys/stat.h>

std::vector<std::string> appDirectoryPaths = {
    ".steam/steam/steamapps/common",
    ".local/share/Steam/steamapps/common",
    ".var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common",
    "snap/steam/common/.local/share/Steam/steamapps/common",
};

std::vector<std::string> workshopDirectoryPaths = {
    ".local/share/Steam/steamapps/workshop/content",
    ".steam/steam/steamapps/workshop/content",
    ".var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/workshop/content",
    "snap/steam/common/.local/share/Steam/steamapps/workshop/content",
};

std::filesystem::path detectHomepath () {
    char* home = getenv ("HOME");

    if (home == nullptr) {
	sLog.exception ("Cannot find home directory for the current user");
    }

    const std::filesystem::path path = home;

    if (!std::filesystem::is_directory (path)) {
	sLog.exception ("Cannot find home directory for current user, ", home, " is not a directory");
    }

    return path;
}

std::optional<std::filesystem::path> Steam::FileSystem::findWorkshopDirectory (int appID, const std::string& contentID) {
    auto homepath = detectHomepath ();

    for (const auto& current : workshopDirectoryPaths) {
	auto currentpath = std::filesystem::path (homepath) / current / std::to_string (appID) / contentID;

	if (!std::filesystem::exists (currentpath) || !std::filesystem::is_directory (currentpath)) {
	    continue;
	}

	return currentpath;
    }
    // Workshop content can live in any library registered with Steam.
    std::set<std::filesystem::path> libraries;
    const std::regex pathLine (R"vdf(^\s*"path"\s*"([^"]+)"\s*$)vdf");
    for (const auto& current : appDirectoryPaths) {
        const auto steamapps = (homepath / current).parent_path ();
        for (const auto& metadata : {steamapps / "libraryfolders.vdf",
                                    steamapps.parent_path () / "config/libraryfolders.vdf"}) {
            std::ifstream input (metadata);
            for (std::string line; std::getline (input, line);) {
                std::smatch match;
                if (std::regex_match (line, match, pathLine)) libraries.emplace (match[1].str ());
            }
        }
    }
    for (const auto& library : libraries) {
        const auto path = library / "steamapps/workshop/content" / std::to_string (appID) / contentID;
        if (std::filesystem::is_directory (path)) return path;
    }
    return std::nullopt;
}

std::filesystem::path Steam::FileSystem::workshopDirectory (int appID, const std::string& contentID) {
    if (const auto path = findWorkshopDirectory (appID, contentID)) return *path;

    sLog.exception ("Cannot find workshop directory for steam app ", appID, " and content ", contentID);
}

std::filesystem::path Steam::FileSystem::appDirectory (const std::string& appDirectory, const std::string& path) {
    auto homepath = detectHomepath ();

    for (const auto& current : appDirectoryPaths) {
	auto currentpath = std::filesystem::path (homepath) / current / appDirectory / path;

	if (!std::filesystem::exists (currentpath) || !std::filesystem::is_directory (currentpath)) {
	    continue;
	}

	return currentpath;
    }

    sLog.exception ("Cannot find directory for steam app ", appDirectory, ": ", path);
}
