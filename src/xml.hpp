// A small, forgiving XML parser. Unlike a browser it never gives up: every
// syntax problem is recorded as an Issue and repaired in the resulting tree
// (truncated files are closed, stray markup dropped, bad entities escaped).
// Serializing the tree therefore always yields well-formed XML.
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace xml {

enum class NodeType { Element, Text, CData, Comment, PI, Doctype };

struct Attr {
    std::string name;
    std::string value;  // decoded (entities resolved)
};

struct Node {
    NodeType type = NodeType::Element;
    std::string name;   // element name or PI target
    std::string value;  // Text: decoded text; others: raw content
    std::vector<Attr> attrs;
    std::vector<std::unique_ptr<Node>> children;
    Node* parent = nullptr;

    bool isElement() const { return type == NodeType::Element; }
    std::string_view localName() const;
    std::string_view prefix() const;
    const std::string* attr(std::string_view n) const;
    void setAttr(std::string_view n, std::string v);  // appends when missing
    bool removeAttr(std::string_view n);
    Node* append(std::unique_ptr<Node> child);
};

struct Document {
    std::vector<std::unique_ptr<Node>> nodes;  // top level: comments, doctype, PIs, root
    Node* root = nullptr;
};

struct Issue {
    size_t offset = 0;
    std::string message;
};

struct ParseResult {
    Document doc;
    std::vector<Issue> issues;
    bool truncated = false;  // the input ended in the middle of the document
    bool hadDeclaration = false;
};

ParseResult parse(std::string_view text);

std::string serialize(const Document& doc);
void serializeNode(const Node& n, std::string& out);
std::string escapeAttr(std::string_view s);
std::string escapeText(std::string_view s);

// 1-based line and column of a byte offset.
void lineCol(std::string_view text, size_t offset, int& line, int& col);

}  // namespace xml
