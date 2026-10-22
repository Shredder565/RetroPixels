// Original code for RetroPixels (Supermodel). Not derived from Supermodel.

#include "Util/ConfigBuilders.h"
#include "Util/NewConfig.h"
#include "OSD/Logger.h"
#include "Pkgs/tinyxml2.h"
#include <cstdio>
#include <fstream>
#include <map>
#include <set>

namespace Util
{
  namespace Config
  {
    //
    // XML
    //

    static void AddElement(Node *parent, const tinyxml2::XMLElement *element)
    {
      const char *text = element->GetText();
      Node &node = parent->Add(element->Name(), std::string(text ? text : ""));
      for (const tinyxml2::XMLAttribute *a = element->FirstAttribute(); a; a = a->Next())
        node.Add(a->Name(), a->Value());
      for (const tinyxml2::XMLElement *child = element->FirstChildElement(); child; child = child->NextSiblingElement())
        AddElement(&node, child);
    }

    static void AddDocument(Node *config, const tinyxml2::XMLDocument &doc)
    {
      for (const tinyxml2::XMLElement *e = doc.RootElement(); e; e = e->NextSiblingElement())
        AddElement(config, e);
    }

    bool FromXML(Node *config, const std::string &text)
    {
      *config = Node("xml");
      tinyxml2::XMLDocument doc;
      if (doc.Parse(text.c_str()) != tinyxml2::XML_SUCCESS)
      {
        ErrorLog("Failed to parse XML (%s).", doc.ErrorName());
        return true;
      }
      AddDocument(config, doc);
      return false;
    }

    bool FromXMLFile(Node *config, const std::string &filename)
    {
      *config = Node("xml");
      tinyxml2::XMLDocument doc;
      if (doc.LoadFile(filename.c_str()) != tinyxml2::XML_SUCCESS)
      {
        ErrorLog("Failed to parse %s (%s).", filename.c_str(), doc.ErrorName());
        return true;
      }
      AddDocument(config, doc);
      return false;
    }

    //
    // INI
    //

    // The line up to the first ';' that is not between double quotes.
    static std::string WithoutComment(const std::string &line)
    {
      bool quoted = false;
      for (size_t i = 0; i < line.size(); i++)
      {
        if (line[i] == '"')
          quoted = !quoted;
        else if (line[i] == ';' && !quoted)
          return line.substr(0, i);
      }
      return line;
    }

    struct Setting
    {
      std::string name;
      std::string value;
    };

    // Splits "name = value". Returns false (with a message) if the line is
    // not a setting at all; 'warning' is set for stray quotes, which are
    // reported but the setting is still taken.
    static bool ReadSetting(const std::string &line, const std::string &filename, size_t line_num, bool quiet, Setting &out, bool &warning)
    {
      warning = false;
      size_t equals = line.find('=');
      if (equals == std::string::npos)
      {
        if (!quiet)
          ErrorLog("%s:%d: Syntax error. No '=' found.", filename.c_str(), int(line_num));
        return false;
      }
      out.name = TrimWhiteSpace(line.substr(0, equals));
      if (out.name.empty())
      {
        if (!quiet)
          ErrorLog("%s:%d: Syntax error. Setting name missing before '='.", filename.c_str(), int(line_num));
        return false;
      }
      std::string value = TrimWhiteSpace(line.substr(equals + 1));
      if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
        value = value.substr(1, value.size() - 2);
      if (value.find('"') != std::string::npos)
      {
        if (!quiet)
          ErrorLog("%s:%d: Warning: Extraneous quotes present on line.", filename.c_str(), int(line_num));
        warning = true;
      }
      out.value = value;
      return true;
    }

    bool FromINIFile(Node *config, const std::string &filename)
    {
      std::ifstream file(filename);
      if (file.fail())
      {
        ErrorLog("Unable to open '%s'. Configuration will not be loaded.", filename.c_str());
        return true;
      }

      *config = Node("Global");
      Node &global = *config;
      // The sections the current settings go to. A set (by address) so a
      // section named twice in one header gets each setting once.
      std::set<Node *> targets{ &global };

      size_t line_num = 1;
      for (;; line_num++)
      {
        if (file.eof())
          break;
        if (file.fail())
        {
          ErrorLog("%s:%d: File read error. Configuration will be incomplete.", filename.c_str(), int(line_num));
          return true;
        }
        std::string line;
        std::getline(file, line);
        line = TrimWhiteSpace(WithoutComment(line));
        if (line.empty())
          continue;

        if (line.front() == '[' && line.back() == ']')
        {
          targets.clear();
          for (const std::string &piece : Util::Format(line.substr(1, line.size() - 2)).Split(','))
          {
            std::string name = TrimWhiteSpace(piece);
            Node *section;
            if (name.empty() || name == "Global")
              section = &global;
            else if (Node *found = global.TryGet(name))
              section = found;
            else
              section = &global.Add(name);
            targets.insert(section);
          }
          continue;
        }

        // A setting: the same for every current section. Once a problem has
        // been reported for this line it is not reported again.
        bool quiet = false;
        for (Node *section : targets)
        {
          Setting setting;
          bool warning;
          if (ReadSetting(line, filename, line_num, quiet, setting, warning))
          {
            section->Set(setting.name, setting.value);
            quiet |= warning;
          }
          else
            quiet = true;
        }
      }
      return false;
    }

    void MergeINISections(Node *merged, const Node &x, const Node &y)
    {
      *merged = Node(x.Key());

      // x's value range for each key (the last value-holding node with that
      // key decides, even if it has no range).
      std::map<std::string, std::shared_ptr<ValueRange>> ranges;

      for (const Node &setting : x)
      {
        if (!setting.IsLeaf())
          continue;
        std::shared_ptr<GenericValue> value = setting.GetValue();
        merged->Set(setting.Key(), value);
        if (value)
          ranges[setting.Key()] = value->GetValueRange();
      }

      for (const Node &setting : y)
      {
        if (setting.IsLeaf() && setting.Exists())
          merged->Set(setting.Key(), setting.GetValue());
      }

      for (Node &setting : *merged)
      {
        auto range = ranges.find(setting.Key());
        if (range != ranges.end() && range->second && setting.GetValue())
          setting.GetValue()->SetValueRange(range->second);
      }
    }

    static void WriteSection(std::ofstream &file, const Node &section)
    {
      file << "[ " << section.Key() << " ]" << std::endl << std::endl;
      for (const Node &setting : section)
      {
        if (setting.IsLeaf() && setting.Exists())
          file << setting.Key() << " = " << setting.ValueAs<std::string>() << std::endl;
      }
      file << std::endl << std::endl;
    }

    void WriteINIFile(const std::string &filename, const Node &config, const std::string &header_comment)
    {
      std::ofstream file(filename);
      if (file.fail())
      {
        ErrorLog("Unable to write to '%s'. Configuration will not be saved.", filename.c_str());
        return;
      }
      if (!header_comment.empty())
        file << header_comment << std::endl << std::endl;
      WriteSection(file, config);
      for (const Node &section : config)
      {
        if (section.HasChildren())
          WriteSection(file, section);
      }
      printf("Configuration successfully saved to '%s'.\n", filename.c_str());
      InfoLog("Configuration successfully saved to '%s'.", filename.c_str());
    }
  }
}
