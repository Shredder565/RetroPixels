// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Renderer/MetalView.h"

@implementation MetalView

// the GPU fills the layer, AppKit never has to draw the view
- (BOOL)wantsUpdateLayer
{
	return YES;
}

- (CALayer*)makeBackingLayer
{
	CAMetalLayer* metalLayer = [CAMetalLayer layer];
	// pixel density of the screen the view is on (2 on Retina displays)
	NSWindow* window = self.window;
	metalLayer.contentsScale = window ? window.backingScaleFactor : [NSScreen mainScreen].backingScaleFactor;
	return metalLayer;
}

// keep the density right when the window moves to another screen
- (BOOL)layer:(CALayer*)layer shouldInheritContentsScale:(CGFloat)newScale fromWindow:(NSWindow*)window
{
	if (layer.contentsScale == newScale)
		return NO;
	layer.contentsScale = newScale;
	return YES;
}

@end
