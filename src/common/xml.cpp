#include "common/xml.h"

#include <cstdint>
#include <format>

#include "common/strings.h"

namespace xbem::xml {

const std::string* Node::attr(std::string_view key) const {
  for (const auto& [k, v] : attributes)
    if (k == key) return &v;
  for (const auto& [k, v] : attributes)
    if (iequals(k, key)) return &v;
  return nullptr;
}

std::string Node::attr_or(std::string_view key, std::string_view fallback) const {
  const std::string* v = attr(key);
  return v ? *v : std::string(fallback);
}

const Node* Node::child(std::string_view child_name) const {
  for (const Node& c : children)
    if (iequals(c.name, child_name)) return &c;
  return nullptr;
}

std::vector<const Node*> Node::children_named(std::string_view child_name) const {
  std::vector<const Node*> out;
  for (const Node& c : children)
    if (iequals(c.name, child_name)) out.push_back(&c);
  return out;
}

std::string Node::child_text(std::string_view child_name) const {
  const Node* c = child(child_name);
  return c ? c->text : std::string();
}

namespace {

class Parser {
 public:
  explicit Parser(std::string_view doc) : s_(doc) {
    if (s_.size() >= 3 && static_cast<uint8_t>(s_[0]) == 0xEF && static_cast<uint8_t>(s_[1]) == 0xBB &&
        static_cast<uint8_t>(s_[2]) == 0xBF)
      pos_ = 3;
  }

  Node parse_document() {
    skip_misc();
    if (!peek('<')) fail("expected root element");
    Node root = parse_element(0);
    skip_misc();
    if (pos_ != s_.size()) fail("unexpected content after root element");
    return root;
  }

 private:
  std::string_view s_;
  size_t pos_ = 0;

  [[noreturn]] void fail(std::string_view what) const {
    size_t line = 1;
    for (size_t i = 0; i < pos_ && i < s_.size(); ++i)
      if (s_[i] == '\n') ++line;
    throw XmlError(std::format("XML parse error at line {}: {}", line, what));
  }

  bool peek(char c) const { return pos_ < s_.size() && s_[pos_] == c; }
  bool starts(std::string_view t) const { return s_.substr(pos_, t.size()) == t; }

  static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
  static bool is_name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == ':' ||
           c == '-' || c == '.' || static_cast<uint8_t>(c) >= 0x80;
  }

  void skip_space() {
    while (pos_ < s_.size() && is_space(s_[pos_])) ++pos_;
  }

  void skip_until(std::string_view terminator) {
    const size_t end = s_.find(terminator, pos_);
    if (end == std::string_view::npos) fail(std::format("missing '{}'", terminator));
    pos_ = end + terminator.size();
  }

  // Skips whitespace, comments, processing instructions and DOCTYPE.
  void skip_misc() {
    for (;;) {
      skip_space();
      if (starts("<?")) {
        skip_until("?>");
      } else if (starts("<!--")) {
        skip_until("-->");
      } else if (starts("<!DOCTYPE") || starts("<!doctype")) {
        // Internal subsets are not supported, but bracketed content is skipped.
        int depth = 0;
        while (pos_ < s_.size()) {
          const char c = s_[pos_++];
          if (c == '[') ++depth;
          else if (c == ']') --depth;
          else if (c == '>' && depth <= 0) break;
        }
      } else {
        return;
      }
    }
  }

  std::string parse_name() {
    const size_t start = pos_;
    while (pos_ < s_.size() && is_name_char(s_[pos_])) ++pos_;
    if (start == pos_) fail("expected a name");
    return std::string(s_.substr(start, pos_ - start));
  }

  static void append_utf8(std::string& out, uint32_t cp) {
    std::u16string tmp;
    if (cp >= 0x10000) {
      cp -= 0x10000;
      tmp.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
      tmp.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
    } else {
      tmp.push_back(static_cast<char16_t>(cp));
    }
    out += utf16_to_utf8(tmp);
  }

  std::string decode(std::string_view raw) const {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
      if (raw[i] != '&') {
        out.push_back(raw[i]);
        continue;
      }
      const size_t semi = raw.find(';', i);
      if (semi == std::string_view::npos) {
        out.push_back('&');
        continue;
      }
      const std::string_view ent = raw.substr(i + 1, semi - i - 1);
      if (ent == "lt") out.push_back('<');
      else if (ent == "gt") out.push_back('>');
      else if (ent == "amp") out.push_back('&');
      else if (ent == "quot") out.push_back('"');
      else if (ent == "apos") out.push_back('\'');
      else if (!ent.empty() && ent[0] == '#') {
        uint32_t cp = 0;
        const bool hex = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X');
        for (size_t k = hex ? 2 : 1; k < ent.size(); ++k) {
          const char c = ent[k];
          int d = -1;
          if (c >= '0' && c <= '9') d = c - '0';
          else if (hex && c >= 'a' && c <= 'f') d = c - 'a' + 10;
          else if (hex && c >= 'A' && c <= 'F') d = c - 'A' + 10;
          if (d < 0) { cp = 0xFFFD; break; }
          cp = cp * (hex ? 16 : 10) + static_cast<uint32_t>(d);
          if (cp > 0x10FFFF) { cp = 0xFFFD; break; }
        }
        append_utf8(out, cp);
      } else {
        // Unknown entity: keep verbatim.
        out.append(raw.substr(i, semi - i + 1));
      }
      i = semi;
    }
    return out;
  }

  Node parse_element(int depth) {
    if (depth > 256) fail("document nested too deeply");
    if (!peek('<')) fail("expected '<'");
    ++pos_;
    Node node;
    node.name = parse_name();

    // Attributes.
    for (;;) {
      skip_space();
      if (starts("/>")) {
        pos_ += 2;
        return node;
      }
      if (peek('>')) {
        ++pos_;
        break;
      }
      std::string key = parse_name();
      skip_space();
      if (!peek('=')) fail("expected '=' after attribute name");
      ++pos_;
      skip_space();
      if (!(peek('"') || peek('\''))) fail("expected quoted attribute value");
      const char q = s_[pos_++];
      const size_t end = s_.find(q, pos_);
      if (end == std::string_view::npos) fail("unterminated attribute value");
      node.attributes.emplace_back(std::move(key), decode(s_.substr(pos_, end - pos_)));
      pos_ = end + 1;
    }

    // Content.
    for (;;) {
      if (pos_ >= s_.size()) fail("unexpected end of document inside <" + node.name + ">");
      if (starts("</")) {
        pos_ += 2;
        const std::string closing = parse_name();
        if (closing != node.name) fail("mismatched closing tag </" + closing + "> for <" + node.name + ">");
        skip_space();
        if (!peek('>')) fail("expected '>'");
        ++pos_;
        break;
      }
      if (starts("<!--")) {
        skip_until("-->");
      } else if (starts("<![CDATA[")) {
        pos_ += 9;
        const size_t end = s_.find("]]>", pos_);
        if (end == std::string_view::npos) fail("unterminated CDATA");
        node.text.append(s_.substr(pos_, end - pos_));
        pos_ = end + 3;
      } else if (starts("<?")) {
        skip_until("?>");
      } else if (peek('<')) {
        node.children.push_back(parse_element(depth + 1));
      } else {
        const size_t end = s_.find('<', pos_);
        const size_t stop = end == std::string_view::npos ? s_.size() : end;
        node.text += decode(s_.substr(pos_, stop - pos_));
        pos_ = stop;
      }
    }

    // Trim insignificant whitespace around text content.
    const size_t first = node.text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
      node.text.clear();
    } else {
      const size_t last = node.text.find_last_not_of(" \t\r\n");
      node.text = node.text.substr(first, last - first + 1);
    }
    return node;
  }
};

}  // namespace

Node parse(std::string_view document) { return Parser(document).parse_document(); }

}  // namespace xbem::xml
