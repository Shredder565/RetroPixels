// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Stand-alone checks of the configuration tree and its builders. Not part of
// the emulator build; compile it on its own with Util/NewConfig.cpp,
// Util/ConfigBuilders.cpp, Util/Format.cpp, Pkgs/tinyxml2.cpp and a logger.
// Prints one line per check and returns non-zero if any failed.

#include "Util/NewConfig.h"
#include "Util/ConfigBuilders.h"
#include <cstdio>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace
{
  int g_failures = 0;

  void Check(const char *what, bool ok)
  {
    std::cout << (ok ? "ok      " : "FAILED  ") << what << std::endl;
    if (!ok)
      g_failures++;
  }

  bool Throws(const std::function<void()> &f)
  {
    try
    {
      f();
    }
    catch (const std::exception &)
    {
      return true;
    }
    return false;
  }
}

int main()
{
  using Util::Config::Node;

  // A small tree built by hand, with a repeated key.
  {
    Node root("cfg");
    Node &unit = root.Add("unit");
    unit.Add<std::string>("label", "alpha");
    Node &parts = unit.Add("parts");
    Node &p1 = parts.Add("part");
    p1.Add<std::string>("id", "one");
    Node &p2 = parts.Add("part");
    p2.Add<std::string>("id", "two");
    p2.Add<int>("size", 7);

    const char *dump =
      "cfg=\"\"  children={ unit }\n"
      "  unit=\"\"  children={ label parts }\n"
      "    label=\"alpha\"  children={ }\n"
      "    parts=\"\"  children={ part }\n"
      "      part=\"\"  children={ id }\n"
      "        id=\"one\"  children={ }\n"
      "      part=\"\"  children={ id size }\n"
      "        id=\"two\"  children={ }\n"
      "        size=\"7\"  children={ }\n";
    Check("dump of a hand-built tree", root.ToString() == dump);
    Check("a repeated key finds the latest node", root["unit/parts/part/id"].Value<std::string>() == "two");
    int count = 0;
    for (const Node &n : root["unit/parts"])
      count += n.Key() == "part";
    Check("iteration sees both repeated nodes", count == 2);

    Node copy(root);
    Check("copy construction copies everything", copy.ToString() == root.ToString());
    Node other("x", "y");
    other.Add<std::string>("q/r", "s");
    other = root;
    other.Get("unit/label").SetValue("beta");
    Check("assignment makes an independent copy",
          other["unit/label"].Value<std::string>() == "beta" && root["unit/label"].Value<std::string>() == "alpha");
  }

  // Missing nodes, defaults and conversions.
  {
    Node root("cfg");
    root.Add<std::string>("hex", "0x1F");
    root.Add<std::string>("neg", "-0x10");
    root.Add<std::string>("flag", "On");
    root.Add<std::string>("num", "42");
    root.Add<float>("f", 2.5f);
    Check("hex string as int", root["hex"].ValueAs<int>() == 31);
    Check("negative hex string", root["neg"].ValueAs<int>() == -16);
    Check("On as bool", root["flag"].ValueAs<bool>());
    Check("decimal string as unsigned", root["num"].ValueAs<unsigned>() == 42);
    Check("float as string", root["f"].ValueAs<std::string>() == "2.5");
    Check("missing node gives the default", root["no/such"].ValueAsDefault<int>(5) == 5);
    Check("missing node does not exist", !root["no/such"].Exists());
    Check("missing node keeps the first missing key", root["no/such"].Key() == "no");
    Check("ValueAs on a missing node throws", Throws([&]() { root["absent"].ValueAs<int>(); }));
    Check("Get on a missing node throws", Throws([&]() { root.Get("absent"); }));
    Check("TryGet on a missing node is null", root.TryGet("absent") == nullptr);
    Check("Value of the wrong type throws", Throws([&]() { root["f"].Value<int>(); }));
  }

  // Add and Set on nested paths.
  {
    Node root("cfg");
    root.Add("a/b");
    root.Add("a/b");
    root.Add("c");
    root.Add("c");
    const char *dump =
      "cfg=\"\"  children={ a c }\n"
      "  a=\"\"  children={ b }\n"
      "    b=\"\"  children={ }\n"
      "    b=\"\"  children={ }\n"
      "  c=\"\"  children={ }\n"
      "  c=\"\"  children={ }\n";
    Check("adding a path twice repeats only the leaf", root.ToString() == dump);
    root.Set<std::string>("a/b", "set");
    Check("Set changes the latest node", root["a/b"].Value<std::string>() == "set");
    root.Set<int>("d/e", 3);
    Check("Set adds a missing node", root["d/e"].Value<int>() == 3);
  }

  // XML.
  {
    const char *xml =
      "<top kind=\"k\">\n"
      "  <item n=\"1\">first</item>\n"
      "  <item n=\"2\"/>\n"
      "</top>\n";
    Node config("before");
    bool failed = Util::Config::FromXML(&config, xml);
    const char *dump =
      "xml=\"\"  children={ top }\n"
      "  top=\"\"  children={ item kind }\n"
      "    kind=\"k\"  children={ }\n"
      "    item=\"first\"  children={ n }\n"
      "      n=\"1\"  children={ }\n"
      "    item=\"\"  children={ n }\n"
      "      n=\"2\"  children={ }\n";
    Check("XML parses", !failed);
    Check("XML tree: attributes, then child elements", config.ToString() == dump);
  }

  // INI round trip.
  {
    const char *path = "Test_Config.ini";
    {
      std::ofstream f(path);
      f << "Early = 1\n"
           "[ Global ]\n"
           "Volume = \"50\" ; comment\n"
           "[ one, two ]\n"
           "Shared = yes\n"
           "[ , three ]\n"
           "Name = \"a;b\"\n";
    }
    Node ini("x");
    bool failed = Util::Config::FromINIFile(&ini, path);
    Check("INI parses", !failed);
    Check("settings before any section are global", ini["Early"].ValueAs<int>() == 1);
    Check("quotes and comments are removed", ini["Volume"].Value<std::string>() == "50");
    Check("a setting goes to every listed section",
          ini["one/Shared"].ValueAs<bool>() && ini["two/Shared"].ValueAs<bool>());
    Check("an empty section name is Global, ';' in quotes is kept",
          ini["Name"].Value<std::string>() == "a;b" && ini["three/Name"].Value<std::string>() == "a;b");

    Node merged("m");
    Util::Config::MergeINISections(&merged, ini, ini["one"]);
    Check("merge keeps x's key and adds y's settings",
          merged.Key() == "Global" && merged["Shared"].ValueAs<bool>() && merged["Volume"].ValueAs<int>() == 50);

    // Values are written without quotes, so an unquoted ';' is a comment
    // when the file is read again.
    ini.Set<std::string>("Name", "a b");
    Util::Config::WriteINIFile(path, ini, "; header");
    Node again("y");
    Util::Config::FromINIFile(&again, path);
    Check("written INI reads back",
          again["Early"].ValueAs<int>() == 1 && again["Name"].Value<std::string>() == "a b" &&
          again["two/Shared"].Value<std::string>() == "yes" && again["three/Name"].Value<std::string>() == "a");
    std::remove(path);
  }

  std::cout << (g_failures ? "Some checks FAILED" : "All checks passed") << std::endl;
  return g_failures ? 1 : 0;
}
