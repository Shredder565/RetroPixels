// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Building configuration trees (Util/NewConfig.h) from XML and INI files and
// writing INI files back. The FromXXX functions return true on failure.

#ifndef INCLUDED_UTIL_CONFIGBUILDERS_H
#define INCLUDED_UTIL_CONFIGBUILDERS_H

#include <string>

namespace Util
{
  namespace Config
  {
    class Node;

    // The tree has key "xml"; every element becomes a node holding its text,
    // with one child per attribute followed by one per child element.
    bool FromXML(Node *config, const std::string &text);
    bool FromXMLFile(Node *config, const std::string &filename);

    // The root is the "Global" section; each [Section] is a child of it.
    // "[A, B]" applies the following settings to both sections, and an empty
    // name stands for Global. ';' starts a comment outside double quotes, and
    // a value fully enclosed in double quotes has them removed.
    bool FromINIFile(Node *config, const std::string &filename);

    // 'merged' becomes x's settings (leaf nodes) with y's overriding them
    // (y's value-less nodes are skipped), keeping x's key. The values are
    // shared with x and y, not copied; x's GUI value ranges are kept.
    void MergeINISections(Node *merged, const Node &x, const Node &y);

    // Writes the header comment (if any), the root's settings as [ Global ]
    // and then each section.
    void WriteINIFile(const std::string &filename, const Node &config, const std::string &header_comment);
  }
}

#endif  // INCLUDED_UTIL_CONFIGBUILDERS_H
