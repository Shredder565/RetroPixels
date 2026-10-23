// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Debug builds only (-gfx-state): a web page for looking through the frames
// drawn with one polygon header or culling node bit highlighted at a time.
// Each tested bit is a cell of a word-by-bit grid; clicking it shows that
// bit's picture.

#ifndef INCLUDED_POLYANALYSIS_H
#define INCLUDED_POLYANALYSIS_H

#include "Util/Format.h"

#include <cstdint>
#include <string>
#include <vector>

namespace PolyAnalysis
{
  // The picture of one highlighted bit: <base>.<kind>.<word>_<mask>.bmp
  inline std::string ImageName(const std::string &base, const char *kind, int word, uint32_t mask)
  {
    return Util::Format() << base << "." << kind << "." << word << "_" << Util::Hex(mask) << ".bmp";
  }

  inline std::string Grid(const std::string &base, const char *kind, const char *title, const std::vector<uint32_t> &tested)
  {
    std::string html = std::string("<section><h2>") + title + "</h2><table><tr><th></th>";
    for (int bit = 31; bit >= 0; bit--)
      html += "<th>" + std::to_string(bit) + "</th>";
    html += "</tr>\n";
    for (size_t word = 0; word < tested.size(); word++)
    {
      html += "<tr><th>" + std::to_string(word) + "</th>";
      for (int bit = 31; bit >= 0; bit--)
      {
        const uint32_t mask = uint32_t(1) << bit;
        if (tested[word] & mask)
          html += "<td class='on' data-img='" + ImageName(base, kind, int(word), mask) + "'></td>";
        else
          html += "<td></td>";
      }
      html += "</tr>\n";
    }
    return html + "</table></section>\n";
  }

  inline std::string Page(const std::string &base, const std::vector<uint32_t> &polyBits, const std::vector<uint32_t> &cullingBits)
  {
    return
      "<!doctype html>\n<html><head><meta charset='utf-8'><title>" + base + "</title>\n"
      "<style>\n"
      "  body { font-family: sans-serif; }\n"
      "  section { display: inline-block; vertical-align: top; margin-right: 2em; }\n"
      "  td, th { width: 1.1em; height: 1.1em; font-size: 70%; text-align: center; }\n"
      "  td { background: #888; }\n"
      "  td.on { background: #46c; cursor: pointer; }\n"
      "  td.shown { background: #2d2; }\n"
      "</style></head><body>\n"
      + Grid(base, "poly", "Polygon header", polyBits)
      + Grid(base, "culling", "Culling node", cullingBits) +
      "<p id='name'></p><img id='picture' alt=''>\n"
      "<script>\n"
      "  document.querySelectorAll('td.on').forEach(function (cell) {\n"
      "    cell.addEventListener('click', function () {\n"
      "      document.querySelectorAll('td.shown').forEach(function (c) { c.classList.remove('shown'); });\n"
      "      cell.classList.add('shown');\n"
      "      document.getElementById('picture').src = cell.dataset.img;\n"
      "      document.getElementById('name').textContent = cell.dataset.img;\n"
      "    });\n"
      "  });\n"
      "</script>\n"
      "</body></html>\n";
  }
}

#endif // INCLUDED_POLYANALYSIS_H
