#include "util.hpp"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <conio.h>
#include <shellapi.h>
#else
#include <unistd.h>
#endif

namespace util {

#ifdef _WIN32
std::wstring utf8ToWide(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string wideToUtf8(std::wstring_view w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

fs::path pathFromUtf8(const std::string& s) { return fs::path(utf8ToWide(s)); }
std::string pathToUtf8(const fs::path& p) { return wideToUtf8(p.wstring()); }
#else
fs::path pathFromUtf8(const std::string& s) { return fs::path(s); }
std::string pathToUtf8(const fs::path& p) { return p.string(); }
#endif

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

std::string utf16ToUtf8(const uint16_t* s, size_t n) {
    std::string out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            ++i;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;  // unpaired surrogate
        }
        appendUtf8(out, c);
    }
    return out;
}

static FILE* openFile(const fs::path& p, bool write) {
#ifdef _WIN32
    return _wfopen(p.c_str(), write ? L"wb" : L"rb");
#else
    return std::fopen(p.c_str(), write ? "wb" : "rb");
#endif
}

bool readFile(const fs::path& p, std::string& out, std::string& err) {
    FILE* f = openFile(p, false);
    if (!f) {
        err = std::strerror(errno);
        return false;
    }
    out.clear();
    char buf[1 << 16];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    bool ok = !std::ferror(f);
    std::fclose(f);
    if (!ok) err = "read error";
    return ok;
}

bool writeFile(const fs::path& p, std::string_view data, std::string& err) {
    FILE* f = openFile(p, true);
    if (!f) {
        err = std::strerror(errno);
        return false;
    }
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = (std::fclose(f) == 0) && ok;
    if (!ok) err = "write error (disk full?)";
    return ok;
}

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

std::string_view trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && isSpace(s[a])) ++a;
    while (b > a && isSpace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string toLower(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = (char)std::tolower((unsigned char)c);
    return r;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}

bool istartsWith(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && iequals(s.substr(0, prefix.size()), prefix);
}

bool iendsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && iequals(s.substr(s.size() - suffix.size()), suffix);
}

size_t ifind(std::string_view hay, std::string_view needle, size_t from) {
    if (needle.empty()) return from <= hay.size() ? from : std::string_view::npos;
    for (size_t i = from; i + needle.size() <= hay.size(); ++i)
        if (iequals(hay.substr(i, needle.size()), needle)) return i;
    return std::string_view::npos;
}

std::string formatBytes(uint64_t n) {
    char buf[32];
    if (n < 1024)
        std::snprintf(buf, sizeof buf, "%llu B", (unsigned long long)n);
    else if (n < 1024 * 1024)
        std::snprintf(buf, sizeof buf, "%.1f KB", n / 1024.0);
    else
        std::snprintf(buf, sizeof buf, "%.2f MB", n / (1024.0 * 1024.0));
    return buf;
}

std::string formatCount(uint64_t n) {
    std::string digits = std::to_string(n), out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return out;
}

std::string formatNumber(double v, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        while (s.back() == '0') s.pop_back();
        if (s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}

std::string htmlEscape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string urlEncodePath(std::string_view relPath) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : relPath) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string localTimestamp() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
    return buf;
}

// ---- console -------------------------------------------------------------

#ifdef _WIN32
static HANDLE g_out = INVALID_HANDLE_VALUE;
static bool g_isConsole = false;
static WORD g_defaultAttr = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;

void consoleInit() {
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    g_isConsole = g_out != INVALID_HANDLE_VALUE && GetConsoleMode(g_out, &mode);
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (g_isConsole && GetConsoleScreenBufferInfo(g_out, &info)) g_defaultAttr = info.wAttributes;
    SetConsoleOutputCP(CP_UTF8);
}

void print(std::string_view s, Color c) {
    if (!g_isConsole) {
        std::fwrite(s.data(), 1, s.size(), stdout);
        std::fflush(stdout);
        return;
    }
    WORD fg = 0;
    switch (c) {
        case Color::Green: fg = FOREGROUND_GREEN | FOREGROUND_INTENSITY; break;
        case Color::Yellow: fg = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY; break;
        case Color::Red: fg = FOREGROUND_RED | FOREGROUND_INTENSITY; break;
        case Color::Cyan: fg = FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY; break;
        case Color::Gray: fg = FOREGROUND_INTENSITY; break;
        case Color::White: fg = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY; break;
        case Color::Normal: break;
    }
    if (fg) SetConsoleTextAttribute(g_out, (WORD)((g_defaultAttr & 0xF0) | fg));
    std::wstring w = utf8ToWide(s);
    DWORD written = 0;
    WriteConsoleW(g_out, w.data(), (DWORD)w.size(), &written, nullptr);
    if (fg) SetConsoleTextAttribute(g_out, g_defaultAttr);
}

bool ownsConsoleWindow() {
    DWORD pids[4];
    return GetConsoleProcessList(pids, 4) == 1;
}

int waitForKey() {
    FlushConsoleInputBuffer(GetStdHandle(STD_INPUT_HANDLE));
    int c = _getch();
    if (c == 0 || c == 0xE0) _getch();  // function/arrow keys send two codes
    return std::tolower(c);
}

void openWithDefaultApp(const fs::path& p) {
    ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
#else
static bool g_color = false;

void consoleInit() { g_color = isatty(1); }

void print(std::string_view s, Color c) {
    const char* code = nullptr;
    switch (c) {
        case Color::Green: code = "\x1b[92m"; break;
        case Color::Yellow: code = "\x1b[93m"; break;
        case Color::Red: code = "\x1b[91m"; break;
        case Color::Cyan: code = "\x1b[96m"; break;
        case Color::Gray: code = "\x1b[90m"; break;
        case Color::White: code = "\x1b[97m"; break;
        case Color::Normal: break;
    }
    if (g_color && code) std::fputs(code, stdout);
    std::fwrite(s.data(), 1, s.size(), stdout);
    if (g_color && code) std::fputs("\x1b[0m", stdout);
    std::fflush(stdout);
}

bool ownsConsoleWindow() { return false; }

int waitForKey() { return std::tolower(std::getchar()); }

void openWithDefaultApp(const fs::path&) {}
#endif

}  // namespace util
