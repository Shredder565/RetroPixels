// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include <string>
#include <vector>

namespace optparse
{
class OptionParser;
class Values;
}  // namespace optparse

// The emulator's command line, shared by the GUI and the headless front ends. Parsing also installs
// a CommandLine config layer holding the settings the arguments override.
namespace CommandLineParse
{
enum class ParserOptions
{
  IncludeGUIOptions,
  OmitGUIOptions,
};

std::unique_ptr<optparse::OptionParser> CreateParser(ParserOptions options);
optparse::Values& ParseArguments(optparse::OptionParser* parser, int argc, char** argv);
optparse::Values& ParseArguments(optparse::OptionParser* parser,
                                 const std::vector<std::string>& arguments);
}  // namespace CommandLineParse
