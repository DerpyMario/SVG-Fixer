// Reading .7z archives (LZMA SDK).
#pragma once

#include <string>
#include <vector>

namespace archive7z {

struct Entry {
    std::string name;   // UTF-8, '/' separated, as stored in the archive
    bool isDir = false;
    std::string data;   // file contents
    std::string error;  // set when this entry could not be extracted
};

bool isArchive(const std::string& bytes);

// Extracts every entry into memory. Returns false (with `error`) when the
// archive itself cannot be opened.
bool extract(const std::string& bytes, std::vector<Entry>& entries, std::string& error);

}  // namespace archive7z
