// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

// Layer-backed view whose backing layer is a CAMetalLayer. On macOS the Vulkan
// backend (MoltenVK) creates its surface on this layer, see CocoaSurface.mm.
@interface MetalView : NSView
@end
