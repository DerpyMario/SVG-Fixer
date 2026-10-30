#include "svgfix.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>

#include "pathdata.hpp"
#include "util.hpp"
#include "xml.hpp"

namespace svgfix {

const char* statusLabel(Status s) {
    switch (s) {
        case Status::Ok: return "OK";
        case Status::Fixed: return "FIXED";
        case Status::Converted: return "NOT SVG";
        case Status::Failed: return "FAILED";
    }
    return "?";
}

const char* severityLabel(Severity s) {
    switch (s) {
        case Severity::Info: return "info";
        case Severity::Warning: return "warning";
        case Severity::Error: return "error";
    }
    return "?";
}

namespace {

constexpr const char* kSvgNs = "http://www.w3.org/2000/svg";
constexpr size_t kMinEmbeddedShapes = 50;  // fewer than this in a web page = UI icons, not artwork
constexpr size_t npos = std::string::npos;

using pathdata::BBox;
using pathdata::Matrix;
using util::formatCount;

std::string num(double v) { return util::formatNumber(v, 3); }

struct Ctx {
    Result& res;
    const Options& opt;
    bool changed = false;

    Report& rep() { return res.report; }
    void add(Severity sev, bool fixed, std::string text) {
        res.report.issues.push_back({sev, fixed, std::move(text)});
        if (fixed) changed = true;
    }
};

// ---------------------------------------------------------------------------
// Byte level: file signatures and text encoding

struct Signature {
    const char* bytes;
    size_t len;
    const char* kind;
    const char* ext;
    bool image;
};

const Signature kSignatures[] = {
    {"\x89PNG\r\n\x1a\n", 8, "PNG image", ".png", true},
    {"\xFF\xD8\xFF", 3, "JPEG image", ".jpg", true},
    {"GIF87a", 6, "GIF image", ".gif", true},
    {"GIF89a", 6, "GIF image", ".gif", true},
    {"\x1F\x8B", 2, "gzip-compressed SVG (SVGZ)", ".svgz", true},
    {"%PDF", 4, "PDF document", ".pdf", false},
    {"PK\x03\x04", 4, "ZIP archive", ".zip", false},
    {"7z\xBC\xAF\x27\x1C", 6, "7z archive", ".7z", false},
    {"Rar!\x1A\x07", 6, "RAR archive", ".rar", false},
};

const Signature* sniff(const std::string& b) {
    for (const Signature& s : kSignatures)
        if (b.size() >= s.len && std::memcmp(b.data(), s.bytes, s.len) == 0) return &s;
    static const Signature webp = {"", 0, "WebP image", ".webp", true};
    if (b.size() >= 12 && b.compare(0, 4, "RIFF") == 0 && b.compare(8, 4, "WEBP") == 0) return &webp;
    return nullptr;
}

const uint16_t kCp1252[32] = {0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                              0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                              0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};

size_t utf8Sequence(const unsigned char* p, size_t avail, uint32_t& cp) {
    unsigned char c = p[0];
    if (c < 0x80) {
        cp = c;
        return 1;
    }
    size_t n;
    uint32_t min;
    if ((c & 0xE0) == 0xC0) {
        n = 2; cp = c & 0x1F; min = 0x80;
    } else if ((c & 0xF0) == 0xE0) {
        n = 3; cp = c & 0x0F; min = 0x800;
    } else if ((c & 0xF8) == 0xF0) {
        n = 4; cp = c & 0x07; min = 0x10000;
    } else {
        return 0;
    }
    if (avail < n) return 0;
    for (size_t k = 1; k < n; ++k) {
        if ((p[k] & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (p[k] & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    return n;
}

// Produces clean UTF-8 that an XML parser accepts.
void decodeText(const std::string& in, std::string& out, Ctx& ctx) {
    const auto* b = reinterpret_cast<const unsigned char*>(in.data());
    const size_t n = in.size();
    std::string utf8;
    const std::string* src = &in;
    size_t start = 0;
    bool le = false, be = false;
    if (n >= 2 && b[0] == 0xFF && b[1] == 0xFE) { le = true; start = 2; }
    else if (n >= 2 && b[0] == 0xFE && b[1] == 0xFF) { be = true; start = 2; }
    else if (n >= 4 && b[0] == '<' && b[1] == 0 && b[2] != 0 && b[3] == 0) le = true;
    else if (n >= 4 && b[0] == 0 && b[1] == '<' && b[2] == 0 && b[3] != 0) be = true;
    if (le || be) {
        std::vector<uint16_t> units;
        for (size_t i = start; i + 1 < n; i += 2) units.push_back(le ? (uint16_t)(b[i] | (b[i + 1] << 8)) : (uint16_t)((b[i] << 8) | b[i + 1]));
        utf8 = util::utf16ToUtf8(units.data(), units.size());
        src = &utf8;
        start = 0;
        ctx.add(Severity::Warning, true, "The file was saved as UTF-16; converted it to UTF-8.");
    } else if (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) {
        start = 3;  // UTF-8 byte order mark: legal, just skipped
    }

    const auto* p = reinterpret_cast<const unsigned char*>(src->data());
    const size_t len = src->size();
    out.clear();
    out.reserve(len - start);
    size_t controls = 0, invalid = 0;
    for (size_t i = start; i < len;) {
        uint32_t cp;
        size_t k = utf8Sequence(p + i, len - i, cp);
        if (k == 0) {
            unsigned char c = p[i];
            util::appendUtf8(out, c >= 0x80 && c <= 0x9F ? kCp1252[c - 0x80] : c);
            ++invalid;
            ++i;
            continue;
        }
        if ((cp < 0x20 && cp != '\t' && cp != '\n' && cp != '\r') || cp == 0xFFFE || cp == 0xFFFF) {
            ++controls;
        } else {
            out.append(reinterpret_cast<const char*>(p + i), k);
        }
        i += k;
    }
    if (controls)
        ctx.add(Severity::Error, true, "Removed " + formatCount(controls) +
                                           " invalid control character(s) (e.g. NUL bytes); XML forbids them, so "
                                           "browsers refuse to open the file.");
    if (invalid)
        ctx.add(Severity::Error, true, formatCount(invalid) +
                                           " byte(s) were not valid UTF-8 (probably Windows-1252 text); converted "
                                           "them to UTF-8.");
}

// ---------------------------------------------------------------------------
// Web pages (the userscript sometimes saves the page instead of the drawing)

bool looksLikeHtml(std::string_view t) {
    size_t i = 0;
    for (;;) {
        while (i < t.size() && util::isSpace(t[i])) ++i;
        if (t.substr(i, 4) == "<!--") {
            size_t e = t.find("-->", i);
            if (e == npos) return false;
            i = e + 3;
        } else if (t.substr(i, 2) == "<?") {
            size_t e = t.find("?>", i);
            if (e == npos) return false;
            i = e + 2;
        } else {
            break;
        }
    }
    std::string_view rest = t.substr(i);
    if (util::istartsWith(rest, "<!doctype html")) return true;
    static const char* const kTags[] = {"<html", "<head", "<body", "<meta", "<link", "<script", "<title", "<div", "<style"};
    for (const char* tag : kTags) {
        size_t n = std::strlen(tag);
        if (util::istartsWith(rest, tag) && rest.size() > n && (util::isSpace(rest[n]) || rest[n] == '>' || rest[n] == '/'))
            return true;
    }
    return false;
}

std::string htmlAttr(std::string_view html, std::string_view name) {
    size_t p = 0;
    while ((p = html.find(name, p)) != npos) {
        size_t q = p + name.size();
        if (p == 0 || !util::isSpace(html[p - 1])) {
            p = q;
            continue;
        }
        while (q < html.size() && util::isSpace(html[q])) ++q;
        if (q >= html.size() || html[q] != '=') {
            p = q;
            continue;
        }
        ++q;
        while (q < html.size() && util::isSpace(html[q])) ++q;
        if (q < html.size() && (html[q] == '"' || html[q] == '\'')) {
            size_t e = html.find(html[q], q + 1);
            if (e != npos) return std::string(html.substr(q + 1, e - q - 1));
        }
        p = q;
    }
    return {};
}

std::string decodeBasicEntities(std::string s) {
    static const std::pair<const char*, const char*> kMap[] = {
        {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&#39;", "'"}, {"&#x27;", "'"}, {"&nbsp;", " "}};
    for (const auto& m : kMap) {
        size_t p = 0, n = std::strlen(m.first);
        while ((p = s.find(m.first, p)) != npos) {
            s.replace(p, n, m.second);
            p += 1;
        }
    }
    return s;
}

PageInfo readPageInfo(std::string_view html) {
    PageInfo pi;
    size_t a = util::ifind(html, "<title>");
    size_t b = a == npos ? npos : util::ifind(html, "</title>", a);
    if (a != npos && b != npos) pi.title = decodeBasicEntities(std::string(util::trim(html.substr(a + 7, b - a - 7))));
    const std::string suffix = " - Vectorizer.AI";
    if (util::iendsWith(pi.title, suffix)) {
        pi.isVectorizer = true;
        pi.sourceImage = pi.title.substr(0, pi.title.size() - suffix.size());
    }
    std::string w = htmlAttr(html, "data-image-width"), h = htmlAttr(html, "data-image-height");
    if (!w.empty() && !h.empty()) pi.imageSize = w + " x " + h;
    const std::string base = "https://vectorizer.ai/images/";
    size_t u = html.find(base);
    if (u != npos) {
        size_t e = u + base.size();
        while (e < html.size() && (std::isalnum((unsigned char)html[e]) || html[e] == '-')) ++e;
        if (e > u + base.size()) {
            pi.url = std::string(html.substr(u, e - u));
            pi.isVectorizer = true;
        }
    }
    return pi;
}

size_t countShapeTags(std::string_view s) {
    static const char* const kShapes[] = {"<path", "<polygon", "<polyline", "<rect", "<circle", "<ellipse", "<line"};
    size_t count = 0;
    for (const char* tag : kShapes) {
        size_t n = std::strlen(tag), p = 0;
        while ((p = s.find(tag, p)) != npos) {
            p += n;
            if (p < s.size() && (util::isSpace(s[p]) || s[p] == '/' || s[p] == '>')) ++count;
        }
    }
    return count;
}

// The inline <svg> element with the most shapes, if any.
bool largestEmbeddedSvg(std::string_view html, size_t& start, size_t& end, size_t& shapes) {
    shapes = 0;
    size_t p = 0;
    while ((p = util::ifind(html, "<svg", p)) != npos) {
        char next = p + 4 < html.size() ? html[p + 4] : 0;
        if (!(util::isSpace(next) || next == '>' || next == '/')) {
            p += 4;
            continue;
        }
        int depth = 0;
        size_t q = p, stop = npos;
        while (q < html.size()) {
            size_t open = util::ifind(html, "<svg", q), close = util::ifind(html, "</svg", q);
            if (close == npos) break;
            if (open != npos && open < close) {
                ++depth;
                q = open + 4;
            } else {
                --depth;
                q = close + 5;
                if (depth <= 0) {
                    size_t gt = html.find('>', q);
                    stop = gt == npos ? html.size() : gt + 1;
                    break;
                }
            }
        }
        if (stop == npos) break;
        size_t n = countShapeTags(html.substr(p, stop - p));
        if (n > shapes) {
            shapes = n;
            start = p;
            end = stop;
        }
        p = stop;
    }
    return shapes > 0;
}

// ---------------------------------------------------------------------------
// SVG helpers

struct Length {
    double px = 0;
    bool ok = false;
};

Length parseLength(const std::string* s) {
    Length r;
    if (!s) return r;
    std::string t(util::trim(*s));
    char* end = nullptr;
    double v = std::strtod(t.c_str(), &end);
    if (end == t.c_str() || !std::isfinite(v) || v <= 0) return r;
    std::string unit = util::toLower(util::trim(std::string_view(end)));
    double f = 0;
    if (unit.empty() || unit == "px") f = 1;
    else if (unit == "pt") f = 96.0 / 72;
    else if (unit == "pc") f = 16;
    else if (unit == "mm") f = 96 / 25.4;
    else if (unit == "cm") f = 96 / 2.54;
    else if (unit == "in") f = 96;
    if (f == 0) return r;  // %, em, ex: no absolute size
    r.px = v * f;
    r.ok = true;
    return r;
}

bool parseViewBox(const std::string& s, double v[4]) {
    std::string t = s;
    for (char& c : t)
        if (c == ',') c = ' ';
    const char* p = t.c_str();
    for (int i = 0; i < 4; ++i) {
        char* end = nullptr;
        v[i] = std::strtod(p, &end);
        if (end == p || !std::isfinite(v[i])) return false;
        p = end;
    }
    while (*p && util::isSpace(*p)) ++p;
    return *p == 0 && v[2] > 0 && v[3] > 0;
}

std::string viewBoxText(double x, double y, double w, double h) { return num(x) + " " + num(y) + " " + num(w) + " " + num(h); }

void walkElements(xml::Node* n, const std::function<void(xml::Node*)>& fn) {
    for (auto& c : n->children) {
        if (!c->isElement()) continue;
        fn(c.get());
        walkElements(c.get(), fn);
    }
}

bool isNonRendered(std::string_view ln) {
    return ln == "defs" || ln == "clipPath" || ln == "mask" || ln == "symbol" || ln == "pattern" || ln == "marker" ||
           ln == "linearGradient" || ln == "radialGradient" || ln == "filter" || ln == "style" || ln == "script" ||
           ln == "metadata" || ln == "title" || ln == "desc";
}

bool isShape(std::string_view ln) {
    return ln == "path" || ln == "rect" || ln == "circle" || ln == "ellipse" || ln == "line" || ln == "polyline" ||
           ln == "polygon" || ln == "text" || ln == "image" || ln == "use";
}

double attrNum(const xml::Node* n, const char* name, double def = 0) {
    const std::string* s = n->attr(name);
    if (!s) return def;
    char* end = nullptr;
    double v = std::strtod(s->c_str(), &end);
    return (end == s->c_str() || !std::isfinite(v)) ? def : v;
}

// Geometry of basic shapes as path data, for measuring.
bool shapeGeometry(const xml::Node* n, pathdata::Path& p) {
    std::string_view ln = n->localName();
    std::string d;
    if (ln == "path") {
        const std::string* s = n->attr("d");
        if (!s) return false;
        d = *s;
    } else if (ln == "rect" || ln == "image" || ln == "use") {
        double x = attrNum(n, "x"), y = attrNum(n, "y"), w = attrNum(n, "width"), h = attrNum(n, "height");
        if (w <= 0 || h <= 0) return false;
        d = "M" + num(x) + " " + num(y) + "H" + num(x + w) + "V" + num(y + h) + "H" + num(x) + "Z";
    } else if (ln == "circle" || ln == "ellipse") {
        double cx = attrNum(n, "cx"), cy = attrNum(n, "cy");
        double rx = ln == "circle" ? attrNum(n, "r") : attrNum(n, "rx");
        double ry = ln == "circle" ? rx : attrNum(n, "ry");
        if (rx <= 0 || ry <= 0) return false;
        d = "M" + num(cx - rx) + " " + num(cy) + "A" + num(rx) + " " + num(ry) + " 0 1 0 " + num(cx + rx) + " " +
            num(cy) + "A" + num(rx) + " " + num(ry) + " 0 1 0 " + num(cx - rx) + " " + num(cy);
    } else if (ln == "line") {
        d = "M" + num(attrNum(n, "x1")) + " " + num(attrNum(n, "y1")) + "L" + num(attrNum(n, "x2")) + " " +
            num(attrNum(n, "y2"));
    } else if (ln == "polyline" || ln == "polygon") {
        const std::string* s = n->attr("points");
        if (!s) return false;
        d = "M" + *s;
    } else {
        return false;
    }
    pathdata::parse(d, p);
    return !p.segs.empty();
}

void contentBounds(xml::Node* n, const Matrix& parent, BBox& box) {
    for (auto& c : n->children) {
        if (!c->isElement()) continue;
        std::string_view ln = c->localName();
        if (isNonRendered(ln)) continue;
        Matrix m = parent;
        if (const std::string* t = c->attr("transform")) {
            Matrix tm;
            if (pathdata::parseTransform(*t, tm)) m = parent * tm;
        }
        pathdata::Path p;
        if (shapeGeometry(c.get(), p)) {
            pathdata::bounds(p, m, box);
        } else if (ln == "text") {
            double x, y;
            m.apply(attrNum(c.get(), "x"), attrNum(c.get(), "y"), x, y);
            box.add(x, y);
        }
        if (ln == "g" || ln == "a" || ln == "switch" || ln == "svg") contentBounds(c.get(), m, box);
    }
}

const char* knownNamespace(const std::string& prefix) {
    static const std::map<std::string, const char*> kKnown = {
        {"xlink", "http://www.w3.org/1999/xlink"},
        {"svg", kSvgNs},
        {"sodipodi", "http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd"},
        {"inkscape", "http://www.inkscape.org/namespaces/inkscape"},
        {"rdf", "http://www.w3.org/1999/02/22-rdf-syntax-ns#"},
        {"cc", "http://creativecommons.org/ns#"},
        {"dc", "http://purl.org/dc/elements/1.1/"},
        {"sketch", "http://www.bohemiancoding.com/sketch/ns"},
        {"i", "http://ns.adobe.com/AdobeIllustrator/10.0/"},
        {"x", "http://ns.adobe.com/Extensibility/1.0/"},
        {"graph", "http://ns.adobe.com/Graphs/1.0/"},
        {"serif", "http://www.serif.com/"},
    };
    auto it = kKnown.find(prefix);
    return it == kKnown.end() ? nullptr : it->second;
}

void fixNamespaces(Ctx& ctx, xml::Node* root) {
    std::string_view prefix = root->prefix();
    std::string declName = prefix.empty() ? "xmlns" : "xmlns:" + std::string(prefix);
    const std::string* ns = root->attr(declName);
    if (!ns) {
        root->attrs.insert(root->attrs.begin(), xml::Attr{declName, kSvgNs});
        ctx.add(Severity::Error, true,
                "The <svg> element has no " + declName + "=\"" + kSvgNs +
                    "\" declaration; browsers then show the file as plain XML instead of a picture. Added it.");
    } else if (*ns != kSvgNs) {
        std::string old = *ns;
        root->setAttr(declName, kSvgNs);
        ctx.add(Severity::Error, true, "The SVG namespace was wrong (\"" + old + "\"); corrected it.");
    }

    std::set<std::string> declared = {"xml", "xmlns"};
    auto collect = [&](xml::Node* n) {
        for (const xml::Attr& a : n->attrs)
            if (a.name.compare(0, 6, "xmlns:") == 0) declared.insert(a.name.substr(6));
    };
    collect(root);
    walkElements(root, collect);
    std::set<std::string> missing;
    auto check = [&](xml::Node* n) {
        if (!n->prefix().empty() && !declared.count(std::string(n->prefix()))) missing.insert(std::string(n->prefix()));
        for (const xml::Attr& a : n->attrs) {
            size_t c = a.name.find(':');
            if (c == npos || a.name.compare(0, 6, "xmlns:") == 0) continue;
            std::string p = a.name.substr(0, c);
            if (!declared.count(p)) missing.insert(p);
        }
    };
    check(root);
    walkElements(root, check);
    for (const std::string& p : missing) {
        const char* uri = knownNamespace(p);
        root->setAttr("xmlns:" + p, uri ? uri : "urn:svg-fixer:undeclared:" + p);
        ctx.add(Severity::Error, true,
                "The prefix \"" + p + ":\" is used without declaring its namespace, which makes browsers reject "
                "the file. Added xmlns:" + p + ".");
    }
}

// Removes <script> elements, on* event handlers and javascript: links from
// n and everything below it.
void stripScripts(xml::Node* n, size_t& scripts, size_t& handlers) {
    for (auto a = n->attrs.begin(); a != n->attrs.end();) {
        size_t colon = a->name.find(':');
        std::string local = colon == npos ? a->name : a->name.substr(colon + 1);
        bool handler = local.size() > 2 && util::istartsWith(local, "on");
        bool jsLink = local == "href" && util::istartsWith(util::trim(a->value), "javascript:");
        if (handler || jsLink) {
            a = n->attrs.erase(a);
            ++handlers;
        } else {
            ++a;
        }
    }
    for (auto it = n->children.begin(); it != n->children.end();) {
        xml::Node* c = it->get();
        if (c->isElement() && util::iequals(c->localName(), "script")) {
            it = n->children.erase(it);
            ++scripts;
            continue;
        }
        if (c->isElement()) stripScripts(c, scripts, handlers);
        ++it;
    }
}

// ---------------------------------------------------------------------------
// Canvas captures: a flat list of <path> elements, each carrying the viewer's
// zoom as transform="matrix(s 0 0 s tx ty)", no width/height/viewBox, float32
// coordinates and stroke-only "gap filler" edges between the filled shapes.

const char* const kCaptureAttrs[] = {"d",          "transform",      "fill",          "stroke",          "fill-opacity",
                                     "stroke-opacity", "stroke-width", "fill-rule",    "opacity",         "stroke-linecap",
                                     "stroke-linejoin", "stroke-miterlimit", "stroke-dasharray", "stroke-dashoffset",
                                     "vector-effect", "id",            "style"};

struct Capture {
    bool hasZoom = false;
    Matrix zoom;
    std::string transform;
};

bool detectCapture(xml::Node* root, Capture& cap) {
    bool first = true;
    std::string transform;
    size_t paths = 0;
    for (auto& ch : root->children) {
        if (ch->type == xml::NodeType::Text) {
            if (!util::trim(ch->value).empty()) return false;
            continue;
        }
        if (ch->type == xml::NodeType::Comment) continue;
        if (ch->type != xml::NodeType::Element || ch->name != "path") return false;
        for (auto& g : ch->children)
            if (g->type != xml::NodeType::Text || !util::trim(g->value).empty()) return false;
        for (const xml::Attr& a : ch->attrs)
            if (std::find_if(std::begin(kCaptureAttrs), std::end(kCaptureAttrs),
                             [&](const char* k) { return a.name == k; }) == std::end(kCaptureAttrs))
                return false;
        const std::string* t = ch->attr("transform");
        std::string tv = t ? *t : std::string();
        if (first)
            transform = tv;
        else if (tv != transform)
            return false;
        first = false;
        ++paths;
    }
    if (!paths) return false;
    if (!transform.empty()) {
        Matrix m;
        if (!pathdata::parseTransform(transform, m)) return false;
        if (m.b != 0 || m.c != 0 || m.a <= 0 || m.d <= 0 || std::fabs(m.a - m.d) > 1e-6 * m.a) return false;
        cap.hasZoom = true;
        cap.zoom = m;
        cap.transform = transform;
    } else if (root->attr("viewBox") || root->attr("width") || root->attr("height")) {
        return false;  // an ordinary, already sized SVG
    }
    return true;
}

bool isGapFiller(const xml::Node* n) {
    const std::string* fill = n->attr("fill");
    const std::string* stroke = n->attr("stroke");
    return fill && util::iequals(util::trim(*fill), "none") && stroke && !util::iequals(util::trim(*stroke), "none");
}

int autoPrecision(double size) {
    int p = (int)std::ceil(5 - std::log10(std::max(size, 1e-9)));
    return std::min(8, std::max(0, p));
}

void fixCapture(Ctx& ctx, xml::Node* root, const Capture& cap) {
    Report& rep = ctx.rep();
    rep.kind = cap.hasZoom ? "canvas capture (vectorizer.ai userscript export)" : "flat list of SVG paths";

    struct Item {
        std::unique_ptr<xml::Node> node;
        pathdata::Path path;
    };
    std::vector<Item> items;
    std::vector<std::unique_ptr<xml::Node>> comments;
    BBox ends;
    size_t numbers = 0, noisy = 0, badPaths = 0, badSegments = 0, oldBytes = 0;
    std::string noisyExample, firstError;
    for (auto& ch : root->children) {
        if (ch->type == xml::NodeType::Comment) {
            comments.push_back(std::move(ch));
            continue;
        }
        if (!ch->isElement()) continue;
        Item it;
        const std::string* d = ch->attr("d");
        pathdata::ParseResult pr = pathdata::parse(d ? *d : std::string(), it.path);
        oldBytes += d ? d->size() : 0;
        numbers += pr.numbers;
        noisy += pr.noisyNumbers;
        if (noisyExample.empty() || (noisyExample.find_first_of("eE") != npos && !pr.noisyExample.empty() &&
                                     pr.noisyExample.find_first_of("eE") == npos))
            noisyExample = pr.noisyExample;
        if (!pr.ok) {
            ++badPaths;
            badSegments += (size_t)pr.droppedSegments;
            if (firstError.empty()) firstError = pr.firstError;
        }
        pathdata::endpointBounds(it.path, ends);
        it.node = std::move(ch);
        items.push_back(std::move(it));
    }
    const size_t pathCount = items.size();

    // Canvas. Captured coordinates are image pixels; the viewer's zoom only
    // scaled them for the screen.
    const Matrix& z = cap.zoom;
    double vx = 0, vy = 0, vw = 0, vh = 0;
    bool hadSize = false;
    double v[4];
    Length w = parseLength(root->attr("width")), h = parseLength(root->attr("height"));
    if (const std::string* vb = root->attr("viewBox"); vb && parseViewBox(*vb, v)) {
        vx = v[0]; vy = v[1]; vw = v[2]; vh = v[3];
        hadSize = true;
    } else if (w.ok && h.ok) {
        vw = w.px; vh = h.px;
        hadSize = true;
    }
    if (hadSize && cap.hasZoom) {  // was expressed in zoomed screen units
        vx = (vx - z.e) / z.a;
        vy = (vy - z.f) / z.d;
        vw /= z.a;
        vh /= z.d;
    }
    if (!hadSize && !ends.empty()) {
        auto snapUp = [](double x) { double r = std::round(x); return std::fabs(x - r) <= 0.01 ? r : std::ceil(x); };
        auto snapDown = [](double x) { double r = std::round(x); return std::fabs(x - r) <= 0.01 ? r : std::floor(x); };
        vx = ends.x0 >= -0.5 ? 0 : snapDown(ends.x0);
        vy = ends.y0 >= -0.5 ? 0 : snapDown(ends.y0);
        vw = snapUp(ends.x1) - vx;
        vh = snapUp(ends.y1) - vy;
    }

    const int precision = ctx.opt.precision >= 0 ? ctx.opt.precision : autoPrecision(std::max(vw, vh));
    pathdata::FormatOptions fo;
    fo.precision = precision;
    fo.dropZeroLength = true;
    pathdata::FormatStats fs;
    size_t emptyPaths = 0, strippedGap = 0, redundant = 0, newBytes = 0;
    std::vector<std::unique_ptr<xml::Node>> kept;
    for (Item& it : items) {
        xml::Node* n = it.node.get();
        const bool gap = isGapFiller(n);
        if (gap && ctx.opt.stripGapFillers) {
            ++strippedGap;
            continue;
        }
        std::string d = it.path.hasDrawing() ? pathdata::format(it.path, fo, &fs) : std::string();
        if (d.find_first_not_of("MmZz0123456789.- ") == npos) {  // nothing left but movetos
            ++emptyPaths;
            continue;
        }
        newBytes += d.size();
        n->setAttr("d", std::move(d));
        n->removeAttr("transform");
        for (const char* a : {"fill-opacity", "stroke-opacity", "opacity"}) {
            const std::string* val = n->attr(a);
            if (val && (*val == "1" || *val == "1.0")) {
                n->removeAttr(a);
                ++redundant;
            }
        }
        (gap ? rep.gapFillers : rep.fills)++;
        kept.push_back(std::move(it.node));
    }

    if (kept.empty() || vw <= 0 || vh <= 0) {
        root->children.clear();  // the elements were moved out above
        ctx.add(Severity::Error, false,
                "The SVG contains no drawable shapes - the export captured an empty canvas (it probably ran before "
                "the vectorized image was drawn). Export it again.");
        return;
    }

    // Rebuild <svg>: namespace, version, size and viewBox first.
    std::vector<xml::Attr> attrs;
    for (const xml::Attr& a : root->attrs)
        if (a.name == "xmlns" || a.name.compare(0, 6, "xmlns:") == 0) attrs.push_back(a);
    const std::string* version = root->attr("version");
    attrs.push_back({"version", version ? *version : "1.1"});
    attrs.push_back({"width", num(vw)});
    attrs.push_back({"height", num(vh)});
    attrs.push_back({"viewBox", viewBoxText(vx, vy, vw, vh)});
    for (const xml::Attr& a : root->attrs)
        if (a.name != "xmlns" && a.name.compare(0, 6, "xmlns:") != 0 && a.name != "version" && a.name != "width" &&
            a.name != "height" && a.name != "viewBox")
            attrs.push_back(a);
    root->attrs = std::move(attrs);
    root->children.clear();
    auto newline = [&] {
        auto t = std::make_unique<xml::Node>();
        t->type = xml::NodeType::Text;
        t->value = "\n";
        root->append(std::move(t));
    };
    for (auto& c : comments) {
        newline();
        root->append(std::move(c));
    }
    for (auto& k : kept) {
        newline();
        root->append(std::move(k));
    }
    newline();
    rep.width = vw;
    rep.height = vh;

    // Report what was wrong and what changed.
    const std::string size = num(vw) + " x " + num(vh) + " px";
    if (!hadSize)
        ctx.add(Severity::Error, true,
                "No width, height or viewBox: viewers fall back to a 300 x 150 px box and crop or shrink the "
                "artwork. Set the canvas to " + size + ", the vectorized image's native size.");
    else if (cap.hasZoom)
        ctx.add(Severity::Warning, true, "Converted the canvas size from zoomed screen units to native pixels: " + size + ".");
    if (cap.hasZoom)
        ctx.add(Severity::Warning, true,
                "All " + formatCount(pathCount) + " paths carried the viewer's zoom transform=\"" + cap.transform +
                    "\" (x" + util::formatNumber(z.a, 4) +
                    "). Removed it, so coordinates are native image pixels.");
    if (badPaths)
        ctx.add(Severity::Error, true,
                formatCount(badPaths) + " path(s) contained invalid path data (" + firstError +
                    "). Renderers stop drawing a path at the first error; removed the " + formatCount(badSegments) +
                    " bad segment(s) so the rest of each shape is drawn.");
    if (noisy && newBytes < oldBytes * 0.9) {
        std::string example = noisyExample;
        if (example.find_first_of("eE") == npos)
            example += " instead of " + pathdata::formatNumber(std::strtod(noisyExample.c_str(), nullptr), precision);
        ctx.add(Severity::Warning, true,
                formatCount(noisy) + " of " + formatCount(numbers) + " numbers carried float32 noise (e.g. " + example +
                    "). Rounded them to " + std::to_string(precision) + " decimals (at most " +
                    util::formatNumber(0.5 * std::pow(10.0, -precision), 8) +
                    " px change, invisible), which makes the file much smaller.");
    }
    if (rep.gapFillers)
        ctx.add(Severity::Info, false,
                "Kept " + formatCount(rep.gapFillers) +
                    " gap-filler strokes (stroke-only paths). They cover the hairline seams that anti-aliasing "
                    "leaves between neighbouring shapes.");
    if (strippedGap)
        ctx.add(Severity::Info, true,
                "Removed " + formatCount(strippedGap) + " gap-filler strokes as requested (hairline seams may show).");
    if (emptyPaths)
        ctx.add(Severity::Warning, true, "Removed " + formatCount(emptyPaths) + " empty path(s) that drew nothing.");
    if (fs.droppedZeroLength || redundant)
        ctx.add(Severity::Info, true,
                "Cleanup: removed " + formatCount((size_t)fs.droppedZeroLength) + " zero-length segment(s) and " +
                    formatCount(redundant) + " redundant opacity=\"1\" attribute(s).");
    if (fs.preciseArcs)
        ctx.add(Severity::Info, false,
                formatCount((size_t)fs.preciseArcs) + " nearly closed arc(s) were kept at full precision so rounding "
                "cannot distort them.");
}

// ---------------------------------------------------------------------------
// Any other SVG: conservative repairs only.

void fixPaths(Ctx& ctx, xml::Node* root) {
    size_t bad = 0, segments = 0;
    std::string firstError;
    walkElements(root, [&](xml::Node* n) {
        if (n->localName() != "path") return;
        const std::string* d = n->attr("d");
        if (!d) return;
        pathdata::Path p;
        pathdata::ParseResult pr = pathdata::parse(*d, p);
        if (pr.ok) return;
        ++bad;
        segments += (size_t)pr.droppedSegments;
        if (firstError.empty()) firstError = pr.firstError;
        n->setAttr("d", pathdata::format(p, pathdata::FormatOptions{}));
    });
    if (bad)
        ctx.add(Severity::Error, true,
                formatCount(bad) + " path(s) contained invalid path data (" + firstError +
                    "). Renderers stop drawing a path at the first error; removed the " + formatCount(segments) +
                    " bad segment(s) so the rest of each shape is drawn.");
}

void fixCanvas(Ctx& ctx, xml::Node* root) {
    Report& rep = ctx.rep();
    const std::string* vb = root->attr("viewBox");
    Length w = parseLength(root->attr("width")), h = parseLength(root->attr("height"));
    double v[4];
    if (vb && parseViewBox(*vb, v)) {
        rep.width = w.ok ? w.px : v[2];
        rep.height = h.ok ? h.px : v[3];
        if (!root->attr("width") && !root->attr("height"))
            ctx.add(Severity::Info, false,
                    "No width/height: the image scales to fit wherever it is placed (fine for browsers; some apps "
                    "assume a default size).");
        return;
    }
    const bool invalid = vb != nullptr;
    const std::string what = invalid ? "The viewBox=\"" + *vb + "\" is invalid" : "There is no viewBox";
    if (w.ok && h.ok) {
        root->setAttr("viewBox", viewBoxText(0, 0, w.px, h.px));
        rep.width = w.px;
        rep.height = h.px;
        ctx.add(invalid ? Severity::Error : Severity::Warning, true,
                what + ", so the drawing cannot be scaled. Added viewBox=\"" + viewBoxText(0, 0, w.px, h.px) +
                    "\" from its width and height.");
        return;
    }
    BBox box;
    contentBounds(root, Matrix(), box);
    if (box.empty()) {
        ctx.add(Severity::Warning, false, what + " and no usable width/height, and the artwork bounds could not be measured.");
        return;
    }
    double x0 = box.x0 >= 0 ? 0 : std::floor(box.x0), y0 = box.y0 >= 0 ? 0 : std::floor(box.y0);
    double x1 = std::ceil(box.x1 - 1e-6), y1 = std::ceil(box.y1 - 1e-6);
    double bw = std::max(1.0, x1 - x0), bh = std::max(1.0, y1 - y0);
    root->setAttr("viewBox", viewBoxText(x0, y0, bw, bh));
    std::string extra;
    if (!root->attr("width") && !root->attr("height")) {
        root->setAttr("width", num(bw));
        root->setAttr("height", num(bh));
        extra = " and width/height";
    }
    rep.width = bw;
    rep.height = bh;
    ctx.add(Severity::Error, true,
            what + " and no usable width/height: viewers fall back to a 300 x 150 px box and crop the artwork. "
                   "Added a viewBox" + extra + " measured from the artwork (" + num(bw) + " x " + num(bh) + " px).");
}

// ---------------------------------------------------------------------------

bool verifyOutput(const std::string& data, std::string& why) {
    std::string_view t = data;
    if (t.size() >= 3 && (unsigned char)t[0] == 0xEF && (unsigned char)t[1] == 0xBB && (unsigned char)t[2] == 0xBF)
        t.remove_prefix(3);
    xml::ParseResult pr = xml::parse(t);
    if (!pr.issues.empty()) {
        why = "XML: " + pr.issues.front().message;
        return false;
    }
    xml::Node* root = pr.doc.root;
    if (!root || root->localName() != "svg") {
        why = "the root element is not <svg>";
        return false;
    }
    std::string decl = root->prefix().empty() ? "xmlns" : "xmlns:" + std::string(root->prefix());
    const std::string* ns = root->attr(decl);
    if (!ns || *ns != kSvgNs) {
        why = "missing SVG namespace";
        return false;
    }
    double v[4];
    const std::string* vb = root->attr("viewBox");
    if (!(vb && parseViewBox(*vb, v)) && !(parseLength(root->attr("width")).ok && parseLength(root->attr("height")).ok)) {
        why = "no usable viewBox or width/height";
        return false;
    }
    bool pathsOk = true;
    walkElements(root, [&](xml::Node* n) {
        const std::string* d = n->localName() == "path" ? n->attr("d") : nullptr;
        pathdata::Path p;
        if (d && !pathdata::parse(*d, p).ok) pathsOk = false;
    });
    if (!pathsOk) why = "invalid path data";
    return pathsOk;
}

size_t countShapes(xml::Node* root) {
    size_t n = 0;
    std::function<void(xml::Node*)> walk = [&](xml::Node* e) {
        for (auto& c : e->children) {
            if (!c->isElement()) continue;
            std::string_view ln = c->localName();
            if (isNonRendered(ln)) continue;
            if (isShape(ln)) ++n;
            walk(c.get());
        }
    };
    walk(root);
    return n;
}

void finish(Ctx& ctx, const xml::Document* doc, const std::string& original) {
    Report& rep = ctx.rep();
    Result& res = ctx.res;
    for (const Issue& i : rep.issues) {
        if (i.severity == Severity::Error && !i.fixed) {
            rep.status = Status::Failed;
            res.data.clear();
            return;
        }
    }
    if (doc && ctx.changed) {
        rep.status = Status::Fixed;
        res.data = xml::serialize(*doc);
    } else {
        rep.status = Status::Ok;
        res.data = original;
    }
    rep.bytesOut = res.data.size();
    std::string why;
    rep.verified = verifyOutput(res.data, why);
    if (!rep.verified) {
        if (rep.status == Status::Fixed) {
            ctx.add(Severity::Error, false, "Internal check of the repaired file failed (" + why + "); nothing was written.");
            rep.status = Status::Failed;
            res.data.clear();
            rep.bytesOut = 0;
        } else {
            ctx.add(Severity::Warning, false, "Could not fully validate this file: " + why + ".");
        }
    }
}

void reportParseIssues(Ctx& ctx, std::string_view text, const xml::ParseResult& pr) {
    struct Group {
        size_t first;
        size_t count;
    };
    std::vector<std::string> order;
    std::map<std::string, Group> groups;
    for (const xml::Issue& is : pr.issues) {
        auto it = groups.find(is.message);
        if (it == groups.end()) {
            groups[is.message] = {is.offset, 1};
            order.push_back(is.message);
        } else {
            it->second.count++;
        }
    }
    for (const std::string& m : order) {
        const Group& g = groups[m];
        int line, col;
        xml::lineCol(text, g.first, line, col);
        std::string msg = "XML syntax: " + m + " (line " + std::to_string(line) + ", column " + std::to_string(col) + ")";
        if (g.count > 1) msg += " - " + formatCount(g.count) + " times";
        msg[12] = (char)std::toupper((unsigned char)msg[12]);
        ctx.add(Severity::Error, true, msg + ".");
    }
    if (pr.truncated)
        ctx.add(Severity::Warning, false,
                "The file was cut off, so part of the artwork is missing; everything up to the cut was kept. "
                "Export it again to get the complete drawing.");
}

void processSvg(Ctx& ctx, const std::string& text, const std::string& original);

// Finishes the result itself (like processSvg).
void processHtml(Ctx& ctx, const std::string& text, const std::string& original) {
    Report& rep = ctx.rep();
    rep.page = readPageInfo(text);
    size_t start = 0, end = 0, shapes = 0;
    if (largestEmbeddedSvg(text, start, end, shapes) && shapes >= kMinEmbeddedShapes) {
        ctx.add(Severity::Error, true,
                "The file is a web page (HTML) with the SVG artwork inside it; extracted the <svg> element (" +
                    formatCount(shapes) + " shapes).");
        processSvg(ctx, text.substr(start, end - start), original);
        rep.kind = "SVG embedded in a web page (" + rep.kind + ")";
        return;
    }
    const PageInfo& pi = rep.page;
    if (pi.isVectorizer) {
        rep.kind = "vectorizer.ai web page (HTML)";
        ctx.add(Severity::Error, false,
                "Not an SVG: this is the vectorizer.ai result page saved as HTML (the userscript saved the page "
                "instead of the drawing). The page loads the artwork at runtime, so this file contains no vector data "
                "and cannot be repaired.");
        if (!pi.sourceImage.empty())
            ctx.add(Severity::Info, false,
                    "Original image: " + pi.sourceImage + (pi.imageSize.empty() ? "" : " (" + pi.imageSize + " px)"));
        if (!pi.url.empty()) ctx.add(Severity::Info, false, "Export it again from: " + pi.url);
    } else {
        rep.kind = "web page (HTML)";
        ctx.add(Severity::Error, false,
                "Not an SVG: this is a saved web page (HTML) and it contains no embedded SVG artwork.");
        if (!pi.title.empty()) ctx.add(Severity::Info, false, "Page title: " + pi.title);
    }
    finish(ctx, nullptr, original);
}

void processSvg(Ctx& ctx, const std::string& text, const std::string& original) {
    Report& rep = ctx.rep();
    xml::ParseResult pr = xml::parse(text);
    xml::Node* root = pr.doc.root;
    if (!root) {
        rep.kind = "text file";
        ctx.add(Severity::Error, false, "Not an SVG: the file contains no XML elements.");
        return finish(ctx, nullptr, original);
    }
    if (util::iequals(root->localName(), "html")) return processHtml(ctx, text, original);
    if (root->localName() != "svg") {
        rep.kind = "XML document";
        ctx.add(Severity::Error, false, "Not an SVG: the root element is <" + root->name + ">, not <svg>.");
        return finish(ctx, nullptr, original);
    }
    reportParseIssues(ctx, text, pr);
    fixNamespaces(ctx, root);

    size_t scripts = 0, handlers = 0;
    stripScripts(root, scripts, handlers);
    if (scripts || handlers)
        ctx.add(Severity::Warning, true,
                "Removed " + formatCount(scripts) + " <script> element(s) and " + formatCount(handlers) +
                    " event handler / javascript: link(s). Scripts never run when an SVG is used as an image and are "
                    "a security risk.");

    Capture cap;
    if (detectCapture(root, cap)) {
        fixCapture(ctx, root, cap);
    } else {
        rep.kind = "SVG";
        fixPaths(ctx, root);
        fixCanvas(ctx, root);
    }
    rep.shapes = countShapes(root);
    if (rep.shapes == 0 && rep.status != Status::Failed) {
        bool already = false;
        for (const Issue& i : rep.issues) already = already || (i.severity == Severity::Error && !i.fixed);
        if (!already)
            ctx.add(Severity::Error, false,
                    "The SVG contains no shapes - the export captured an empty canvas. Export it again.");
    }
    finish(ctx, &pr.doc, original);
}

}  // namespace

Result process(const std::string& name, const std::string& bytes, const Options& opt) {
    Result res;
    res.extension = ".svg";
    Ctx ctx{res, opt};
    Report& rep = res.report;
    rep.name = name;
    rep.bytesIn = bytes.size();

    if (bytes.empty() || bytes.find_first_not_of('\0') == npos) {
        rep.kind = "empty file";
        ctx.add(Severity::Error, false,
                bytes.empty() ? "The file is empty (0 bytes): the export did not save anything."
                              : "The file contains only zero bytes: the download never completed.");
        rep.status = Status::Failed;
        return res;
    }
    if (const Signature* sig = sniff(bytes)) {
        rep.kind = sig->kind;
        if (sig->image) {
            ctx.add(Severity::Error, true,
                    std::string("Not an SVG: this is a ") + sig->kind + " saved with an .svg extension. Saved it with "
                    "the correct extension (" + sig->ext + ").");
            rep.status = Status::Converted;
            res.data = bytes;
            res.extension = sig->ext;
            rep.bytesOut = bytes.size();
        } else {
            ctx.add(Severity::Error, false,
                    std::string("Not an SVG: this is a ") + sig->kind + ". Extract it and drop the files inside.");
            rep.status = Status::Failed;
        }
        return res;
    }

    std::string text;
    decodeText(bytes, text, ctx);
    if (util::trim(text).empty()) {
        rep.kind = "empty file";
        ctx.add(Severity::Error, false, "The file contains only whitespace: the export did not save anything.");
        rep.status = Status::Failed;
        return res;
    }
    if (looksLikeHtml(text))
        processHtml(ctx, text, bytes);
    else
        processSvg(ctx, text, bytes);
    return res;
}

}  // namespace svgfix
