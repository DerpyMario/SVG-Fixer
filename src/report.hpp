// Text and HTML reports written next to the repaired files.
#pragma once

#include <string>
#include <vector>

#include "svgfix.hpp"

namespace report {

struct Header {
    std::string source;      // what was dropped (UTF-8 path)
    std::string outputDir;   // where the results went (UTF-8 path)
    std::string date;
    std::vector<std::string> notes;  // e.g. archive entries that were skipped
};

std::string text(const Header& h, const std::vector<svgfix::Report>& files);
std::string html(const Header& h, const std::vector<svgfix::Report>& files);

// Reports ordered for reading: fixed, ok, converted, failed; then by name.
std::vector<svgfix::Report> sorted(const std::vector<svgfix::Report>& files);

// "2 fixed, 1 ok, 16 failed"
std::string summaryLine(const std::vector<svgfix::Report>& files);

// "2374 x 1325 px | 5,022 shapes (897 filled, 4,125 gap fillers) | 1.79 MB -> 612.3 KB"
std::string statsLine(const svgfix::Report& r);

}  // namespace report
