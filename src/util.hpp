// Small platform and string helpers. All strings are UTF-8; on Windows the
// conversion to UTF-16 happens only at the OS boundary (paths, console).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace util {

fs::path pathFromUtf8(const std::string& s);
std::string pathToUtf8(const fs::path& p);
#ifdef _WIN32
std::wstring utf8ToWide(std::string_view s);
std::string wideToUtf8(std::wstring_view w);
#endif
std::string utf16ToUtf8(const uint16_t* s, size_t n);
void appendUtf8(std::string& out, uint32_t codepoint);

bool readFile(const fs::path& p, std::string& out, std::string& err);
bool writeFile(const fs::path& p, std::string_view data, std::string& err);

bool isSpace(char c);
std::string_view trim(std::string_view s);
std::string toLower(std::string_view s);
bool iequals(std::string_view a, std::string_view b);
bool istartsWith(std::string_view s, std::string_view prefix);
bool iendsWith(std::string_view s, std::string_view suffix);
size_t ifind(std::string_view hay, std::string_view needle, size_t from = 0);

std::string formatBytes(uint64_t n);   // "1.79 MB"
std::string formatCount(uint64_t n);   // "52,946"
std::string formatNumber(double v, int decimals);  // fixed decimals, trailing zeros trimmed
std::string htmlEscape(std::string_view s);
std::string urlEncodePath(std::string_view relPath);
std::string localTimestamp();          // "2026-10-30 12:34"

// ---- console -------------------------------------------------------------
enum class Color { Normal, Green, Yellow, Red, Cyan, Gray, White };
void consoleInit();
void print(std::string_view utf8, Color c = Color::Normal);
// True when this process got its own console window, i.e. it was started by
// dropping files on the exe or double-clicking it in Explorer.
bool ownsConsoleWindow();
int waitForKey();  // returns the lower-cased key that was pressed
void openWithDefaultApp(const fs::path& p);

}  // namespace util
