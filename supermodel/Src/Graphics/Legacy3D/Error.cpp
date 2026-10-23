// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Error.cpp
//
// Problems met while drawing would otherwise be logged for every model of
// every frame; each kind is logged at most once per frame.

#include "Supermodel.h"
#include "Legacy3D.h"

namespace Legacy3D {

namespace {
constexpr unsigned kLocalOverflow = 1u << 0;
constexpr unsigned kUncachable = 1u << 1;
}

Result CLegacy3D::ReportLocalOverflow()
{
  if (m_reported & kLocalOverflow)
    return Result::FAIL;
  m_reported |= kLocalOverflow;
  return ErrorLog("Legacy3D: a model has more triangles than its build buffer holds.");
}

Result CLegacy3D::ReportUncachable(uint32_t addr)
{
  if (m_reported & kUncachable)
    return Result::FAIL;
  m_reported |= kUncachable;
  return ErrorLog("Legacy3D: model at %08X does not fit in the model cache.", addr);
}

void CLegacy3D::ResetReports()
{
  m_reported = 0;
}

} // Legacy3D
