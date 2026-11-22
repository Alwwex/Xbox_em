#include "common/xml.h"
#include "test.h"

using namespace xbem;

TEST(xml_basic_document) {
  const auto root = xml::parse(
      "\xEF\xBB\xBF<?xml version=\"1.0\"?>\n<!-- comment -->\n"
      "<Root a=\"1\" B='two &amp; three'>\n"
      "  <Child name=\"x\">text &lt;here&gt; &#x41;&#66;</Child>\n"
      "  <Empty/>\n"
      "  <Child name=\"y\"><![CDATA[<raw>]]></Child>\n"
      "</Root>");
  CHECK_EQ(root.name, std::string("Root"));
  CHECK_EQ(root.attr_or("a"), std::string("1"));
  CHECK_EQ(root.attr_or("b"), std::string("two & three"));  // case-insensitive fallback
  CHECK(root.attr("missing") == nullptr);
  CHECK_EQ(root.children.size(), 3u);
  CHECK_EQ(root.children_named("child").size(), 2u);
  CHECK_EQ(root.child("Child")->text, std::string("text <here> AB"));
  CHECK_EQ(root.children[2].text, std::string("<raw>"));
  CHECK(root.child("Empty") != nullptr);
}

TEST(xml_errors) {
  CHECK_THROWS(xml::parse("<a><b></a>"));
  CHECK_THROWS(xml::parse("<a attr=noquotes/>"));
  CHECK_THROWS(xml::parse("<a>"));
  CHECK_THROWS(xml::parse("<a/><b/>"));
  CHECK_THROWS(xml::parse(""));
}

TEST(xml_doctype_and_pi_are_skipped) {
  const auto root = xml::parse("<!DOCTYPE x [ <!ENTITY e 'v'> ]><x><?pi data?>t</x>");
  CHECK_EQ(root.name, std::string("x"));
  CHECK_EQ(root.text, std::string("t"));
}
