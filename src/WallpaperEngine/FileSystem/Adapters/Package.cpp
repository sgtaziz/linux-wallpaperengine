#include <fstream>
#include <memory>

#include "Package.h"

#include "WallpaperEngine/Assets/AssetLoadException.h"
#include "WallpaperEngine/Data/Parsers/PackageParser.h"
#include "WallpaperEngine/Data/Utils/BinaryReader.h"
#include "WallpaperEngine/Data/Utils/MemoryStream.h"

#include <algorithm>
#include <string_view>

using namespace WallpaperEngine::FileSystem;
using namespace WallpaperEngine::FileSystem::Adapters;

namespace {
bool equalAsciiCaseInsensitive (std::string_view left, std::string_view right) {
    if (left.size () != right.size ()) return false;
    for (size_t i = 0; i < left.size (); ++i) {
        const auto lower = [] (unsigned char c) {
            return c >= 'A' && c <= 'Z' ? static_cast<unsigned char> (c - 'A' + 'a') : c;
        };
        if (lower (static_cast<unsigned char> (left[i]))
            != lower (static_cast<unsigned char> (right[i]))) return false;
    }
    return true;
}

const FileEntry* findEntry (const Package& package, const std::filesystem::path& path) {
    const std::string requested = path.generic_string ();
    // An exact package name always wins. Some Workshop assets authored on
    // Windows use a different ASCII case in the JSON reference and archive.
    for (const auto& file : package.files)
        if (file->filename == requested) return file.get ();

    const FileEntry* fallback = nullptr;
    for (const auto& file : package.files) {
        if (!equalAsciiCaseInsensitive (file->filename, requested)) continue;
        // If two archive names differ only by case, do not choose an
        // arbitrary entry for an inexact reference.
        if (fallback) return nullptr;
        fallback = file.get ();
    }
    return fallback;
}
}

ReadStreamSharedPtr PackageAdapter::open (const std::filesystem::path& path) const {
    const auto* file = findEntry (*this->package, path);
    if (!file) {
	throw std::filesystem::filesystem_error ("Cannot find file", path, std::error_code ());
    }

    // read file into memory
    auto buffer = std::make_unique<char[]> (file->length);

    // go to the file's position and read into the buffer
    this->package->file->base ().seekg (file->offset + this->package->baseOffset, std::ios::beg);
    this->package->file->next (buffer.get (), file->length);

    // create a memory stream and return that
    return std::make_shared<MemoryStream> (std::move (buffer), file->length);
}

bool PackageAdapter::exists (const std::filesystem::path& path) const {
    return findEntry (*this->package, path) != nullptr;
}

std::filesystem::path PackageAdapter::physicalPath (const std::filesystem::path& path) const {
    throw std::filesystem::filesystem_error ("Package adapter does not support realpath", path, std::error_code ());
}

bool PackageFactory::handlesMountpoint (const std::filesystem::path& path) const {
    try {
	const auto finalpath = std::filesystem::canonical (path);
	const auto status = std::filesystem::status (finalpath);

	return std::filesystem::exists (finalpath) && std::filesystem::is_regular_file (status)
	    && finalpath.extension () == ".pkg";
    } catch (std::filesystem::filesystem_error&) {
	return false;
    }
}

AdapterSharedPtr PackageFactory::create (const std::filesystem::path& path) const {
    const auto stream = std::make_shared<std::ifstream> (path, std::ios::binary);
    auto package = Data::Parsers::PackageParser::parse (stream);

    return std::make_unique<PackageAdapter> (std::move (package));
}
