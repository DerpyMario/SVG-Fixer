// Unit and integration tests. Usage: svgfix_tests <path to tests/data>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "archive7z.hpp"
#include "pathdata.hpp"
#include "svgfix.hpp"
#include "util.hpp"
#include "xml.hpp"

namespace {

int g_checks = 0, g_failures = 0;
std::string g_data;

std::string str(const std::string& s) { return "\"" + s + "\""; }
template <typename T>
std::string str(const T& v) {
    return std::to_string(v);
}

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                                                  \
    do {                                                                                                \
        ++g_checks;                                                                                     \
        auto va_ = (a);                                                                                 \
        auto vb_ = (b);                                                                                 \
        if (!(va_ == vb_)) {                                                                            \
            ++g_failures;                                                                               \
            std::printf("FAIL %s:%d: %s == %s\n   got      %s\n   expected %s\n", __FILE__, __LINE__, #a, \
                        #b, str(va_).c_str(), str(vb_).c_str());                                        \
        }                                                                                               \
    } while (0)

#define CHECK_NEAR(a, b, eps) CHECK(std::fabs((a) - (b)) <= (eps))

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

std::string load(const std::string& rel) {
    std::string data, err;
    if (!util::readFile(util::pathFromUtf8(g_data + "/" + rel), data, err)) {
        std::printf("cannot read fixture %s: %s\n", rel.c_str(), err.c_str());
        ++g_failures;
    }
    return data;
}

bool hasIssue(const svgfix::Report& r, const std::string& part) {
    for (const svgfix::Issue& i : r.issues)
        if (contains(i.text, part)) return true;
    return false;
}

void testNumbers() {
    using pathdata::formatNumber;
    CHECK_EQ(formatNumber(228.92999267578125, 2), std::string("228.93"));
    CHECK_EQ(formatNumber(151.33999633789062, 2), std::string("151.34"));
    CHECK_EQ(formatNumber(100.0, 2), std::string("100"));
    CHECK_EQ(formatNumber(-0.0001, 2), std::string("0"));
    CHECK_EQ(formatNumber(-2.7308018862294717e-10, 2), std::string("0"));
    CHECK_EQ(formatNumber(1.5, -1), std::string("1.5"));
    CHECK_EQ(formatNumber(0.1 + 0.2, -1), std::string("0.3"));
    CHECK_EQ(util::formatCount(52946), std::string("52,946"));
    CHECK_EQ(util::formatBytes(1875117), std::string("1.79 MB"));
}

void testPathParse() {
    pathdata::Path p;
    pathdata::ParseResult r = pathdata::parse("M10-20L.5.5", p);
    CHECK(r.ok);
    CHECK_EQ(p.segs.size(), (size_t)2);
    CHECK_NEAR(p.segs[0].p[1], -20, 1e-12);
    CHECK_NEAR(p.segs[1].p[0], 0.5, 1e-12);
    CHECK_NEAR(p.segs[1].p[1], 0.5, 1e-12);

    r = pathdata::parse("M0 0a1 1 0 00.5.5", p);  // compact arc flags
    CHECK(r.ok);
    CHECK_EQ(p.segs.size(), (size_t)2);
    CHECK_NEAR(p.segs[1].p[5], 0.5, 1e-12);
    CHECK_NEAR(p.segs[1].p[6], 0.5, 1e-12);

    r = pathdata::parse("m1 2 3 4", p);  // implicit relative lineto
    CHECK(r.ok);
    CHECK_EQ(p.segs[1].cmd, 'l');
    CHECK_NEAR(p.segs[1].p[0], 4, 1e-12);
    CHECK_NEAR(p.segs[1].p[1], 6, 1e-12);

    r = pathdata::parse("M0 0L NaN 5 L10 10Z", p);
    CHECK(!r.ok);
    CHECK_EQ(r.invalidNumbers, 1);
    CHECK_EQ(p.segs.size(), (size_t)3);  // M, L 10 10, Z
    CHECK_NEAR(p.segs[1].p[0], 10, 1e-12);

    r = pathdata::parse("L10 10 M0 0 L5 5", p);  // must start with M
    CHECK(!r.ok);
    CHECK_EQ(p.segs.size(), (size_t)2);

    r = pathdata::parse("M0 0 C1 2 3 4 5", p);  // truncated curve
    CHECK(!r.ok);
    CHECK_EQ(p.segs.size(), (size_t)1);
}

void testPathFormat() {
    // Relative coordinates must not accumulate rounding error.
    std::string d = "M0 0";
    for (int i = 0; i < 1000; ++i) d += "l0.333 0.0004";
    pathdata::Path p;
    CHECK(pathdata::parse(d, p).ok);
    pathdata::FormatOptions fo;
    fo.precision = 2;
    std::string out = pathdata::format(p, fo);
    pathdata::Path q;
    CHECK(pathdata::parse(out, q).ok);
    pathdata::BBox exact, rounded;
    pathdata::endpointBounds(p, exact);
    pathdata::endpointBounds(q, rounded);
    CHECK_NEAR(rounded.x1, exact.x1, 0.0051);
    CHECK_NEAR(rounded.y1, exact.y1, 0.0051);

    // Zero-length segments disappear only when asked.
    pathdata::parse("M587.05 744.7L587.0500213748396 744.6999933691243L600 700", p);
    fo.dropZeroLength = true;
    pathdata::FormatStats st;
    CHECK_EQ(pathdata::format(p, fo, &st), std::string("M587.05 744.7L600 700"));
    CHECK_EQ(st.droppedZeroLength, 1);

    // A nearly closed large arc keeps its end point; rounding would make it vanish.
    pathdata::parse("M0 0A50 50 0 1 1 0.001 0", p);
    out = pathdata::format(p, fo, &st);
    CHECK(pathdata::parse(out, q).ok);
    CHECK_EQ(q.segs.size(), (size_t)2);
    CHECK_NEAR(q.segs[1].p[5], 0.001, 1e-12);
    CHECK_EQ(st.preciseArcs, 1);
}

void testBoundsAndTransforms() {
    pathdata::Path p;
    pathdata::BBox b;
    pathdata::parse("M0 0Q50 100 100 0", p);
    pathdata::bounds(p, pathdata::Matrix(), b);
    CHECK_NEAR(b.y1, 50, 1e-9);
    CHECK_NEAR(b.x1, 100, 1e-9);

    b = pathdata::BBox();
    pathdata::parse("M0 0A50 50 0 0 1 100 0", p);  // upper semicircle
    pathdata::bounds(p, pathdata::Matrix(), b);
    CHECK_NEAR(b.y0, -50, 0.05);
    CHECK_NEAR(b.y1, 0, 1e-9);

    pathdata::Matrix m;
    CHECK(pathdata::parseTransform("translate(10,20) scale(2)", m));
    double x, y;
    m.apply(1, 1, x, y);
    CHECK_NEAR(x, 12, 1e-12);
    CHECK_NEAR(y, 22, 1e-12);
    CHECK(pathdata::parseTransform("rotate(90)", m));
    m.apply(1, 0, x, y);
    CHECK_NEAR(x, 0, 1e-12);
    CHECK_NEAR(y, 1, 1e-12);
    CHECK(!pathdata::parseTransform("matrix(1 2 3)", m));
}

void testXml() {
    // Truncated in the middle of an element: keep everything before it.
    xml::ParseResult r = xml::parse("<svg xmlns=\"http://www.w3.org/2000/svg\"><path d=\"M0 0L10 10\"/><path d=\"M1");
    CHECK(r.truncated);
    CHECK(r.doc.root != nullptr);
    CHECK_EQ(r.doc.root->children.size(), (size_t)1);

    // Unclosed shapes become siblings, not thousands of nested paths.
    r = xml::parse("<svg><path d=\"M0 0L1 1\"><path d=\"M0 0L2 2\"></svg>");
    CHECK_EQ(r.doc.root->children.size(), (size_t)2);
    CHECK(!r.issues.empty());

    r = xml::parse("<svg><title>A &amp; B&nbsp;&copy; & C</title></svg>");
    CHECK_EQ(r.doc.root->children[0]->children[0]->value, std::string("A & B\xC2\xA0\xC2\xA9 & C"));
    CHECK_EQ(r.issues.size(), (size_t)3);  // &nbsp;, &copy;, bare &

    r = xml::parse("<svg a=\"1\" a=\"2\" b=3 hidden/>");
    CHECK_EQ(*r.doc.root->attr("a"), std::string("2"));
    CHECK_EQ(*r.doc.root->attr("b"), std::string("3"));
    CHECK(r.doc.root->attr("hidden") != nullptr);
    CHECK_EQ(r.issues.size(), (size_t)3);

    r = xml::parse("<svg><g></svg></g>junk<svg/>");
    CHECK(r.doc.root != nullptr);
    CHECK(!r.issues.empty());
    std::string out = xml::serialize(r.doc);
    CHECK(xml::parse(out).issues.empty());

    r = xml::parse("<svg t=\"a&lt;b &quot;c&quot;\">x &lt; y</svg>");
    CHECK(r.issues.empty());
    out = xml::serialize(r.doc);
    CHECK(contains(out, "t=\"a&lt;b &quot;c&quot;\""));
    CHECK(contains(out, ">x &lt; y<"));

    // Entities from an internal DTD subset (old Illustrator exports).
    r = xml::parse("<!DOCTYPE svg [<!ENTITY ns_svg \"http://www.w3.org/2000/svg\">]><svg xmlns=\"&ns_svg;\"/>");
    CHECK(r.issues.empty());
    CHECK_EQ(*r.doc.root->attr("xmlns"), std::string("http://www.w3.org/2000/svg"));
}

svgfix::Result run(const std::string& name, const std::string& bytes, svgfix::Options opt = {}) {
    return svgfix::process(name, bytes, opt);
}

void testCapture() {
    svgfix::Result r = run("capture.svg", load("capture.svg"));
    const svgfix::Report& rep = r.report;
    CHECK(rep.status == svgfix::Status::Fixed);
    CHECK(rep.verified);
    CHECK_NEAR(rep.width, 100, 1e-9);
    CHECK_NEAR(rep.height, 50, 1e-9);
    CHECK_EQ(rep.gapFillers, (size_t)1);
    CHECK_EQ(rep.fills, (size_t)2);
    CHECK(contains(r.data, "width=\"100\" height=\"50\" viewBox=\"0 0 100 50\""));
    CHECK(!contains(r.data, "transform"));
    CHECK(!contains(r.data, "fill-opacity"));
    CHECK(contains(r.data, "<path d=\"M0 0L100 0L100 50L0 50Z\" fill=\"#c3c6cb\"/>"));
    CHECK(contains(r.data, "Q30 21 40.75 10.5A"));  // float32 noise gone, zero-length L dropped
    CHECK(hasIssue(rep, "No width, height or viewBox"));
    CHECK(hasIssue(rep, "zoom transform"));
    CHECK(hasIssue(rep, "gap-filler"));

    svgfix::Options strip;
    strip.stripGapFillers = true;
    r = run("capture.svg", load("capture.svg"), strip);
    CHECK_EQ(r.report.gapFillers, (size_t)0);
    CHECK(!contains(r.data, "fill=\"none\""));

    // Cut off in the middle of the last path: the rest is kept and closed.
    std::string cut = load("capture.svg");
    cut = cut.substr(0, cut.find("Q30"));
    r = run("cut.svg", cut);
    CHECK(r.report.status == svgfix::Status::Fixed);
    CHECK(r.report.verified);
    CHECK(hasIssue(r.report, "cut off"));
    CHECK_EQ(r.report.fills, (size_t)1);

    // Invalid numbers inside a capture.
    std::string bad = load("capture.svg");
    bad.replace(bad.find("L100 50.000000745058"), 20, "L NaN undefined");
    r = run("bad.svg", bad);
    CHECK(r.report.status == svgfix::Status::Fixed);
    CHECK(hasIssue(r.report, "invalid path data"));
    CHECK(!contains(r.data, "NaN"));
}

void testPages() {
    svgfix::Result r = run("page.svg", load("page.svg"));
    const svgfix::Report& rep = r.report;
    CHECK(rep.status == svgfix::Status::Failed);
    CHECK(r.data.empty());
    CHECK(rep.page.isVectorizer);
    CHECK_EQ(rep.page.sourceImage, std::string("holiday_photo.jpeg"));
    CHECK_EQ(rep.page.imageSize, std::string("1376 x 768"));
    CHECK_EQ(rep.page.url, std::string("https://vectorizer.ai/images/1700000000000-0123456789abcdef-test"));

    // A page that does contain the artwork inline: extract it.
    std::string art = "<svg viewBox=\"0 0 100 100\">";
    for (int i = 0; i < 60; ++i) art += "<path d=\"M" + std::to_string(i) + " 0L" + std::to_string(i) + " 100\" stroke=\"#000\"/>";
    art += "</svg>";
    r = run("inline.svg", "<!DOCTYPE html><html><body><svg viewBox=\"0 0 24 24\"><path d=\"M0 0h1\"/></svg>" + art + "</body></html>");
    CHECK(r.report.status == svgfix::Status::Fixed);
    CHECK(r.report.verified);
    CHECK_EQ(r.report.shapes, (size_t)60);
    CHECK(contains(r.data, "xmlns=\"http://www.w3.org/2000/svg\""));
}

void testGeneric() {
    // Already valid: written back byte for byte.
    std::string ok = load("sub/ok.svg");
    svgfix::Result r = run("ok.svg", ok);
    CHECK(r.report.status == svgfix::Status::Ok);
    CHECK(r.data == ok);
    CHECK(r.report.verified);

    r = run("ns.svg", "<svg width=\"10\" height=\"10\"><image xlink:href=\"a.png\" width=\"10\" height=\"10\"/></svg>");
    CHECK(r.report.status == svgfix::Status::Fixed);
    CHECK(contains(r.data, "xmlns=\"http://www.w3.org/2000/svg\""));
    CHECK(contains(r.data, "xmlns:xlink=\"http://www.w3.org/1999/xlink\""));
    CHECK(contains(r.data, "viewBox=\"0 0 10 10\""));
    CHECK(r.report.verified);

    // No size at all: measured from the artwork, through transforms.
    r = run("nosize.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\"><g transform=\"translate(10 5)\"><circle cx=\"20\" cy=\"20\" r=\"10\"/></g></svg>");
    CHECK(r.report.status == svgfix::Status::Fixed);
    CHECK(contains(r.data, "viewBox=\"0 0 40 35\""));

    r = run("nan.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 9 9\"><path d=\"M1 1L2 NaN L3 3Z\"/></svg>");
    CHECK(r.report.status == svgfix::Status::Fixed);
    CHECK(contains(r.data, "d=\"M1 1L3 3Z\""));

    r = run("script.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 9 9\" onload=\"x()\"><script>x()</script><rect width=\"1\" height=\"1\"/></svg>");
    CHECK(r.report.status == svgfix::Status::Fixed);
    CHECK(!contains(r.data, "script"));
    CHECK(!contains(r.data, "onload"));

    // Junk in front of the XML declaration, NUL bytes and Windows-1252 text.
    std::string junk = "\r\n<?xml version=\"1.0\"?><svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 9 9\"><title>caf\xE9";
    junk += '\0';
    junk += "</title><rect width=\"1\" height=\"1\"/></svg>";
    r = run("junk.svg", junk);
    CHECK(r.report.status == svgfix::Status::Fixed);
    CHECK(contains(r.data, "caf\xC3\xA9</title>"));
    CHECK(r.report.verified);

    // UTF-16 with byte order mark.
    std::string u8 = "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 9 9\"><rect width=\"1\" height=\"1\"/></svg>";
    std::string u16 = "\xFF\xFE";
    for (char c : u8) {
        u16 += c;
        u16 += '\0';
    }
    r = run("utf16.svg", u16);
    CHECK(r.report.status == svgfix::Status::Fixed);
    CHECK(contains(r.data, "<rect width=\"1\" height=\"1\"/>"));

    r = run("empty.svg", "");
    CHECK(r.report.status == svgfix::Status::Failed);
    r = run("zeros.svg", std::string(64, '\0'));
    CHECK(r.report.status == svgfix::Status::Failed);
    r = run("nothing.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\"></svg>");
    CHECK(r.report.status == svgfix::Status::Failed);
    r = run("photo.svg", std::string("\x89PNG\r\n\x1a\n\0\0\0\rIHDR", 16));
    CHECK(r.report.status == svgfix::Status::Converted);
    CHECK_EQ(r.extension, std::string(".png"));
    r = run("doc.svg", "<?xml version=\"1.0\"?><note>hi</note>");
    CHECK(r.report.status == svgfix::Status::Failed);
}

void testArchive() {
    for (const char* name : {"sample.7z", "sample_ppmd.7z"}) {
        std::string bytes = load(name);
        CHECK(archive7z::isArchive(bytes));
        std::vector<archive7z::Entry> entries;
        std::string err;
        CHECK(archive7z::extract(bytes, entries, err));
        bool capture = false, sub = false, notes = false;
        for (const archive7z::Entry& e : entries) {
            CHECK(e.error.empty());
            if (e.name == "capture.svg") capture = e.data == load("capture.svg");
            if (e.name == "sub/ok.svg") sub = e.data == load("sub/ok.svg");
            if (e.name == "notes.txt") notes = e.data == "not an svg\n";
        }
        CHECK(capture);
        CHECK(sub);
        CHECK(notes);

        std::vector<archive7z::Entry> broken;
        std::string cut = bytes.substr(0, bytes.size() / 2);
        bool opened = archive7z::extract(cut, broken, err);
        CHECK(!opened);  // headers live at the end of a 7z archive
    }
}

}  // namespace

int main(int argc, char** argv) {
    g_data = argc > 1 ? argv[1] : "tests/data";
    testNumbers();
    testPathParse();
    testPathFormat();
    testBoundsAndTransforms();
    testXml();
    testCapture();
    testPages();
    testGeneric();
    testArchive();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
