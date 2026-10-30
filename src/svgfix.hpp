// Classifies, validates and repairs one SVG file (as exported from
// vectorizer.ai by a canvas-capturing userscript, or any other SVG).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace svgfix {

enum class Severity { Info, Warning, Error };

enum class Status {
    Ok,         // valid; written unchanged
    Fixed,      // problems were repaired
    Converted,  // not an SVG but a recognised image; saved with its real extension
    Failed,     // cannot be repaired (nothing written)
};

struct Issue {
    Severity severity = Severity::Info;
    bool fixed = false;  // repaired in the output
    std::string text;
};

// What could be read from a saved web page instead of an SVG.
struct PageInfo {
    bool isVectorizer = false;
    std::string title;
    std::string sourceImage;  // original upload, e.g. "photo.jpeg"
    std::string imageSize;    // "2752 x 1536"
    std::string url;          // result page, where the SVG can be exported again
};

struct Report {
    std::string name;        // path inside the input (UTF-8, '/' separated)
    std::string outputName;  // written file, relative to the output folder
    std::string kind;        // what the file turned out to be
    Status status = Status::Ok;
    std::vector<Issue> issues;
    uint64_t bytesIn = 0, bytesOut = 0;
    double width = 0, height = 0;  // canvas size in px (0 = unknown)
    size_t shapes = 0;             // drawable elements
    size_t fills = 0;              // filled paths (capture exports)
    size_t gapFillers = 0;         // stroke-only seam fillers (capture exports)
    bool verified = false;         // output re-parsed and validated cleanly
    PageInfo page;
};

struct Options {
    int precision = -1;            // decimals for capture exports; -1 = automatic
    bool stripGapFillers = false;  // drop the stroke-only seam-filler paths
};

struct Result {
    Report report;
    std::string data;       // bytes to write (empty when Failed)
    std::string extension;  // ".svg", or the real extension of a converted file
};

Result process(const std::string& name, const std::string& bytes, const Options& opt);

const char* statusLabel(Status s);
const char* severityLabel(Severity s);

}  // namespace svgfix
