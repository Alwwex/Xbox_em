#pragma once

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xbem::xml {

class XmlError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Small, tolerant DOM parser. Enough for MicrosoftGame.config / appxmanifest style
// documents: elements, attributes, text, comments, CDATA, processing instructions,
// DOCTYPE (skipped) and the predefined + numeric character entities.
struct Node {
  std::string name;
  std::vector<std::pair<std::string, std::string>> attributes;
  std::vector<Node> children;
  std::string text;  // concatenated character data directly inside this element

  // Attribute lookup; exact match first, then ASCII case-insensitive.
  const std::string* attr(std::string_view key) const;
  std::string attr_or(std::string_view key, std::string_view fallback = {}) const;

  // First direct child with the given name (case-insensitive), or nullptr.
  const Node* child(std::string_view child_name) const;
  std::vector<const Node*> children_named(std::string_view child_name) const;
  std::string child_text(std::string_view child_name) const;
};

Node parse(std::string_view document);

}  // namespace xbem::xml
