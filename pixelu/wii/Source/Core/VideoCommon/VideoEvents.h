// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/HookableEvent.h"

namespace Core
{
class System;
}

// Describes one presentation of a frame to the host display, handed to the present hooks.
struct PresentInfo
{
  // How much the presentation timestamp can be trusted.
  enum class PresentTimeAccuracy
  {
    PresentOnScreenExact,  // reported by the driver as the moment scan-out began
    PresentOnScreen,       // a close estimate of scan-out
    PresentInProgress,     // handed to the driver, will reach the screen shortly
    Unimplemented,         // no information available
  };

  // What caused the presentation.
  enum class PresentReason
  {
    Immediate,                // the FIFO presented an XFB copy straight away
    VideoInterface,           // the emulated VI scanned out a new frame
    VideoInterfaceDuplicate,  // the emulated VI scanned out the previous frame again
  };

  u64 frame_count = 0;         // distinct emulated frames since boot
  u64 present_count = 0;       // presents since the backend started; monotonic
  u64 emulated_timestamp = 0;  // emulated time at which the console would have shown it

  TimePoint intended_present_time{};
  TimePoint actual_present_time{};  // only filled in for after_present_event
  PresentTimeAccuracy present_time_accuracy = PresentTimeAccuracy::Unimplemented;

  PresentReason reason = PresentReason::Immediate;

  u32 frame_buffer_width;
  u32 frame_buffer_height;

  std::vector<std::string_view> xfb_copy_hashes;
};

// Hook points the video code fires so that other subsystems can follow frame progress.
struct VideoEvents
{
  // A video setting changed; the argument holds the changed-config bits.
  Common::HookableEvent<u32> config_changed_event;

  // Fired right before the first draw of a frame.
  Common::HookableEvent<> before_frame_event;

  // Fired once the XFB copy that ends a frame has been submitted to the host GPU. A game that
  // stitches several XFB copies into one image fires it more than once per shown frame; use
  // before_present_event when that matters.
  Common::HookableEvent<Core::System&> after_frame_event;

  // Fired when a frame is queued for display. With "Immediately Present XFB" this follows
  // after_frame_event directly; otherwise it waits for the emulated VI to start the next field.
  Common::HookableEvent<PresentInfo&> before_present_event;

  // Fired after the host has presented a frame; timing depends on what the backend can observe.
  Common::HookableEvent<PresentInfo&> after_present_event;

  // Fired on the CPU thread at the end of each emulated VI field.
  Common::HookableEvent<> vi_end_field_event;
};

VideoEvents& GetVideoEvents();
