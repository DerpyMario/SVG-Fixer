#include "xml.hpp"

#include <cctype>
#include <cstdint>

#include "util.hpp"

namespace xml {

std::string_view Node::localName() const {
    size_t c = name.find(':');
    return c == std::string::npos ? std::string_view(name) : std::string_view(name).substr(c + 1);
}

std::string_view Node::prefix() const {
    size_t c = name.find(':');
    return c == std::string::npos ? std::string_view() : std::string_view(name).substr(0, c);
}

const std::string* Node::attr(std::string_view n) const {
    for (const Attr& a : attrs)
        if (a.name == n) return &a.value;
    return nullptr;
}

void Node::setAttr(std::string_view n, std::string v) {
    for (Attr& a : attrs) {
        if (a.name == n) {
            a.value = std::move(v);
            return;
        }
    }
    attrs.push_back({std::string(n), std::move(v)});
}

bool Node::removeAttr(std::string_view n) {
    for (auto it = attrs.begin(); it != attrs.end(); ++it) {
        if (it->name == n) {
            attrs.erase(it);
            return true;
        }
    }
    return false;
}

Node* Node::append(std::unique_ptr<Node> child) {
    child->parent = this;
    children.push_back(std::move(child));
    return children.back().get();
}

namespace {

constexpr size_t npos = std::string_view::npos;

bool isWs(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool isNameStart(unsigned char c) { return std::isalpha(c) || c == '_' || c == ':' || c >= 0x80; }
bool isNameChar(unsigned char c) { return isNameStart(c) || std::isdigit(c) || c == '-' || c == '.'; }

struct HtmlEntity {
    const char* name;
    uint32_t cp;
};

// HTML named entities that show up in exported pages; XML only knows the five
// predefined ones, so these make browsers reject an SVG outright.
const HtmlEntity kHtmlEntities[] = {
    {"nbsp", 0xA0},    {"copy", 0xA9},    {"reg", 0xAE},     {"trade", 0x2122}, {"hellip", 0x2026},
    {"mdash", 0x2014}, {"ndash", 0x2013}, {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C},
    {"rdquo", 0x201D}, {"bull", 0x2022},  {"middot", 0xB7},  {"deg", 0xB0},     {"times", 0xD7},
    {"euro", 0x20AC},  {"laquo", 0xAB},   {"raquo", 0xBB},   {"shy", 0xAD},     {"para", 0xB6},
    {"sect", 0xA7},    {"plusmn", 0xB1},  {"micro", 0xB5},   {"frac12", 0xBD},  {"frac14", 0xBC},
    {"frac34", 0xBE},  {"pound", 0xA3},   {"yen", 0xA5},     {"cent", 0xA2},    {"ensp", 0x2002},
    {"emsp", 0x2003},  {"thinsp", 0x2009}, {"iexcl", 0xA1},  {"iquest", 0xBF},
};

// Shapes never contain other shapes; an unclosed <path ...> followed by
// another <path ...> must be closed instead of nesting thousands deep.
bool isLeafShape(std::string_view n) {
    return n == "path" || n == "rect" || n == "circle" || n == "ellipse" || n == "line" || n == "polyline" ||
           n == "polygon" || n == "image" || n == "use" || n == "stop";
}

bool allowedInsideShape(std::string_view n) {
    return n == "title" || n == "desc" || n == "metadata" || n == "animate" || n == "animateTransform" ||
           n == "animateMotion" || n == "animateColor" || n == "set";
}

std::string_view localOf(std::string_view name) {
    size_t c = name.find(':');
    return c == npos ? name : name.substr(c + 1);
}

std::string preview(std::string_view s) {
    std::string t(util::trim(s));
    if (t.size() > 40) t = t.substr(0, 40) + "...";
    for (char& c : t)
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    return t;
}

class Parser {
public:
    Parser(std::string_view s, ParseResult& r) : s_(s), r_(r) {}

    void run() {
        const size_t n = s_.size();
        while (i_ < n) {
            if (s_[i_] != '<') {
                parseText();
            } else if (at("<?")) {
                parsePI();
            } else if (at("<!--")) {
                parseComment();
            } else if (at("<![CDATA[")) {
                parseCData();
            } else if (util::istartsWith(s_.substr(i_), "<!DOCTYPE")) {
                parseDoctype();
            } else if (at("<!")) {
                size_t gt = s_.find('>', i_);
                issue(i_, "an unknown <!...> declaration was removed");
                i_ = gt == npos ? n : gt + 1;
            } else if (at("</")) {
                parseEndTag();
            } else if (i_ + 1 < n && isNameStart((unsigned char)s_[i_ + 1])) {
                parseStartTag();
            } else {
                issue(i_, "a '<' that does not start a tag was escaped as &lt;");
                addText("<", i_);
                ++i_;
            }
        }
        if (!stack_.empty()) {
            if (stack_.front() == r_.doc.root) {
                r_.truncated = true;
                std::string msg = "the file ends before the closing </" + r_.doc.root->name +
                                  "> tag (it was cut off); the document was closed";
                if (stack_.size() > 1) msg += " (" + std::to_string(stack_.size()) + " open elements)";
                issue(n, msg);
            }
            stack_.clear();
        }
    }

private:
    std::string_view s_;
    size_t i_ = 0;
    ParseResult& r_;
    std::vector<Node*> stack_;
    std::vector<std::pair<std::string, std::string>> entities_;
    std::vector<std::unique_ptr<Node>> discarded_;
    bool rootClosed_ = false;
    bool reportedExtra_ = false;
    bool seenMarkup_ = false;

    void issue(size_t off, std::string msg) { r_.issues.push_back({off, std::move(msg)}); }
    bool at(std::string_view lit) const { return s_.substr(i_, lit.size()) == lit; }

    void skipWs() {
        while (i_ < s_.size() && isWs(s_[i_])) ++i_;
    }

    std::string readName() {
        size_t start = i_;
        while (i_ < s_.size() && isNameChar((unsigned char)s_[i_])) ++i_;
        return std::string(s_.substr(start, i_ - start));
    }

    // Returns 0 when the reference is unknown (the '&' is kept literally),
    // 1 when decoded, 2 when an invalid character reference was dropped.
    int decodeEntity(std::string_view name, std::string& out, size_t off) {
        if (name.empty()) return 0;
        if (name[0] == '#') {
            bool hex = name.size() > 1 && (name[1] == 'x' || name[1] == 'X');
            std::string_view digits = name.substr(hex ? 2 : 1);
            if (digits.empty() || digits.size() > 8) return 0;
            uint32_t cp = 0;
            for (char c : digits) {
                int v;
                if (c >= '0' && c <= '9')
                    v = c - '0';
                else if (hex && c >= 'a' && c <= 'f')
                    v = c - 'a' + 10;
                else if (hex && c >= 'A' && c <= 'F')
                    v = c - 'A' + 10;
                else
                    return 0;
                cp = cp * (hex ? 16 : 10) + (uint32_t)v;
            }
            bool valid = cp == 0x9 || cp == 0xA || cp == 0xD || (cp >= 0x20 && cp <= 0xD7FF) ||
                         (cp >= 0xE000 && cp <= 0xFFFD) || (cp >= 0x10000 && cp <= 0x10FFFF);
            if (!valid) {
                issue(off, "an invalid character reference &" + std::string(name) + "; was removed");
                return 2;
            }
            util::appendUtf8(out, cp);
            return 1;
        }
        for (char c : name)
            if (!isNameChar((unsigned char)c)) return 0;
        if (name == "lt") { out += '<'; return 1; }
        if (name == "gt") { out += '>'; return 1; }
        if (name == "amp") { out += '&'; return 1; }
        if (name == "quot") { out += '"'; return 1; }
        if (name == "apos") { out += '\''; return 1; }
        for (const auto& e : entities_) {
            if (e.first == name) {
                out += e.second;
                return 1;
            }
        }
        for (const HtmlEntity& h : kHtmlEntities) {
            if (name == h.name) {
                util::appendUtf8(out, h.cp);
                issue(off, "the HTML entity &" + std::string(name) + "; is not valid XML; replaced it with the character");
                return 1;
            }
        }
        return 0;
    }

    std::string decode(std::string_view raw, size_t off, bool attr) {
        std::string out;
        out.reserve(raw.size());
        for (size_t k = 0; k < raw.size(); ++k) {
            char c = raw[k];
            if (c == '&') {
                size_t semi = raw.find(';', k + 1);
                if (semi != npos && semi - k <= 40) {
                    if (decodeEntity(raw.substr(k + 1, semi - k - 1), out, off + k)) {
                        k = semi;
                        continue;
                    }
                }
                issue(off + k, "an unescaped '&' was escaped as &amp;");
                out += '&';
            } else if (c == '\r') {  // XML end-of-line handling
                if (k + 1 < raw.size() && raw[k + 1] == '\n') ++k;
                out += attr ? ' ' : '\n';
            } else if (attr && (c == '\n' || c == '\t')) {  // attribute-value normalization
                out += ' ';
            } else {
                out += c;
            }
        }
        return out;
    }

    void addText(std::string_view raw, size_t off) {
        if (stack_.empty()) {
            if (!util::trim(raw).empty())
                issue(off, "stray text outside the root element was removed: \"" + preview(raw) + "\"");
            return;
        }
        std::string decoded = decode(raw, off, false);
        Node* parent = stack_.back();
        if (!parent->children.empty() && parent->children.back()->type == NodeType::Text) {
            parent->children.back()->value += decoded;
            return;
        }
        auto t = std::make_unique<Node>();
        t->type = NodeType::Text;
        t->value = std::move(decoded);
        parent->append(std::move(t));
    }

    void addMisc(NodeType type, std::string name, std::string value, size_t off) {
        seenMarkup_ = true;
        auto node = std::make_unique<Node>();
        node->type = type;
        node->name = std::move(name);
        node->value = std::move(value);
        if (!stack_.empty())
            stack_.back()->append(std::move(node));
        else if (type == NodeType::CData)
            issue(off, "a CDATA section outside the root element was removed");
        else
            r_.doc.nodes.push_back(std::move(node));
    }

    void addElement(std::unique_ptr<Node> el, bool open, size_t off) {
        seenMarkup_ = true;
        Node* raw = el.get();
        if (!stack_.empty()) {
            stack_.back()->append(std::move(el));
        } else if (!r_.doc.root && !rootClosed_) {
            r_.doc.root = raw;
            r_.doc.nodes.push_back(std::move(el));
            if (!open) rootClosed_ = true;
        } else {
            if (!reportedExtra_) {
                std::string rootName = r_.doc.root ? r_.doc.root->name : std::string("root");
                issue(off, "content after the closing </" + rootName + "> tag was removed");
                reportedExtra_ = true;
            }
            discarded_.push_back(std::move(el));
        }
        if (open) stack_.push_back(raw);
    }

    void parseText() {
        size_t start = i_;
        size_t end = s_.find('<', i_);
        if (end == npos) end = s_.size();
        i_ = end;
        addText(s_.substr(start, end - start), start);
    }

    void parseStartTag() {
        const size_t start = i_;
        ++i_;
        auto el = std::make_unique<Node>();
        el->type = NodeType::Element;
        el->name = readName();
        bool selfClosing = false;
        for (;;) {
            skipWs();
            if (i_ >= s_.size()) {
                r_.truncated = true;
                issue(start, "the file ends inside a <" + el->name +
                                 "> tag (it was cut off); the incomplete element was removed");
                return;
            }
            char c = s_[i_];
            if (c == '>') {
                ++i_;
                break;
            }
            if (c == '/' && i_ + 1 < s_.size() && s_[i_ + 1] == '>') {
                i_ += 2;
                selfClosing = true;
                break;
            }
            if (c == '<') {
                issue(i_, "a <" + el->name + "> tag was not closed with '>'");
                selfClosing = true;
                break;
            }
            if (!isNameStart((unsigned char)c)) {
                issue(i_, std::string("an unexpected '") + c + "' inside a <" + el->name + "> tag was removed");
                ++i_;
                continue;
            }
            const size_t aoff = i_;
            std::string aname = readName();
            skipWs();
            std::string value;
            if (i_ < s_.size() && s_[i_] == '=') {
                ++i_;
                skipWs();
                if (i_ < s_.size() && (s_[i_] == '"' || s_[i_] == '\'')) {
                    char q = s_[i_++];
                    size_t end = s_.find(q, i_);
                    if (end == npos) {
                        r_.truncated = true;
                        issue(aoff, "the file ends inside the " + aname + " attribute of a <" + el->name +
                                        "> tag (it was cut off); the incomplete element was removed");
                        i_ = s_.size();
                        return;
                    }
                    std::string_view raw = s_.substr(i_, end - i_);
                    if (raw.find('<') != npos) issue(i_, "a '<' inside an attribute value was escaped as &lt;");
                    value = decode(raw, i_, true);
                    i_ = end + 1;
                } else {
                    size_t vstart = i_;
                    while (i_ < s_.size() && !isWs(s_[i_]) && s_[i_] != '>' &&
                           !(s_[i_] == '/' && i_ + 1 < s_.size() && s_[i_ + 1] == '>'))
                        ++i_;
                    issue(aoff, "an attribute value without quotes was quoted");
                    value = decode(s_.substr(vstart, i_ - vstart), vstart, true);
                }
            } else {
                issue(aoff, "an attribute without a value (" + aname + ") was given an empty value");
            }
            bool duplicate = false;
            for (Attr& a : el->attrs) {
                if (a.name == aname) {
                    a.value = value;
                    duplicate = true;
                    break;
                }
            }
            if (duplicate)
                issue(aoff, "duplicate " + aname + " attribute on <" + el->name + "> (kept the last value)");
            else
                el->attrs.push_back({std::move(aname), std::move(value)});
        }
        if (!stack_.empty() && isLeafShape(stack_.back()->localName()) && !allowedInsideShape(localOf(el->name))) {
            issue(start, "<" + stack_.back()->name + "> was not closed before the next <" + el->name + ">; closed it");
            stack_.pop_back();
        }
        addElement(std::move(el), !selfClosing, start);
    }

    void parseEndTag() {
        const size_t start = i_;
        i_ += 2;
        std::string name = readName();
        skipWs();
        if (i_ < s_.size() && s_[i_] == '>') {
            ++i_;
        } else if (i_ >= s_.size()) {
            issue(start, "the closing </" + name + "> tag at the end of the file is missing its '>' (cut off)");
        } else {
            issue(start, "a malformed closing tag </" + name + "> was repaired");
            size_t stop = s_.find_first_of("<>", i_);
            i_ = stop == npos ? s_.size() : (s_[stop] == '>' ? stop + 1 : stop);
        }
        if (stack_.empty()) {
            issue(start, "a stray closing tag </" + name + "> was removed");
            return;
        }
        size_t k = stack_.size();
        while (k > 0 && stack_[k - 1]->name != name) --k;
        if (k == 0) {
            issue(start, "a closing tag </" + name + "> without a matching opening tag was removed");
            return;
        }
        for (size_t j = stack_.size(); j > k; --j)
            issue(start, "<" + stack_[j - 1]->name + "> was not closed; closed it before </" + name + ">");
        stack_.resize(k - 1);
        if (stack_.empty()) rootClosed_ = true;
    }

    void parseComment() {
        const size_t start = i_;
        size_t end = s_.find("-->", i_ + 4);
        if (end == npos) {
            r_.truncated = true;
            issue(start, "the file ends inside a comment (it was cut off)");
            i_ = s_.size();
            return;
        }
        std::string body(s_.substr(i_ + 4, end - i_ - 4));
        i_ = end + 3;
        if (body.find("--") != std::string::npos || (!body.empty() && body.back() == '-')) {
            issue(start, "'--' is not allowed inside a comment; it was replaced");
            size_t p;
            while ((p = body.find("--")) != std::string::npos) body.replace(p, 2, "- -");
            if (!body.empty() && body.back() == '-') body += ' ';
        }
        addMisc(NodeType::Comment, "", std::move(body), start);
    }

    void parseCData() {
        const size_t start = i_;
        size_t end = s_.find("]]>", i_ + 9);
        if (end == npos) {
            r_.truncated = true;
            issue(start, "the file ends inside a CDATA section (it was cut off)");
            i_ = s_.size();
            return;
        }
        std::string body(s_.substr(i_ + 9, end - i_ - 9));
        i_ = end + 3;
        addMisc(NodeType::CData, "", std::move(body), start);
    }

    void parsePI() {
        const size_t start = i_;
        size_t end = s_.find("?>", i_ + 2);
        if (end == npos) {
            r_.truncated = true;
            issue(start, "the file ends inside a <?...?> instruction (it was cut off)");
            i_ = s_.size();
            return;
        }
        std::string_view body = s_.substr(i_ + 2, end - i_ - 2);
        i_ = end + 2;
        size_t nl = 0;
        while (nl < body.size() && isNameChar((unsigned char)body[nl])) ++nl;
        std::string target(body.substr(0, nl));
        if (util::iequals(target, "xml")) {
            if (r_.hadDeclaration) {
                issue(start, "a second <?xml ...?> declaration was removed");
            } else {
                r_.hadDeclaration = true;
                if (start != 0)
                    issue(start, "the <?xml ...?> declaration was not at the very start of the file; moved it there");
            }
            return;
        }
        if (target.empty()) {
            issue(start, "an invalid <?...?> instruction was removed");
            return;
        }
        addMisc(NodeType::PI, std::move(target), std::string(util::trim(body.substr(nl))), start);
    }

    void parseEntityDecls(std::string_view dtd) {
        size_t p = 0;
        while ((p = dtd.find("<!ENTITY", p)) != npos) {
            p += 8;
            while (p < dtd.size() && isWs(dtd[p])) ++p;
            if (p < dtd.size() && dtd[p] == '%') continue;  // parameter entity
            size_t ns = p;
            while (p < dtd.size() && isNameChar((unsigned char)dtd[p])) ++p;
            std::string name(dtd.substr(ns, p - ns));
            while (p < dtd.size() && isWs(dtd[p])) ++p;
            if (name.empty() || p >= dtd.size() || (dtd[p] != '"' && dtd[p] != '\'')) continue;  // external
            char q = dtd[p++];
            size_t e = dtd.find(q, p);
            if (e == npos) break;
            entities_.emplace_back(name, decode(dtd.substr(p, e - p), 0, false));
            p = e + 1;
        }
    }

    void parseDoctype() {
        const size_t start = i_;
        size_t k = i_ + 9;
        int depth = 0;
        char quote = 0;
        for (; k < s_.size(); ++k) {
            char c = s_[k];
            if (quote) {
                if (c == quote) quote = 0;
            } else if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == '[') {
                ++depth;
            } else if (c == ']') {
                if (depth) --depth;
            } else if (c == '>' && depth == 0) {
                break;
            }
        }
        if (k >= s_.size()) {
            r_.truncated = true;
            issue(start, "the file ends inside the <!DOCTYPE> (it was cut off)");
            i_ = s_.size();
            return;
        }
        std::string_view raw = s_.substr(start, k + 1 - start);
        i_ = k + 1;
        size_t lb = raw.find('['), rb = raw.rfind(']');
        if (lb != npos && rb != npos && rb > lb) parseEntityDecls(raw.substr(lb + 1, rb - lb - 1));
        if (r_.doc.root) {
            issue(start, "a <!DOCTYPE> after the start of the document was removed");
            return;
        }
        addMisc(NodeType::Doctype, "", std::string(raw), start);
    }
};

}  // namespace

ParseResult parse(std::string_view text) {
    ParseResult r;
    Parser(text, r).run();
    return r;
}

std::string escapeAttr(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '"': out += "&quot;"; break;
            case '\t': out += "&#9;"; break;
            case '\n': out += "&#10;"; break;
            case '\r': out += "&#13;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string escapeText(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            default: out += c;
        }
    }
    return out;
}

void serializeNode(const Node& n, std::string& out) {
    switch (n.type) {
        case NodeType::Element:
            out += '<';
            out += n.name;
            for (const Attr& a : n.attrs) {
                out += ' ';
                out += a.name;
                out += "=\"";
                out += escapeAttr(a.value);
                out += '"';
            }
            if (n.children.empty()) {
                out += "/>";
            } else {
                out += '>';
                for (const auto& c : n.children) serializeNode(*c, out);
                out += "</";
                out += n.name;
                out += '>';
            }
            break;
        case NodeType::Text:
            out += escapeText(n.value);
            break;
        case NodeType::CData: {
            // "]]>" cannot appear inside CDATA; split the section around it.
            std::string body = n.value;
            size_t p = 0;
            while ((p = body.find("]]>", p)) != std::string::npos) {
                body.replace(p, 3, "]]]]><![CDATA[>");
                p += 15;
            }
            out += "<![CDATA[";
            out += body;
            out += "]]>";
            break;
        }
        case NodeType::Comment:
            out += "<!--";
            out += n.value;
            out += "-->";
            break;
        case NodeType::PI:
            out += "<?";
            out += n.name;
            if (!n.value.empty()) {
                out += ' ';
                out += n.value;
            }
            out += "?>";
            break;
        case NodeType::Doctype:
            out += n.value;
            break;
    }
}

std::string serialize(const Document& doc) {
    std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    for (const auto& n : doc.nodes) {
        serializeNode(*n, out);
        out += '\n';
    }
    return out;
}

void lineCol(std::string_view text, size_t offset, int& line, int& col) {
    line = 1;
    col = 1;
    if (offset > text.size()) offset = text.size();
    for (size_t k = 0; k < offset; ++k) {
        if (text[k] == '\n') {
            ++line;
            col = 1;
        } else if (((unsigned char)text[k] & 0xC0) != 0x80) {
            ++col;
        }
    }
}

}  // namespace xml
