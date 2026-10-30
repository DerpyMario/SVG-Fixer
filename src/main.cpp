// SVG-Fixer: drop .svg files, folders or .7z archives onto the exe.
// Repaired files go to a new folder next to what was dropped, together with
// a text and an HTML report; the originals are never modified.

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "archive7z.hpp"
#include "report.hpp"
#include "svgfix.hpp"
#include "util.hpp"

#ifndef SVGFIX_VERSION
#define SVGFIX_VERSION "dev"
#endif

namespace {

using util::Color;
using util::print;

const char* const kReportTxt = "SVG-Fixer-Report.txt";
const char* const kReportHtml = "SVG-Fixer-Report.html";

struct InputFile {
    std::string name;   // relative path, UTF-8, '/' separated
    std::string data;
    fs::path source;    // file on disk (empty for archive entries)
    std::string error;  // could not be read / extracted
};

struct Job {
    std::string title;
    std::string source;  // what was dropped, for the report
    fs::path outDir;
    std::vector<InputFile> files;
    std::vector<std::string> notes;
    std::string error;
    bool loose = false;  // individual files rather than a folder or archive
};

struct Settings {
    svgfix::Options fix;
    bool validateOnly = false;
    bool noPause = false;
    bool quiet = false;
    std::string outputDir;
};

std::string u8(const fs::path& p) { return util::pathToUtf8(p); }

std::string slashed(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

void usage() {
    print("SVG-Fixer " SVGFIX_VERSION " - repairs and validates SVG files exported from vectorizer.ai\n\n", Color::White);
    print(
        "Drag and drop .svg files, folders or .7z archives onto SVG-Fixer.exe.\n"
        "Repaired files are written next to what you dropped:\n"
        "  photos.7z      ->  photos_fixed\\\n"
        "  my folder\\     ->  my folder_fixed\\\n"
        "  single files   ->  fixed\\  (in the same folder)\n"
        "with SVG-Fixer-Report.html / .txt describing every problem found.\n\n"
        "Command line: SVG-Fixer [options] <file.svg | folder | archive.7z> ...\n"
        "  -o, --output DIR       write the results into DIR\n"
        "  -n, --validate-only    only check the files, write nothing\n"
        "  -p, --precision N      decimals for vectorizer coordinates (default: automatic, 2 for normal sizes)\n"
        "      --strip-gap-fillers  remove the stroke-only seam fillers (pure filled shapes)\n"
        "  -q, --quiet            one line per file\n"
        "      --no-pause         do not wait for a key press at the end\n"
        "  -h, --help             this help\n");
}

// An archive entry name as a safe relative path (no "..", no absolute
// paths, no characters or device names Windows rejects).
bool safeRelative(const std::string& name, fs::path& out) {
    static const char* const kReserved[] = {"con",  "prn",  "aux",  "nul",  "com1", "com2", "com3", "com4",
                                            "com5", "com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3",
                                            "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    std::vector<std::string> parts;
    std::string part;
    auto flush = [&] {
        if (part == "..") return false;
        if (!part.empty() && part != ".") parts.push_back(part);
        part.clear();
        return true;
    };
    for (char c : name) {
        if (c == '/' || c == '\\') {
            if (!flush()) return false;
            continue;
        }
        part += ((unsigned char)c < 0x20 || std::strchr("<>:\"|?*", c)) ? '_' : c;
    }
    if (!flush() || parts.empty()) return false;
    out.clear();
    for (std::string& p : parts) {
        while (!p.empty() && (p.back() == '.' || p.back() == ' ')) p.pop_back();
        if (p.empty()) p = "_";
        std::string stem = util::toLower(p.substr(0, p.find('.')));
        for (const char* r : kReserved)
            if (stem == r) p = "_" + p;
        out /= util::pathFromUtf8(p);
    }
    return true;
}

void collectFolder(const fs::path& dir, Job& job) {
    std::error_code ec;
    std::vector<fs::path> found;
    fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        std::error_code fe;
        if (it->is_regular_file(fe) && util::iendsWith(u8(it->path().filename()), ".svg")) found.push_back(it->path());
    }
    if (ec) job.notes.push_back("Could not read the whole folder (" + ec.message() + "); some files may be missing.");
    std::sort(found.begin(), found.end());
    for (const fs::path& f : found) {
        InputFile in;
        in.source = f;
        in.name = slashed(u8(f.lexically_relative(dir)));
        util::readFile(f, in.data, in.error);
        job.files.push_back(std::move(in));
    }
}

void collectArchive(const std::string& bytes, Job& job) {
    std::vector<archive7z::Entry> entries;
    if (!archive7z::extract(bytes, entries, job.error)) return;
    std::vector<std::string> skipped;
    for (archive7z::Entry& e : entries) {
        if (e.isDir) continue;
        if (!util::iendsWith(e.name, ".svg")) {
            skipped.push_back(e.name);
            continue;
        }
        InputFile in;
        in.name = e.name;
        in.data = std::move(e.data);
        in.error = e.error;
        job.files.push_back(std::move(in));
    }
    std::sort(job.files.begin(), job.files.end(),
              [](const InputFile& a, const InputFile& b) { return util::toLower(a.name) < util::toLower(b.name); });
    if (!skipped.empty()) {
        std::string note = "Skipped " + std::to_string(skipped.size()) + " file(s) in the archive that are not .svg: ";
        for (size_t i = 0; i < skipped.size() && i < 5; ++i) note += (i ? ", " : "") + skipped[i];
        if (skipped.size() > 5) note += ", ...";
        job.notes.push_back(note);
    }
}

std::string firstSentence(const std::string& s) {
    size_t p = s.find(". ");
    std::string r = p == std::string::npos ? s : s.substr(0, p + 1);
    return r.size() > 160 ? r.substr(0, 157) + "..." : r;
}

void printFile(const svgfix::Report& r, const Settings& st) {
    Color c = Color::Normal;
    switch (r.status) {
        case svgfix::Status::Ok: c = Color::Green; break;
        case svgfix::Status::Fixed: c = Color::Cyan; break;
        case svgfix::Status::Converted: c = Color::Yellow; break;
        case svgfix::Status::Failed: c = Color::Red; break;
    }
    std::string label = std::string("  ") + svgfix::statusLabel(r.status);
    label.resize(11, ' ');
    print(label, c);
    print(r.name + "\n");
    if (st.quiet) return;
    std::string detail;
    if (r.status == svgfix::Status::Failed || r.status == svgfix::Status::Converted) {
        // the problem that decided the status: the unfixed error, if any
        for (const svgfix::Issue& i : r.issues) {
            if (i.severity == svgfix::Severity::Error && (!i.fixed || detail.empty())) {
                detail = firstSentence(i.text);
                if (!i.fixed) break;
            }
        }
    } else {
        detail = r.kind;
        std::string stats = report::statsLine(r);
        if (!stats.empty()) detail += " | " + stats;
    }
    if (!detail.empty()) print(std::string(11, ' ') + detail + "\n", Color::Gray);
    if (st.validateOnly) {
        for (const svgfix::Issue& i : r.issues) {
            std::string tag = svgfix::severityLabel(i.severity);
            print(std::string(11, ' ') + "- " + tag + ": " + i.text + "\n",
                  i.severity == svgfix::Severity::Error ? Color::Yellow : Color::Normal);
        }
    }
}

int run(const std::vector<std::string>& args) {
    Settings st;
    std::vector<std::string> inputs;
    for (size_t i = 0; i < args.size(); ++i) {
        std::string a = args[i], value;
        size_t eq = a.find('=');
        if (a.rfind("--", 0) == 0 && eq != std::string::npos) {
            value = a.substr(eq + 1);
            a = a.substr(0, eq);
        }
        auto next = [&](std::string& out) {
            if (!value.empty()) {
                out = value;
                return true;
            }
            if (i + 1 >= args.size()) return false;
            out = args[++i];
            return true;
        };
        std::string v;
        if (a == "-h" || a == "--help" || a == "/?") {
            usage();
            return 0;
        } else if (a == "--version") {
            print("SVG-Fixer " SVGFIX_VERSION "\n");
            return 0;
        } else if (a == "-n" || a == "--validate-only" || a == "--validate") {
            st.validateOnly = true;
        } else if (a == "--strip-gap-fillers") {
            st.fix.stripGapFillers = true;
        } else if (a == "--no-pause") {
            st.noPause = true;
        } else if (a == "-q" || a == "--quiet") {
            st.quiet = true;
        } else if ((a == "-p" || a == "--precision") && next(v)) {
            st.fix.precision = std::min(8, std::max(0, std::atoi(v.c_str())));
        } else if ((a == "-o" || a == "--output") && next(v)) {
            st.outputDir = v;
        } else if (a.size() > 1 && a[0] == '-') {
            std::error_code ec;
            if (!fs::exists(util::pathFromUtf8(args[i]), ec)) {
                print("Unknown option: " + args[i] + "\n\n", Color::Red);
                usage();
                return 2;
            }
            inputs.push_back(args[i]);
        } else {
            inputs.push_back(args[i]);
        }
    }

    print("SVG-Fixer " SVGFIX_VERSION, Color::White);
    print(" - repairs and validates vectorizer.ai SVG exports\n");
    if (inputs.empty()) {
        print("\n");
        usage();
        if (util::ownsConsoleWindow() && !st.noPause) {  // double-clicked: keep the help readable
            print("\nPress any key to close this window.");
            util::waitForKey();
        }
        return 2;
    }

    std::vector<Job> jobs;
    std::map<std::string, size_t> looseJobs;
    bool inputErrors = false;
    for (const std::string& in : inputs) {
        std::error_code ec;
        fs::path p = fs::absolute(util::pathFromUtf8(in), ec);
        if (ec) p = util::pathFromUtf8(in);
        p.make_preferred();
        if (!fs::exists(p, ec)) {
            print("\nNot found: " + in + "\n", Color::Red);
            inputErrors = true;
            continue;
        }
        if (fs::is_directory(p, ec)) {
            if (!p.has_filename()) p = p.parent_path();  // "folder\" -> "folder"
            Job job;
            job.title = "Folder: " + u8(p);
            job.source = u8(p);
            job.outDir = p.parent_path() / util::pathFromUtf8(u8(p.filename()) + "_fixed");
            collectFolder(p, job);
            jobs.push_back(std::move(job));
            continue;
        }
        std::string bytes, err;
        if (!util::readFile(p, bytes, err)) {
            print("\nCannot read " + in + ": " + err + "\n", Color::Red);
            inputErrors = true;
            continue;
        }
        if (archive7z::isArchive(bytes)) {
            Job job;
            job.title = "Archive: " + u8(p);
            job.source = u8(p);
            job.outDir = p.parent_path() / util::pathFromUtf8(u8(p.stem()) + "_fixed");
            collectArchive(bytes, job);
            jobs.push_back(std::move(job));
            continue;
        }
        std::string key = u8(p.parent_path());
        auto it = looseJobs.find(key);
        if (it == looseJobs.end()) {
            Job job;
            job.title = "Files in: " + key;
            job.source = key;
            job.outDir = p.parent_path() / "fixed";
            job.loose = true;
            it = looseJobs.emplace(key, jobs.size()).first;
            jobs.push_back(std::move(job));
        }
        InputFile f;
        f.source = p;
        f.name = u8(p.filename());
        f.data = std::move(bytes);
        jobs[it->second].files.push_back(std::move(f));
    }
    if (!st.outputDir.empty()) {
        fs::path base = util::pathFromUtf8(st.outputDir);
        for (Job& j : jobs) j.outDir = j.loose ? base : base / j.outDir.filename();
    }

    size_t total = 0, failed = 0;
    std::vector<svgfix::Report> all;
    fs::path lastReport;
    for (Job& job : jobs) {
        print("\n" + job.title + "\n", Color::White);
        if (!job.error.empty()) {
            print("  Cannot process it: " + job.error + "\n", Color::Red);
            inputErrors = true;
            continue;
        }
        if (job.files.empty()) {
            print("  No .svg files found.\n", Color::Yellow);
            for (const std::string& n : job.notes) print("  " + n + "\n", Color::Gray);
            continue;
        }
        std::error_code ec;
        if (!st.validateOnly) {
            fs::create_directories(job.outDir, ec);
            if (ec) {
                print("  Cannot create the output folder " + u8(job.outDir) + ": " + ec.message() + "\n", Color::Red);
                inputErrors = true;
                continue;
            }
        }
        std::vector<svgfix::Report> reports;
        for (InputFile& f : job.files) {
            svgfix::Result r;
            if (!f.error.empty()) {
                r.report.name = f.name;
                r.report.kind = "unreadable";
                r.report.status = svgfix::Status::Failed;
                r.report.issues.push_back({svgfix::Severity::Error, false, "Could not read the file: " + f.error});
            } else {
                r = svgfix::process(f.name, f.data, st.fix);
            }
            if (!st.validateOnly && r.report.status != svgfix::Status::Failed) {
                fs::path rel;
                std::string err;
                if (!safeRelative(f.name, rel)) {
                    err = "unsafe file name in the archive";
                } else {
                    if (r.extension != ".svg") rel.replace_extension(util::pathFromUtf8(r.extension));
                    fs::path out = job.outDir / rel;
                    std::error_code eq;
                    if (!f.source.empty() && fs::equivalent(out, f.source, eq)) {
                        err = "the output would overwrite the original file";
                    } else {
                        fs::create_directories(out.parent_path(), ec);
                        if (util::writeFile(out, r.data, err)) r.report.outputName = slashed(u8(rel));
                    }
                }
                if (r.report.outputName.empty()) {
                    r.report.issues.push_back({svgfix::Severity::Error, false, "Could not save the result: " + err});
                    r.report.status = svgfix::Status::Failed;
                }
            }
            printFile(r.report, st);
            if (r.report.status == svgfix::Status::Failed) ++failed;
            ++total;
            reports.push_back(std::move(r.report));
            f.data = std::string();
        }
        if (!st.validateOnly) {
            report::Header h{job.source, u8(job.outDir), util::localTimestamp(), job.notes};
            std::string err;
            fs::path txt = job.outDir / kReportTxt, html = job.outDir / kReportHtml;
            if (!util::writeFile(txt, report::text(h, reports), err) || !util::writeFile(html, report::html(h, reports), err))
                print("  Could not write the report: " + err + "\n", Color::Red);
            else
                lastReport = html;
            print("\n  " + report::summaryLine(reports) + "\n", Color::White);
            print("  Output: " + u8(job.outDir) + "\n");
            print("  Report: " + u8(html) + "\n");
        } else {
            print("\n  " + report::summaryLine(reports) + " (validate-only: nothing written)\n", Color::White);
        }
        for (const std::string& n : job.notes) print("  " + n + "\n", Color::Gray);
        size_t pages = 0;
        for (const svgfix::Report& r : reports) pages += r.page.isVectorizer && r.status == svgfix::Status::Failed;
        if (pages)
            print("  " + std::to_string(pages) +
                      " file(s) are vectorizer.ai web pages saved as .svg: they hold no vector data. The report lists "
                      "the page to export each one from again.\n",
                  Color::Yellow);
        all.insert(all.end(), reports.begin(), reports.end());
    }

    if (jobs.size() > 1) print("\nAll done: " + std::to_string(total) + " file(s) - " + report::summaryLine(all) + "\n", Color::White);

    if (util::ownsConsoleWindow() && !st.noPause) {
        if (!lastReport.empty()) {
            print("\nPress R to open the report, or any other key to close this window.");
            if (util::waitForKey() == 'r') util::openWithDefaultApp(lastReport);
        } else {
            print("\nPress any key to close this window.");
            util::waitForKey();
        }
    }
    if (inputErrors || failed) return 1;
    return 0;
}

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    util::consoleInit();
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(util::wideToUtf8(argv[i]));
    return run(args);
}
#else
int main(int argc, char** argv) {
    util::consoleInit();
    return run(std::vector<std::string>(argv + 1, argv + argc));
}
#endif
