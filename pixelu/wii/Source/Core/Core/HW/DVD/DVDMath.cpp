// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/DVD/DVDMath.h"

#include <cmath>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"

namespace DVDMath
{
namespace
{
// Geometry of a 12 cm DVD as used by the Wii (and the 8 cm GameCube disc, which
// has the same track pitch and data density, just a smaller outer edge).
constexpr double R_INNER = 0.024;        // first data track, metres
constexpr double R_OUTER_WII = 0.058;    // last track of a full layer
constexpr double R_OUTER_GC = 0.038;     // last track of a mini DVD
constexpr double TRACK_PITCH = 0.74e-6;  // metres between spiral turns

// One layer of a Wii disc holds 2294912 sectors of 2 KiB.
constexpr u64 LAYER_BYTES = 0x118240000;

constexpr double R_INNER_SQ = R_INNER * R_INNER;
constexpr double LAYER_AREA_TERM = R_OUTER_WII * R_OUTER_WII - R_INNER * R_INNER;

// Measured drive throughput (bytes per second) at the inner and outer edge.
// The drive spins at a constant angular velocity, so throughput grows
// linearly with radius between these two points.
struct ThroughputCurve
{
  double outer_radius;
  double inner_rate;
  double outer_rate;
  double rotations_per_second;
};

constexpr double MIB = 1024 * 1024;
constexpr ThroughputCurve GC_CURVE{R_OUTER_GC, MIB * 2.1, MIB * 3.325, 28.5};
constexpr ThroughputCurve WII_CURVE{R_OUTER_WII, MIB * 3.48, MIB * 8.41, 57};

const ThroughputCurve& CurveFor(bool wii_disc)
{
  return wii_disc ? WII_CURVE : GC_CURVE;
}

// Seek model fitted to hardware measurements: a fixed settle time plus a
// travel time proportional to the radial distance. Moves under a millimetre
// use a slower, separate fit.
constexpr double NEAR_SEEK_LIMIT = 0.001;
constexpr double NEAR_SEEK_BASE = 0.035;
constexpr double NEAR_SEEK_SECONDS_PER_METRE = 50;
constexpr double FAR_SEEK_BASE = 0.075;
constexpr double FAR_SEEK_SECONDS_PER_METRE = 4.5;
}  // namespace

// The spiral is approximated by spreading the recorded area of the disc into a
// strip one track pitch wide. A byte's share of the layer is then its share of
// the annulus area between R_INNER and the position, which gives
//   r^2 = fraction * (R_OUTER^2 - R_INNER^2) + R_INNER^2.
// The second layer runs from the outside back in (opposite track path).
double CalculatePhysicalDiscPosition(u64 offset)
{
  u64 in_layer = offset % (LAYER_BYTES * 2);
  if (in_layer > LAYER_BYTES)
    in_layer = LAYER_BYTES * 2 - in_layer;

  const double fraction = static_cast<double>(in_layer) / LAYER_BYTES;
  return std::sqrt(fraction * LAYER_AREA_TERM + R_INNER_SQ);
}

double CalculateSeekTime(u64 offset_from, u64 offset_to)
{
  const double travel = std::fabs(CalculatePhysicalDiscPosition(offset_from) -
                                  CalculatePhysicalDiscPosition(offset_to));

  if (travel < NEAR_SEEK_LIMIT)
    return travel * NEAR_SEEK_SECONDS_PER_METRE + NEAR_SEEK_BASE;
  return travel * FAR_SEEK_SECONDS_PER_METRE + FAR_SEEK_BASE;
}

// Angles are measured in whole turns (0..1). The disc is assumed to have been
// spinning at a constant rate since time zero. Each track turn advances the
// radius by one pitch, so the number of turns from the centre to a byte gives
// its angle on the platter.
double CalculateRotationalLatency(u64 offset, double time, bool wii_disc)
{
  const double rps = CurveFor(wii_disc).rotations_per_second;

  const double byte_angle = std::fmod(CalculatePhysicalDiscPosition(offset) / TRACK_PITCH, 1.0);
  const double head_angle = std::fmod(time * rps, 1.0);
  // Add a full turn first so the remainder is taken of a positive value.
  const double turns_to_wait = std::fmod(byte_angle + 1.0 - head_angle, 1.0);

  const double seconds = turns_to_wait / rps;
  DEBUG_LOG_FMT(DVDINTERFACE, "DVD rotational wait {} ms", seconds * 1000);
  return seconds;
}

// Throughput is sampled at the radius of the middle byte of the transfer; it
// barely changes across one request. DMA overhead is folded into the rates.
double CalculateRawDiscReadTime(u64 offset, u64 length, bool wii_disc)
{
  const ThroughputCurve& curve = CurveFor(wii_disc);
  const double radius = CalculatePhysicalDiscPosition(offset + length / 2);

  const double rate = (radius - R_INNER) / (curve.outer_radius - R_INNER) *
                          (curve.outer_rate - curve.inner_rate) +
                      curve.inner_rate;

  const double seconds = static_cast<double>(length) / rate;
  DEBUG_LOG_FMT(DVDINTERFACE, "DVD stream {:#x} bytes at {:#x} (r={} mm): {} us at {} MiB/s",
                length, offset, radius * 1000, seconds * 1e6, rate / MIB);
  return seconds;
}

}  // namespace DVDMath
