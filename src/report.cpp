#include "report.hpp"

#include <algorithm>

#include "util.hpp"

#ifndef SVGFIX_VERSION
#define SVGFIX_VERSION "dev"
#endif

namespace report {

using svgfix::Report;
using svgfix::Severity;
using svgfix::Status;

namespace {

int rank(Status s) {
    switch (s) {
        case Status::Fixed: return 0;
        case Status::Ok: return 1;
        case Status::Converted: return 2;
        case Status::Failed: return 3;
    }
    return 4;
}

const char* cssClass(Status s) {
    switch (s) {
        case Status::Fixed: return "fixed";
        case Status::Ok: return "ok";
        case Status::Converted: return "conv";
        case Status::Failed: return "failed";
    }
    return "";
}

std::string issueTag(const svgfix::Issue& i) {
    std::string t = svgfix::severityLabel(i.severity);
    if (i.fixed) t += ", fixed";
    return t;
}

}  // namespace

std::vector<Report> sorted(const std::vector<Report>& files) {
    std::vector<Report> v = files;
    std::stable_sort(v.begin(), v.end(), [](const Report& a, const Report& b) {
        if (rank(a.status) != rank(b.status)) return rank(a.status) < rank(b.status);
        return util::toLower(a.name) < util::toLower(b.name);
    });
    return v;
}

std::string summaryLine(const std::vector<Report>& files) {
    size_t n[4] = {0, 0, 0, 0};
    for (const Report& r : files) n[rank(r.status)]++;
    std::string s;
    auto part = [&](size_t count, const char* label) {
        if (!count) return;
        if (!s.empty()) s += ", ";
        s += std::to_string(count) + " " + label;
    };
    part(n[0], "fixed");
    part(n[1], "already OK");
    part(n[2], "not SVG (saved as image)");
    part(n[3], "failed");
    return s.empty() ? "no files" : s;
}

namespace {
std::string plural(size_t n, const char* word) { return util::formatCount(n) + " " + word + (n == 1 ? "" : "s"); }
}  // namespace

std::string statsLine(const Report& r) {
    std::string s;
    auto sep = [&] {
        if (!s.empty()) s += " | ";
    };
    if (r.width > 0 && r.height > 0) s = util::formatNumber(r.width, 2) + " x " + util::formatNumber(r.height, 2) + " px";
    if (r.shapes) {
        sep();
        s += plural(r.shapes, "shape");
        if (r.gapFillers || r.fills)
            s += " (" + util::formatCount(r.fills) + " filled, " + plural(r.gapFillers, "gap filler") + ")";
    }
    if (r.status != Status::Failed) {
        sep();
        s += util::formatBytes(r.bytesIn);
        if (r.bytesOut != r.bytesIn) s += " -> " + util::formatBytes(r.bytesOut);
    } else if (r.bytesIn) {
        sep();
        s += util::formatBytes(r.bytesIn);
    }
    return s;
}

std::string text(const Header& h, const std::vector<Report>& files) {
    std::string o = "\xEF\xBB\xBF";  // BOM, so every editor picks UTF-8
    o += "SVG-Fixer " SVGFIX_VERSION " report\r\n";
    o += "Source : " + h.source + "\r\n";
    o += "Output : " + h.outputDir + "\r\n";
    o += "Date   : " + h.date + "\r\n\r\n";
    o += "Result : " + std::to_string(files.size()) + " file(s) - " + summaryLine(files) + "\r\n";
    for (const std::string& n : h.notes) o += "Note   : " + n + "\r\n";
    o += "\r\n";
    for (const Report& r : sorted(files)) {
        std::string label = std::string("[") + svgfix::statusLabel(r.status) + "]";
        label.resize(11, ' ');
        o += label + r.name + "\r\n";
        o += std::string(11, ' ') + r.kind + "\r\n";
        std::string st = statsLine(r);
        if (!st.empty()) o += std::string(11, ' ') + st + "\r\n";
        if (!r.outputName.empty()) o += std::string(11, ' ') + "Saved as: " + r.outputName + "\r\n";
        for (const svgfix::Issue& i : r.issues) o += std::string(11, ' ') + "- " + issueTag(i) + ": " + i.text + "\r\n";
        if (r.verified && r.status != Status::Failed) o += std::string(11, ' ') + "- output re-validated: OK\r\n";
        o += "\r\n";
    }
    return o;
}

std::string html(const Header& h, const std::vector<Report>& files) {
    using util::htmlEscape;
    std::string o;
    o += "<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n";
    o += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n";
    o += "<title>SVG-Fixer report</title>\n<style>\n";
    o += R"CSS(:root{--bg:#f5f6f8;--card:#fff;--text:#1d2129;--muted:#5f6b7a;--border:#e1e5ea;--chip:#eceff3;
--ok:#1a7f37;--fixed:#0b62c4;--conv:#9a6700;--failed:#c62828;--sq1:#e9ecef;--sq2:#fff}
@media (prefers-color-scheme:dark){:root{--bg:#121519;--card:#1c2026;--text:#e6e9ee;--muted:#9aa4b2;
--border:#2c323b;--chip:#262b33;--ok:#4ac26b;--fixed:#58a6ff;--conv:#d4a72c;--failed:#ff7070;--sq1:#2a2f36;--sq2:#22262c}}
*{box-sizing:border-box}
body{margin:0;font:14px/1.5 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;background:var(--bg);color:var(--text)}
header{padding:24px 24px 4px}
h1{font-size:20px;margin:0 0 6px}
.meta{color:var(--muted);font-size:13px;overflow-wrap:anywhere}
.summary{display:flex;gap:8px;flex-wrap:wrap;padding:12px 24px 18px}
.pill{padding:3px 11px;border-radius:999px;background:var(--chip);font-weight:600;font-size:13px}
.notes{padding:0 24px 12px;color:var(--muted);font-size:13px}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(min(100%,360px),1fr));gap:16px;padding:0 24px 32px}
.card{background:var(--card);border:1px solid var(--border);border-radius:10px;overflow:hidden;display:flex;flex-direction:column}
.card.failed{border-color:color-mix(in srgb,var(--failed) 45%,var(--border))}
.thumb{height:210px;display:flex;align-items:center;justify-content:center;border-bottom:1px solid var(--border);
background:repeating-conic-gradient(var(--sq1) 0 25%,var(--sq2) 0 50%) 50%/18px 18px}
.thumb a{display:block;width:100%;height:100%}
.thumb img{width:100%;height:100%;object-fit:contain;display:block}
.thumb.none{height:auto;background:var(--chip);color:var(--muted);font-size:12.5px;padding:8px 14px;justify-content:flex-start}
.body{padding:12px 14px 14px}
.badge{display:inline-block;font-size:11px;font-weight:700;letter-spacing:.05em;padding:2px 8px;border-radius:4px;color:#fff}
.badge.ok{background:var(--ok)}.badge.fixed{background:var(--fixed)}.badge.conv{background:var(--conv)}.badge.failed{background:var(--failed)}
.name{font-weight:600;overflow-wrap:anywhere;margin:6px 0 2px}
.kind,.stats{color:var(--muted);font-size:12.5px}
ul{margin:8px 0 0;padding-left:18px}li{margin:3px 0;overflow-wrap:anywhere}
.tag{font-size:10.5px;font-weight:700;text-transform:uppercase;letter-spacing:.04em;margin-right:5px}
.tag.error{color:var(--failed)}.tag.warning{color:var(--conv)}.tag.info{color:var(--muted)}.tag.fixed{color:var(--ok)}
.verified{color:var(--ok);font-size:12.5px;margin-top:6px}
a.btn{display:inline-block;margin-top:10px;padding:6px 12px;border-radius:6px;background:var(--fixed);color:#fff;text-decoration:none;font-weight:600;font-size:13px}
a{color:var(--fixed)}
)CSS";
    o += "</style>\n</head>\n<body>\n<header>\n<h1>SVG-Fixer report</h1>\n";
    o += "<div class=\"meta\">Source: " + htmlEscape(h.source) + "<br>Output: " + htmlEscape(h.outputDir) + "<br>" +
         htmlEscape(h.date) + " &middot; SVG-Fixer " SVGFIX_VERSION "</div>\n</header>\n";

    size_t n[4] = {0, 0, 0, 0};
    for (const Report& r : files) n[rank(r.status)]++;
    o += "<div class=\"summary\"><span class=\"pill\">" + std::to_string(files.size()) + " files</span>";
    const char* labels[4] = {"fixed", "already OK", "not SVG", "failed"};
    const char* classes[4] = {"fixed", "ok", "conv", "failed"};
    for (int k = 0; k < 4; ++k)
        if (n[k])
            o += std::string("<span class=\"pill\"><span class=\"badge ") + classes[k] + "\">" + std::to_string(n[k]) +
                 "</span> " + labels[k] + "</span>";
    o += "</div>\n";
    if (!h.notes.empty()) {
        o += "<div class=\"notes\">";
        for (const std::string& note : h.notes) o += htmlEscape(note) + "<br>";
        o += "</div>\n";
    }

    o += "<main class=\"grid\">\n";
    for (const Report& r : sorted(files)) {
        o += std::string("<section class=\"card ") + cssClass(r.status) + "\">\n";
        if (!r.outputName.empty()) {
            o += "<div class=\"thumb\"><a href=\"" + util::urlEncodePath(r.outputName) + "\"><img loading=\"lazy\" src=\"" +
                 util::urlEncodePath(r.outputName) + "\" alt=\"" + htmlEscape(r.name) + "\"></a></div>\n";
        } else {
            std::string why = r.status == Status::Failed ? "Nothing written: this file cannot be repaired." : "Not written (validate-only run).";
            o += "<div class=\"thumb none\">" + htmlEscape(why) + "</div>\n";
        }
        o += "<div class=\"body\">\n";
        o += std::string("<span class=\"badge ") + cssClass(r.status) + "\">" + svgfix::statusLabel(r.status) + "</span>\n";
        o += "<div class=\"name\">" + htmlEscape(r.name) + "</div>\n";
        o += "<div class=\"kind\">" + htmlEscape(r.kind) + "</div>\n";
        std::string st = statsLine(r);
        if (!st.empty()) o += "<div class=\"stats\">" + htmlEscape(st) + "</div>\n";
        if (!r.issues.empty()) {
            o += "<ul>\n";
            for (const svgfix::Issue& i : r.issues) {
                o += std::string("<li><span class=\"tag ") + svgfix::severityLabel(i.severity) + "\">" +
                     svgfix::severityLabel(i.severity) + "</span>";
                if (i.fixed) o += "<span class=\"tag fixed\">fixed</span>";
                o += htmlEscape(i.text) + "</li>\n";
            }
            o += "</ul>\n";
        }
        if (r.verified && r.status != Status::Failed) o += "<div class=\"verified\">&#10003; Output re-validated</div>\n";
        if (!r.page.url.empty())
            o += "<a class=\"btn\" href=\"" + htmlEscape(r.page.url) + "\">Open on vectorizer.ai to export again</a>\n";
        o += "</div>\n</section>\n";
    }
    o += "</main>\n</body>\n</html>\n";
    return o;
}

}  // namespace report
