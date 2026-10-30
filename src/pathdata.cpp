#include "pathdata.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pathdata {

namespace {

constexpr double kPi = 3.14159265358979323846;

bool isWs(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

bool isCommand(char c) { return c != 0 && std::strchr("MmLlHhVvCcSsQqTtAaZz", c) != nullptr; }

int argCount(char upper) {
    switch (upper) {
        case 'M': case 'L': case 'T': return 2;
        case 'H': case 'V': return 1;
        case 'C': return 6;
        case 'S': case 'Q': return 4;
        case 'A': return 7;
        default: return 0;
    }
}

double readBack(const std::string& s) { return std::strtod(s.c_str(), nullptr); }

double roundTo(double v, int precision) { return precision < 0 ? v : readBack(formatNumber(v, precision)); }

// Tokens that script-generated path data contains when a number went wrong.
const char* const kBadWords[] = {"-Infinity", "+Infinity", "Infinity", "-NaN", "NaN", "undefined", "null",
                                 "-inf", "inf", "nan"};

class Scanner {
public:
    explicit Scanner(std::string_view s) : s_(s) {}

    size_t pos() const { return i_; }
    void skipWs() {
        while (i_ < s_.size() && isWs(s_[i_])) ++i_;
    }
    void skipSep() {
        skipWs();
        if (i_ < s_.size() && s_[i_] == ',') {
            ++i_;
            skipWs();
        }
    }
    bool atEnd() {
        skipWs();
        return i_ >= s_.size();
    }
    bool peekCommand(char& c) {
        skipWs();
        if (i_ < s_.size() && isCommand(s_[i_])) {
            c = s_[i_];
            return true;
        }
        return false;
    }
    void advance() { ++i_; }

    bool badWordLength(size_t& len) const {
        for (const char* w : kBadWords) {
            size_t n = std::strlen(w);
            if (s_.substr(i_, n) == w) {
                len = n;
                return true;
            }
        }
        return false;
    }

    // 0 = number read, 1 = no number here, 2 = an invalid token was consumed
    int number(double& v, ParseResult& r) {
        size_t len;
        if (badWordLength(len)) {
            i_ += len;
            return 2;
        }
        size_t k = i_;
        if (k < s_.size() && (s_[k] == '+' || s_[k] == '-')) ++k;
        size_t intStart = k;
        while (k < s_.size() && std::isdigit((unsigned char)s_[k])) ++k;
        bool digits = k > intStart;
        if (k < s_.size() && s_[k] == '.') {
            size_t fracStart = ++k;
            while (k < s_.size() && std::isdigit((unsigned char)s_[k])) ++k;
            digits = digits || k > fracStart;
        }
        if (!digits) return 1;
        if (k < s_.size() && (s_[k] == 'e' || s_[k] == 'E')) {
            size_t e = k + 1;
            if (e < s_.size() && (s_[e] == '+' || s_[e] == '-')) ++e;
            if (e < s_.size() && std::isdigit((unsigned char)s_[e])) {
                while (e < s_.size() && std::isdigit((unsigned char)s_[e])) ++e;
                k = e;
            }
        }
        std::string tok(s_.substr(i_, k - i_));
        i_ = k;
        v = readBack(tok);
        if (!std::isfinite(v)) return 2;  // e.g. 1e999
        r.numbers++;
        size_t dot = tok.find('.'), exp = tok.find_first_of("eE");
        size_t decimals = dot == std::string::npos ? 0 : (exp == std::string::npos ? tok.size() : exp) - dot - 1;
        if (decimals > 4 || exp != std::string::npos) {
            r.noisyNumbers++;
            // prefer a plain decimal as the example over exponent forms
            if (r.noisyExample.empty() || (exp == std::string::npos && r.noisyExample.find_first_of("eE") != std::string::npos))
                r.noisyExample = tok;
        }
        return 0;
    }

    int flag(double& v) {
        if (i_ < s_.size() && (s_[i_] == '0' || s_[i_] == '1')) {
            v = s_[i_] - '0';
            ++i_;
            return 0;
        }
        size_t len;
        if (badWordLength(len)) {
            i_ += len;
            return 2;
        }
        return 1;
    }

    // Skip to the next command letter (the way renderers would never get
    // there, but a repair can keep the rest of the path).
    void resync() {
        while (i_ < s_.size()) {
            size_t len;
            if (badWordLength(len)) {
                i_ += len;
            } else if (isCommand(s_[i_])) {
                return;
            } else {
                ++i_;
            }
        }
    }

private:
    std::string_view s_;
    size_t i_ = 0;
};

// Cubic Bezier extrema, in already transformed coordinates.
void addCubic(BBox& b, double x0, double y0, double x1, double y1, double x2, double y2, double x3, double y3) {
    b.add(x0, y0);
    b.add(x3, y3);
    const double P[2][4] = {{x0, x1, x2, x3}, {y0, y1, y2, y3}};
    for (int axis = 0; axis < 2; ++axis) {
        const double* p = P[axis];
        double A = p[1] - p[0], B = p[2] - p[1], C = p[3] - p[2];
        double qa = A - 2 * B + C, qb = 2 * (B - A), qc = A;
        double roots[2];
        int n = 0;
        if (std::fabs(qa) < 1e-12) {
            if (std::fabs(qb) > 1e-12) roots[n++] = -qc / qb;
        } else {
            double disc = qb * qb - 4 * qa * qc;
            if (disc >= 0) {
                double sq = std::sqrt(disc);
                roots[n++] = (-qb + sq) / (2 * qa);
                roots[n++] = (-qb - sq) / (2 * qa);
            }
        }
        for (int i = 0; i < n; ++i) {
            double t = roots[i];
            if (t <= 0 || t >= 1) continue;
            double u = 1 - t;
            double w0 = u * u * u, w1 = 3 * u * u * t, w2 = 3 * u * t * t, w3 = t * t * t;
            b.add(w0 * x0 + w1 * x1 + w2 * x2 + w3 * x3, w0 * y0 + w1 * y1 + w2 * y2 + w3 * y3);
        }
    }
}

// Endpoint-parameterized arc to cubic Beziers (SVG 1.1 implementation notes F.6.5).
void arcToCubics(double x1, double y1, double rx, double ry, double phiDeg, bool largeArc, bool sweep, double x2,
                 double y2, std::vector<std::array<double, 6>>& out) {
    if (x1 == x2 && y1 == y2) return;  // omitted by renderers
    rx = std::fabs(rx);
    ry = std::fabs(ry);
    if (rx == 0 || ry == 0) {  // treated as a straight line
        out.push_back({x1, y1, x2, y2, x2, y2});
        return;
    }
    double phi = phiDeg * kPi / 180, cp = std::cos(phi), sp = std::sin(phi);
    double dx2 = (x1 - x2) / 2, dy2 = (y1 - y2) / 2;
    double x1p = cp * dx2 + sp * dy2, y1p = -sp * dx2 + cp * dy2;
    double lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
    if (lambda > 1) {
        double s = std::sqrt(lambda);
        rx *= s;
        ry *= s;
    }
    double rx2 = rx * rx, ry2 = ry * ry;
    double num = rx2 * ry2 - rx2 * y1p * y1p - ry2 * x1p * x1p;
    double den = rx2 * y1p * y1p + ry2 * x1p * x1p;
    double coef = den > 0 ? std::sqrt(std::max(0.0, num / den)) : 0;
    if (largeArc == sweep) coef = -coef;
    double cxp = coef * rx * y1p / ry, cyp = -coef * ry * x1p / rx;
    double cx = cp * cxp - sp * cyp + (x1 + x2) / 2, cy = sp * cxp + cp * cyp + (y1 + y2) / 2;
    auto angle = [](double ux, double uy, double vx, double vy) { return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy); };
    double ux = (x1p - cxp) / rx, uy = (y1p - cyp) / ry, vx = (-x1p - cxp) / rx, vy = (-y1p - cyp) / ry;
    double theta = angle(1, 0, ux, uy);
    double delta = angle(ux, uy, vx, vy);
    if (!sweep && delta > 0) delta -= 2 * kPi;
    if (sweep && delta < 0) delta += 2 * kPi;
    int n = std::max(1, (int)std::ceil(std::fabs(delta) / (kPi / 2) - 1e-9));
    double step = delta / n, k = 4.0 / 3.0 * std::tan(step / 4);
    auto map = [&](double u, double v, double& X, double& Y) {
        X = cx + rx * cp * u - ry * sp * v;
        Y = cy + rx * sp * u + ry * cp * v;
    };
    for (int i = 0; i < n; ++i) {
        double t2 = theta + step;
        std::array<double, 6> c{};
        map(std::cos(theta) - k * std::sin(theta), std::sin(theta) + k * std::cos(theta), c[0], c[1]);
        map(std::cos(t2) + k * std::sin(t2), std::sin(t2) - k * std::cos(t2), c[2], c[3]);
        map(std::cos(t2), std::sin(t2), c[4], c[5]);
        out.push_back(c);
        theta = t2;
    }
    out.back()[4] = x2;
    out.back()[5] = y2;
}

}  // namespace

Matrix Matrix::operator*(const Matrix& m) const {
    Matrix r;
    r.a = a * m.a + c * m.b;
    r.b = b * m.a + d * m.b;
    r.c = a * m.c + c * m.d;
    r.d = b * m.c + d * m.d;
    r.e = a * m.e + c * m.f + e;
    r.f = b * m.e + d * m.f + f;
    return r;
}

bool parseTransform(std::string_view s, Matrix& out) {
    Matrix m;
    size_t i = 0;
    auto skip = [&] {
        while (i < s.size() && (isWs(s[i]) || s[i] == ',')) ++i;
    };
    skip();
    while (i < s.size()) {
        size_t nameStart = i;
        while (i < s.size() && std::isalpha((unsigned char)s[i])) ++i;
        std::string name(s.substr(nameStart, i - nameStart));
        while (i < s.size() && isWs(s[i])) ++i;
        if (name.empty() || i >= s.size() || s[i] != '(') return false;
        ++i;
        double a[6];
        int n = 0;
        for (;;) {
            skip();
            if (i >= s.size()) return false;
            if (s[i] == ')') {
                ++i;
                break;
            }
            if (n == 6) return false;
            std::string tmp(s.substr(i, 64));
            char* end = nullptr;
            double v = std::strtod(tmp.c_str(), &end);
            size_t used = (size_t)(end - tmp.c_str());
            if (used == 0 || !std::isfinite(v)) return false;
            a[n++] = v;
            i += used;
        }
        Matrix t;
        if (name == "matrix" && n == 6) {
            t.a = a[0]; t.b = a[1]; t.c = a[2]; t.d = a[3]; t.e = a[4]; t.f = a[5];
        } else if (name == "translate" && (n == 1 || n == 2)) {
            t.e = a[0];
            t.f = n == 2 ? a[1] : 0;
        } else if (name == "scale" && (n == 1 || n == 2)) {
            t.a = a[0];
            t.d = n == 2 ? a[1] : a[0];
        } else if (name == "rotate" && (n == 1 || n == 3)) {
            double r = a[0] * kPi / 180;
            Matrix rot;
            rot.a = std::cos(r); rot.b = std::sin(r); rot.c = -std::sin(r); rot.d = std::cos(r);
            if (n == 3) {
                Matrix to, back;
                to.e = a[1]; to.f = a[2];
                back.e = -a[1]; back.f = -a[2];
                t = to * rot * back;
            } else {
                t = rot;
            }
        } else if (name == "skewX" && n == 1) {
            t.c = std::tan(a[0] * kPi / 180);
        } else if (name == "skewY" && n == 1) {
            t.b = std::tan(a[0] * kPi / 180);
        } else {
            return false;
        }
        m = m * t;
        skip();
    }
    out = m;
    return true;
}

bool Path::hasDrawing() const {
    for (const Segment& s : segs) {
        char up = (char)std::toupper((unsigned char)s.cmd);
        if (up != 'M' && up != 'Z') return true;
    }
    return false;
}

ParseResult parse(std::string_view d, Path& out) {
    ParseResult r;
    out.segs.clear();
    Scanner sc(d);
    double cx = 0, cy = 0, sx = 0, sy = 0;
    char cmd = 0;
    bool started = false;
    auto fail = [&](const std::string& msg) {
        if (r.ok) r.firstError = msg;
        r.ok = false;
        r.droppedSegments++;
        sc.resync();
    };
    while (!sc.atEnd()) {
        char c;
        if (sc.peekCommand(c)) {
            sc.advance();
            cmd = c;
        } else if (cmd == 0) {
            fail("the path data does not start with a command");
            continue;
        } else if (cmd == 'Z' || cmd == 'z') {
            fail("numbers follow a Z command");
            continue;
        }
        const char up = (char)std::toupper((unsigned char)cmd);
        const bool rel = cmd != up;
        if (!started && up != 'M') {
            fail(std::string("the path data starts with '") + cmd + "' instead of M");
            continue;
        }
        if (up == 'Z') {
            Segment s;
            s.cmd = cmd;
            out.segs.push_back(s);
            cx = sx;
            cy = sy;
            continue;
        }
        const int n = argCount(up);
        double a[7] = {0, 0, 0, 0, 0, 0, 0};
        int status = 0;
        for (int k = 0; k < n && status == 0; ++k) {
            sc.skipSep();
            status = (up == 'A' && (k == 3 || k == 4)) ? sc.flag(a[k]) : sc.number(a[k], r);
        }
        if (status != 0) {
            if (status == 2) {
                r.invalidNumbers++;
                fail(std::string("an invalid number (NaN/Infinity/undefined) in a ") + cmd + " command");
            } else {
                fail(std::string("an incomplete or malformed ") + cmd + " command");
            }
            continue;
        }
        Segment s;
        s.cmd = cmd;
        const double ox = rel ? cx : 0, oy = rel ? cy : 0;
        switch (up) {
            case 'M':
                s.p[0] = a[0] + ox;
                s.p[1] = a[1] + oy;
                cx = sx = s.p[0];
                cy = sy = s.p[1];
                started = true;
                cmd = rel ? 'l' : 'L';  // extra pairs after a moveto are linetos
                break;
            case 'L':
            case 'T':
                s.p[0] = a[0] + ox;
                s.p[1] = a[1] + oy;
                cx = s.p[0];
                cy = s.p[1];
                break;
            case 'H':
                s.p[0] = a[0] + ox;
                cx = s.p[0];
                break;
            case 'V':
                s.p[0] = a[0] + oy;
                cy = s.p[0];
                break;
            case 'C':
            case 'S':
            case 'Q':
                for (int k = 0; k < n; k += 2) {
                    s.p[k] = a[k] + ox;
                    s.p[k + 1] = a[k + 1] + oy;
                }
                cx = s.p[n - 2];
                cy = s.p[n - 1];
                break;
            case 'A':
                for (int k = 0; k < 5; ++k) s.p[k] = a[k];
                s.p[5] = a[5] + ox;
                s.p[6] = a[6] + oy;
                cx = s.p[5];
                cy = s.p[6];
                break;
        }
        out.segs.push_back(s);
    }
    return r;
}

std::string formatNumber(double v, int precision) {
    char buf[64];
    if (!std::isfinite(v)) v = 0;
    if (std::fabs(v) >= 1e15) {
        std::snprintf(buf, sizeof buf, "%.17g", v);
        return buf;
    }
    if (precision >= 0) {
        std::snprintf(buf, sizeof buf, "%.*f", std::min(precision, 12), v);
    } else {
        // shortest fixed-point text within 15 significant digits: lossless
        // for any real drawing, and free of noise like 0.30000000000000004
        bool found = false;
        for (int p = 0; p <= 17 && !found; ++p) {
            std::snprintf(buf, sizeof buf, "%.*f", p, v);
            double back = std::strtod(buf, nullptr);
            found = back == v || std::fabs(back - v) <= std::fabs(v) * 1e-15;
        }
        if (!found) std::snprintf(buf, sizeof buf, "%.15g", v);
    }
    std::string s = buf;
    if (s.find('.') != std::string::npos && s.find_first_of("eE") == std::string::npos) {
        while (s.back() == '0') s.pop_back();
        if (s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}

std::string format(const Path& p, const FormatOptions& opt, FormatStats* stats) {
    std::string out;
    out.reserve(p.segs.size() * 28);
    const int prec = opt.precision;
    double ox = 0, oy = 0, osx = 0, osy = 0;  // current point / subpath start as written
    double ex = 0, ey = 0, esx = 0, esy = 0;  // exact current point / subpath start
    std::string seg;

    auto num = [](std::string& s, const std::string& t) {
        if (!s.empty() && !std::isalpha((unsigned char)s.back())) s += ' ';
        s += t;
    };
    // Relative coordinates are derived from rounded absolute positions, so
    // rounding never accumulates along a path.
    auto coord = [&](std::string& s, double v, double base, bool rel, int pr) {
        std::string t = rel ? formatNumber(roundTo(v, pr) - base, pr) : formatNumber(v, pr);
        num(s, t);
        return rel ? base + readBack(t) : readBack(t);
    };

    for (const Segment& s : p.segs) {
        const char up = (char)std::toupper((unsigned char)s.cmd);
        const bool rel = s.cmd != up;
        seg.assign(1, s.cmd);
        double nx = ox, ny = oy;
        bool drop = false;
        switch (up) {
            case 'M':
                nx = coord(seg, s.p[0], ox, rel, prec);
                ny = coord(seg, s.p[1], oy, rel, prec);
                ex = esx = s.p[0];
                ey = esy = s.p[1];
                out += seg;
                ox = osx = nx;
                oy = osy = ny;
                continue;
            case 'Z':
                out += seg;
                ox = osx;
                oy = osy;
                ex = esx;
                ey = esy;
                continue;
            case 'L':
            case 'T':
                nx = coord(seg, s.p[0], ox, rel, prec);
                ny = coord(seg, s.p[1], oy, rel, prec);
                drop = up == 'L' && nx == ox && ny == oy;
                ex = s.p[0];
                ey = s.p[1];
                break;
            case 'H':
                nx = coord(seg, s.p[0], ox, rel, prec);
                drop = nx == ox;
                ex = s.p[0];
                break;
            case 'V':
                ny = coord(seg, s.p[0], oy, rel, prec);
                drop = ny == oy;
                ey = s.p[0];
                break;
            case 'C':
            case 'S':
            case 'Q': {
                const int n = argCount(up);
                bool degenerate = true;
                for (int k = 0; k < n; k += 2) {
                    double x = coord(seg, s.p[k], ox, rel, prec);
                    double y = coord(seg, s.p[k + 1], oy, rel, prec);
                    degenerate = degenerate && x == ox && y == oy;
                    nx = x;
                    ny = y;
                }
                drop = up != 'S' && degenerate;  // S could still change a following S
                ex = s.p[n - 2];
                ey = s.p[n - 1];
                break;
            }
            case 'A': {
                const double chordExact = std::hypot(s.p[5] - ex, s.p[6] - ey);
                const double radius = std::max(std::fabs(s.p[0]), std::fabs(s.p[1]));
                int pr = prec;
                for (int attempt = 0; attempt < 2; ++attempt) {
                    seg.assign(1, s.cmd);
                    num(seg, formatNumber(s.p[0], pr));
                    num(seg, formatNumber(s.p[1], pr));
                    num(seg, formatNumber(s.p[2], pr));
                    seg += s.p[3] != 0 ? " 1" : " 0";
                    seg += s.p[4] != 0 ? " 1" : " 0";
                    nx = coord(seg, s.p[5], ox, rel, pr);
                    ny = coord(seg, s.p[6], oy, rel, pr);
                    // Rounding the end point of a nearly closed large arc can
                    // collapse it (renderers then skip it) or move its centre
                    // a lot; keep such arcs at full precision.
                    const double chordOut = std::hypot(nx - ox, ny - oy);
                    bool risky = pr >= 0 && chordExact > 0 &&
                                 (chordOut == 0 || (s.p[3] != 0 && radius > 10 * chordExact));
                    if (!risky) break;
                    pr = -1;
                    if (stats) stats->preciseArcs++;
                }
                drop = nx == ox && ny == oy;
                ex = s.p[5];
                ey = s.p[6];
                break;
            }
        }
        if (drop && opt.dropZeroLength) {
            if (stats) stats->droppedZeroLength++;
            continue;
        }
        out += seg;
        ox = nx;
        oy = ny;
    }
    return out;
}

void endpointBounds(const Path& p, BBox& box) {
    double cx = 0, cy = 0, sx = 0, sy = 0;
    for (const Segment& s : p.segs) {
        switch (std::toupper((unsigned char)s.cmd)) {
            case 'M':
                cx = sx = s.p[0];
                cy = sy = s.p[1];
                break;
            case 'Z':
                cx = sx;
                cy = sy;
                break;
            case 'L': case 'T': cx = s.p[0]; cy = s.p[1]; break;
            case 'H': cx = s.p[0]; break;
            case 'V': cy = s.p[0]; break;
            case 'C': cx = s.p[4]; cy = s.p[5]; break;
            case 'S': case 'Q': cx = s.p[2]; cy = s.p[3]; break;
            case 'A': cx = s.p[5]; cy = s.p[6]; break;
        }
        box.add(cx, cy);
    }
}

void bounds(const Path& p, const Matrix& m, BBox& box) {
    double cx = 0, cy = 0, sx = 0, sy = 0;
    double lcx = 0, lcy = 0;  // last control point, for S/T reflection
    char last = 0;
    std::vector<std::array<double, 6>> arcs;
    auto cubic = [&](double x1, double y1, double x2, double y2, double x3, double y3) {
        double a[8];
        m.apply(cx, cy, a[0], a[1]);
        m.apply(x1, y1, a[2], a[3]);
        m.apply(x2, y2, a[4], a[5]);
        m.apply(x3, y3, a[6], a[7]);
        addCubic(box, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
    };
    auto quad = [&](double x1, double y1, double x2, double y2) {
        cubic(cx + 2.0 / 3 * (x1 - cx), cy + 2.0 / 3 * (y1 - cy), x2 + 2.0 / 3 * (x1 - x2), y2 + 2.0 / 3 * (y1 - y2), x2, y2);
    };
    auto point = [&](double x, double y) {
        double tx, ty;
        m.apply(x, y, tx, ty);
        box.add(tx, ty);
    };
    for (const Segment& s : p.segs) {
        const char up = (char)std::toupper((unsigned char)s.cmd);
        switch (up) {
            case 'M':
                cx = sx = s.p[0];
                cy = sy = s.p[1];
                point(cx, cy);
                break;
            case 'Z':
                cx = sx;
                cy = sy;
                break;
            case 'L':
                point(s.p[0], s.p[1]);
                cx = s.p[0];
                cy = s.p[1];
                break;
            case 'H':
                point(s.p[0], cy);
                cx = s.p[0];
                break;
            case 'V':
                point(cx, s.p[0]);
                cy = s.p[0];
                break;
            case 'C':
                cubic(s.p[0], s.p[1], s.p[2], s.p[3], s.p[4], s.p[5]);
                lcx = s.p[2];
                lcy = s.p[3];
                cx = s.p[4];
                cy = s.p[5];
                break;
            case 'S': {
                double x1 = cx, y1 = cy;
                if (last == 'C' || last == 'S') {
                    x1 = 2 * cx - lcx;
                    y1 = 2 * cy - lcy;
                }
                cubic(x1, y1, s.p[0], s.p[1], s.p[2], s.p[3]);
                lcx = s.p[0];
                lcy = s.p[1];
                cx = s.p[2];
                cy = s.p[3];
                break;
            }
            case 'Q':
                quad(s.p[0], s.p[1], s.p[2], s.p[3]);
                lcx = s.p[0];
                lcy = s.p[1];
                cx = s.p[2];
                cy = s.p[3];
                break;
            case 'T': {
                double x1 = cx, y1 = cy;
                if (last == 'Q' || last == 'T') {
                    x1 = 2 * cx - lcx;
                    y1 = 2 * cy - lcy;
                }
                quad(x1, y1, s.p[0], s.p[1]);
                lcx = x1;
                lcy = y1;
                cx = s.p[0];
                cy = s.p[1];
                break;
            }
            case 'A':
                arcs.clear();
                arcToCubics(cx, cy, s.p[0], s.p[1], s.p[2], s.p[3] != 0, s.p[4] != 0, s.p[5], s.p[6], arcs);
                for (const auto& c : arcs) {
                    cubic(c[0], c[1], c[2], c[3], c[4], c[5]);
                    cx = c[4];
                    cy = c[5];
                }
                cx = s.p[5];
                cy = s.p[6];
                break;
        }
        last = up;
    }
}

}  // namespace pathdata
