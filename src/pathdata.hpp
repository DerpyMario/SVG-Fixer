// SVG path data ("d" attribute): parsing, validation, re-serialization with
// rounding, and exact bounding boxes. Also SVG transform lists.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace pathdata {

struct Matrix {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    void apply(double x, double y, double& ox, double& oy) const {
        ox = a * x + c * y + e;
        oy = b * x + d * y + f;
    }
    Matrix operator*(const Matrix& m) const;  // m is applied first, then *this
};
bool parseTransform(std::string_view s, Matrix& m);

struct BBox {
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    void add(double x, double y) {
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
    }
    void add(const BBox& o) {
        if (!o.empty()) {
            add(o.x0, o.y0);
            add(o.x1, o.y1);
        }
    }
    bool empty() const { return x0 > x1 || y0 > y1; }
};

// One path command with absolute values. `cmd` is the letter as written
// (upper case = absolute, lower case = relative); implicit repeats keep the
// letter and the pairs after a moveto become L/l. Layout of p[]:
//   M L T: x y      H: x      V: y      Z: -
//   C: x1 y1 x2 y2 x y        S: x2 y2 x y        Q: x1 y1 x y
//   A: rx ry x-axis-rotation large-arc sweep x y
struct Segment {
    char cmd = 'M';
    double p[7] = {0, 0, 0, 0, 0, 0, 0};
};

struct Path {
    std::vector<Segment> segs;
    bool hasDrawing() const;  // anything besides movetos
};

struct ParseResult {
    bool ok = true;              // false if anything had to be dropped
    int droppedSegments = 0;     // invalid segments removed
    int invalidNumbers = 0;      // NaN / Infinity / undefined tokens
    std::string firstError;      // human readable description of the first problem
    size_t numbers = 0;          // numbers seen
    size_t noisyNumbers = 0;     // numbers with more than 4 decimals (or an exponent)
    std::string noisyExample;    // a typical one, e.g. "228.92999267578125"
};

ParseResult parse(std::string_view d, Path& out);

struct FormatOptions {
    int precision = -1;           // decimals; < 0 keeps full precision
    bool dropZeroLength = false;  // remove line/arc segments that collapse to a point
};

struct FormatStats {
    int droppedZeroLength = 0;
    int preciseArcs = 0;  // arcs written with extra precision to keep their shape
};

std::string format(const Path& p, const FormatOptions& opt, FormatStats* stats = nullptr);

// Exact bounds of the geometry (curve extrema included) after applying m.
void bounds(const Path& p, const Matrix& m, BBox& box);
// Bounds of the on-curve end points only.
void endpointBounds(const Path& p, BBox& box);

// Number to text: fixed `precision` decimals with trailing zeros removed, or
// the shortest exact round-trip form when precision < 0.
std::string formatNumber(double v, int precision);

}  // namespace pathdata
