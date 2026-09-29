#pragma once

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>

namespace WallpaperEngine::Scripting {

// Imported Workshop scripts retain paths relative to their own asset bundle.
// The scene package stores those assets under <kind>/workshop/<id>/.
template <typename Exists>
std::string resolveWorkshopScriptAssetPath (const std::string& path, std::string_view workshopId,
                                            Exists&& exists) {
    if (exists (path) || workshopId.empty () || workshopId.size () > 20
        || !std::all_of (workshopId.begin (), workshopId.end (), [] (char digit) {
               return digit >= '0' && digit <= '9';
           }))
        return path;

    const std::filesystem::path asset (path);
    if (asset.is_absolute () || asset.has_root_path ()) return path;
    const auto part = asset.begin ();
    if (part == asset.end () || (*part != "models" && *part != "particles" && *part != "sounds"))
        return path;
    const auto next = std::next (part);
    if (next == asset.end () || *next == "workshop") return path;
    for (const auto& component : asset)
        if (component == ".." || component == ".") return path;

    const std::filesystem::path candidate = *part / "workshop" / std::string (workshopId)
        / asset.lexically_relative (*part);
    const auto qualified = candidate.generic_string ();
    return exists (qualified) ? qualified : path;
}

} // namespace WallpaperEngine::Scripting
