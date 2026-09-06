/*
 *  macos_window_bridge.mm - Host macOS Cocoa window mirroring bridge
 *
 *  Cockatrice III
 *  (C) 2026 Cockatrice III Project
 *
 *  High-Level Architectural Context:
 *  ==================================
 *  toolbox_window.cpp watches the guest Window Manager and decides which windows
 *  exist, where they are and what they are called. This file turns each of those
 *  into a real NSWindow with native chrome, and paints the guest's pixels into
 *  its content view.
 *
 *  The chrome is deliberately the host's: only the window's *content* region is
 *  drawn, so the Classic WDEF title bar is cropped away and macOS supplies the
 *  title bar, close button and resize grip. Inside the frame everything stays
 *  authentically Classic -- the buttons, fonts and controls are the guest's own
 *  pixels.
 *
 *  Threading:
 *  ----------
 *  Every callback here arrives on the CPU thread, which in this build is also
 *  the Cocoa main thread: Start680x0() is called from applicationDidFinishLaunching:,
 *  so [NSApp run]'s loop is blocked and the main queue is only serviced
 *  incidentally by SDL_PollEvent() inside VideoInterrupt(). Two consequences
 *  shape this file:
 *
 *    1. Cocoa calls made from a callback are already on the right thread, so
 *       they are made directly rather than dispatched.
 *    2. Cocoa event handlers (the close button) run re-entrantly *inside* the
 *       68k interrupt. They must never call into the guest, so they post to the
 *       MenuQueue and let the CPU thread act on it at a safe point -- the same
 *       contract every MenuAction_* function follows.
 */

#import <Cocoa/Cocoa.h>
#include <stdio.h>
#include <string>
#include <vector>

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "main.h"
#include "menu_bar.h"
#include "prefs.h"
#include "toolbox_window.h"
#include "macos_window_bridge.h"

/* =========================================================================
 *  Content view: draws one guest window's pixels
 * ====================================================================== */

@interface GuestWindowView : NSView
{
	const void *_pixels;      // Host pointer to the top-left pixel
	int32 _rowBytes;
	int32 _pixelWidth;
	int32 _pixelHeight;
	int16 _pixelSize;
}
@property (nonatomic, assign) uint32 windowPtr;
- (void)setPixels:(const void *)pixels
         rowBytes:(int32)rowBytes
            width:(int32)width
           height:(int32)height
        pixelSize:(int16)pixelSize;
@end

@implementation GuestWindowView

/*
 * Converts a Cocoa event location into guest content-local coordinates.
 *
 * Two conversions are involved: Cocoa's y runs up from the bottom where the
 * guest's runs down from the top, and the view may not be the same size as the
 * guest window if the host was resized and the guest has not caught up yet.
 */
- (NSPoint)guestPointForEvent:(NSEvent *)event
{
	NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
	NSRect bounds = [self bounds];
	CGFloat sx = (bounds.size.width > 0 && _pixelWidth > 0)
		? (CGFloat)_pixelWidth / bounds.size.width : 1.0;
	CGFloat sy = (bounds.size.height > 0 && _pixelHeight > 0)
		? (CGFloat)_pixelHeight / bounds.size.height : 1.0;
	return NSMakePoint(p.x * sx, (bounds.size.height - p.y) * sy);
}

- (void)forwardMouse:(NSEvent *)event kind:(int)kind
{
	if (!self.windowPtr)
		return;
	NSPoint p = [self guestPointForEvent:event];
	MenuAction_GuestInput((int)self.windowPtr, kind, (int)p.x, (int)p.y, 0);
}

- (BOOL)acceptsFirstResponder { return YES; }

/*
 * A tracking area is what makes hover work.
 *
 * -mouseMoved: alone is not enough: AppKit only delivers it to the key window,
 * so moving over a mirrored window that is not focused would leave the guest
 * cursor stranded wherever it was last put -- and the guest decides the cursor
 * shape, and the Finder its highlighting, from that position. NSTrackingActiveAlways
 * gets the events regardless of focus, and -mouseEntered: puts the guest cursor
 * in the right place the moment the pointer arrives rather than on the first
 * movement after that.
 */
- (void)updateTrackingAreas
{
	[super updateTrackingAreas];
	for (NSTrackingArea *area in [[self trackingAreas] copy])
		[self removeTrackingArea:area];

	NSTrackingArea *area = [[NSTrackingArea alloc]
		initWithRect:NSZeroRect
		     options:(NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
		              NSTrackingActiveAlways | NSTrackingInVisibleRect)
		       owner:self
		    userInfo:nil];
	[self addTrackingArea:area];
}

- (void)mouseEntered:(NSEvent *)event { [self forwardMouse:event kind:kGuestInput_MouseMove]; }

/* Let a click into an inactive window reach the guest as well as focusing it,
   which is how the Finder behaves */
- (BOOL)acceptsFirstMouse:(NSEvent *)event { (void)event; return YES; }

- (void)mouseDown:(NSEvent *)event    { [self forwardMouse:event kind:kGuestInput_MouseDown]; }
- (void)mouseUp:(NSEvent *)event      { [self forwardMouse:event kind:kGuestInput_MouseUp]; }
- (void)mouseDragged:(NSEvent *)event { [self forwardMouse:event kind:kGuestInput_MouseMove]; }
- (void)mouseMoved:(NSEvent *)event   { [self forwardMouse:event kind:kGuestInput_MouseMove]; }

/*
 * macOS virtual key codes are the original ADB key codes, so they are passed
 * through unchanged.
 */
- (void)keyDown:(NSEvent *)event
{
	if (self.windowPtr)
		MenuAction_GuestInput((int)self.windowPtr, kGuestInput_KeyDown, 0, 0, [event keyCode]);
}

- (void)keyUp:(NSEvent *)event
{
	if (self.windowPtr)
		MenuAction_GuestInput((int)self.windowPtr, kGuestInput_KeyUp, 0, 0, [event keyCode]);
}

- (void)setPixels:(const void *)pixels
         rowBytes:(int32)rowBytes
            width:(int32)width
           height:(int32)height
        pixelSize:(int16)pixelSize
{
	_pixels = pixels;
	_rowBytes = rowBytes;
	_pixelWidth = width;
	_pixelHeight = height;
	_pixelSize = pixelSize;
	[self setNeedsDisplay:YES];
}

/*
 * Builds a CGImage over the guest pixels and draws it.
 *
 * The image is rebuilt every frame rather than cached: the guest writes into
 * that memory continuously, so any cached image would have to be invalidated on
 * every write anyway. Copying happens inside CGImageCreate, which is fast enough
 * at the frame sizes involved here.
 */
- (void)drawRect:(NSRect)dirtyRect
{
	if (!_pixels || _pixelWidth <= 0 || _pixelHeight <= 0) {
		// Nothing to show yet: a window we know about but have not been handed
		// pixels for. Neutral fill rather than garbage.
		[[NSColor windowBackgroundColor] setFill];
		NSRectFill(dirtyRect);
		return;
	}

	CGColorSpaceRef colorSpace = NULL;
	size_t bitsPerComponent = 8;
	size_t bitsPerPixel = (size_t)_pixelSize;
	CGBitmapInfo bitmapInfo = kCGImageAlphaNone;

	if (_pixelSize == 8) {
		const uint8 *palette = ToolboxWindow_GetPalette();
		if (!palette)
			return;
		CGColorSpaceRef base = CGColorSpaceCreateDeviceRGB();
		colorSpace = CGColorSpaceCreateIndexed(base, 255, palette);
		CGColorSpaceRelease(base);
	} else if (_pixelSize == 32) {
		colorSpace = CGColorSpaceCreateDeviceRGB();
		// Guest 32-bit pixels are xRGB, big-endian as seen from the host
		bitmapInfo = kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Big;
	} else if (_pixelSize == 16) {
		colorSpace = CGColorSpaceCreateDeviceRGB();
		bitsPerComponent = 5;
		bitmapInfo = kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder16Big;
	} else {
		return;
	}
	if (!colorSpace)
		return;

	size_t dataLength = (size_t)_rowBytes * (size_t)_pixelHeight;
	CGDataProviderRef provider =
		CGDataProviderCreateWithData(NULL, _pixels, dataLength, NULL);
	if (!provider) {
		CGColorSpaceRelease(colorSpace);
		return;
	}

	CGImageRef image = CGImageCreate((size_t)_pixelWidth, (size_t)_pixelHeight,
	                                 bitsPerComponent, bitsPerPixel,
	                                 (size_t)_rowBytes, colorSpace, bitmapInfo,
	                                 provider, NULL, false,
	                                 kCGRenderingIntentDefault);
	CGDataProviderRelease(provider);
	CGColorSpaceRelease(colorSpace);
	if (!image)
		return;

	/* No CTM flip here: this view is not flipped, and Quartz already draws a
	   CGImage the right way up in a bottom-up context. Flipping as well turns
	   the picture upside down. */
	CGContextRef ctx = (CGContextRef)[[NSGraphicsContext currentContext] CGContext];
	CGContextSaveGState(ctx);
	CGContextSetInterpolationQuality(ctx, kCGInterpolationNone);
	CGContextDrawImage(ctx, CGRectMake(0, 0, self.bounds.size.width,
	                                   self.bounds.size.height), image);
	CGContextRestoreGState(ctx);
	CGImageRelease(image);
}

@end

/* =========================================================================
 *  Per-window holder
 * ====================================================================== */

@interface MirrorWindow : NSObject <NSWindowDelegate>
@property (nonatomic, assign) NSWindow *window;
@property (nonatomic, assign) GuestWindowView *view;   // nil for natively rendered dialogs
@property (nonatomic, assign) uint32 windowPtr;
@property (nonatomic, assign) BOOL isDialog;
// Set while the geometry is being pushed from the guest, so that the move and
// resize notifications Cocoa raises in response are not sent straight back
@property (nonatomic, assign) BOOL applyingSnapshot;
// The host frame origin last applied from the guest, and the guest content
// origin it came from. A drag is measured as the difference from these.
@property (nonatomic, assign) NSPoint appliedHostOrigin;
@property (nonatomic, assign) NSPoint appliedGuestOrigin;
- (void)dialogButtonClicked:(id)sender;
@end

@implementation MirrorWindow

/*
 * The host close button is a request, not an order: the guest owns the window
 * and may want to put up a "save changes?" dialog. So the close is forwarded and
 * NO returned; the window disappears when the guest actually disposes it and the
 * next window-list walk notices.
 */
- (BOOL)windowShouldClose:(NSWindow *)sender
{
	MenuAction_GuestWindowClose((int)self.windowPtr);
	return NO;
}

/*
 * A button in a natively rendered dialog was pressed.
 *
 * Runs on the Cocoa main thread, which is also the CPU thread and may well be
 * inside the 68k interrupt right now, so nothing here touches the guest: the
 * item number goes onto the MenuQueue and the CPU thread presses it at a safe
 * point.
 */
- (void)dialogButtonClicked:(id)sender
{
	if (![sender isKindOfClass:[NSButton class]])
		return;
	MenuAction_GuestDialogClick((int)self.windowPtr, (int)[(NSButton *)sender tag]);
}

/*
 * Focusing a host window brings the matching guest window to the front.
 *
 * Without this the guest's idea of which window is in front drifts from the
 * host's, and a click forwarded into a background window would be delivered to
 * whatever the guest still had on top.
 */
- (void)windowDidBecomeKey:(NSNotification *)note
{
	(void)note;
	if (!self.isDialog)
		MenuAction_GuestWindowSelect((int)self.windowPtr);
}

/*
 * A host resize is passed to the guest, which resizes its own window; the next
 * window-list walk then sees the new size and, when redirection is on, gives it
 * a buffer to match.
 */
- (void)windowDidResize:(NSNotification *)note
{
	(void)note;
	if (self.isDialog || self.applyingSnapshot)
		return;
	NSRect content = [self.window contentRectForFrameRect:[self.window frame]];
	MenuAction_GuestWindowResize((int)self.windowPtr,
	                             (int)content.size.width, (int)content.size.height);
}

/*
 * Dragging the host window by its title bar moves the guest window to match, so
 * that forwarded mouse coordinates keep landing where the user is pointing.
 *
 * What is forwarded is the *movement*, not the position. Cocoa moves windows on
 * its own account -- it will not put a title bar under the menu bar, so a window
 * near the top of the guest screen is nudged down as it appears -- and the host
 * frame is therefore not always where the guest was asked to put it. Measuring
 * from the frame we last applied means such a nudge is absorbed once, at the
 * moment it happens, instead of being fed back as a move the guest never made.
 * A frame that has not moved since then reports nothing at all, which is what
 * breaks the loop where each side keeps correcting the other.
 */
- (void)windowDidMove:(NSNotification *)note
{
	(void)note;
	if (self.isDialog || self.applyingSnapshot)
		return;

	NSPoint origin = [self.window frame].origin;
	CGFloat dx = origin.x - self.appliedHostOrigin.x;
	CGFloat dy = origin.y - self.appliedHostOrigin.y;
	if (dx == 0 && dy == 0)
		return;

	/*
	 * Only a drag is forwarded. Cocoa also repositions windows with no button
	 * down -- constraining them out from under the menu bar, tidying them onto a
	 * screen -- and that has to be absorbed, not sent on: moving a Finder window
	 * it never asked to move makes the Finder rebuild its windows, which retires
	 * and re-adopts every mirror and starts the whole exchange again.
	 *
	 * Absorbing means adopting the new position as the baseline, so the offset
	 * is forgotten rather than accumulating into the next real drag.
	 */
	if (([NSEvent pressedMouseButtons] & 1) == 0) {
		self.appliedHostOrigin = origin;
		return;
	}

	// Cocoa's y grows upwards, the guest's downwards
	MenuAction_GuestWindowMove((int)self.windowPtr,
	                           (int)(self.appliedGuestOrigin.x + dx),
	                           (int)(self.appliedGuestOrigin.y - dy));
}

@end

// WindowPtr -> MirrorWindow
static NSMutableDictionary<NSNumber *, MirrorWindow *> *g_windows = nil;

/*
 * Converts a string out of guest memory into an NSString.
 *
 * Classic Mac OS text is Mac OS Roman, not UTF-8, so anything above ASCII --
 * curly quotes and the ellipsis that Apple's own dialogs are full of -- would
 * otherwise fail to decode.
 */
static NSString *nsstring_from_mac(const std::string &text)
{
	NSString *s = [[NSString alloc] initWithBytes:text.data()
	                                       length:text.size()
	                                     encoding:NSMacOSRomanStringEncoding];
	if (!s)
		s = [NSString stringWithUTF8String:text.c_str()];
	return s ? s : @"";
}

/*
 * Converts a guest global content rect into a host content rect on the main
 * screen. Guest coordinates run y-down from the top-left of the guest screen;
 * Cocoa runs y-up from the bottom-left, hence the flip.
 */
static NSRect host_rect_for_guest(int16 left, int16 top, int16 right, int16 bottom)
{
	NSScreen *screen = [NSScreen mainScreen];
	NSRect frame = screen ? [screen frame] : NSMakeRect(0, 0, 1920, 1080);
	CGFloat width = (CGFloat)(right - left);
	CGFloat height = (CGFloat)(bottom - top);
	return NSMakeRect(frame.origin.x + (CGFloat)left,
	                  frame.origin.y + frame.size.height - (CGFloat)bottom,
	                  width > 1 ? width : 1,
	                  height > 1 ? height : 1);
}

/*
 * Applies a snapshot's geometry and title to an existing host window.
 */
static void apply_snapshot(MirrorWindow *mirror, const MacWindowSnapshot *w)
{
	NSRect content = host_rect_for_guest(w->content_left, w->content_top,
	                                     w->content_right, w->content_bottom);
	// Held across the whole update, ordering included: showing a window is
	// another moment Cocoa may reposition it, and that must not come back to us
	mirror.applyingSnapshot = YES;

	NSRect want = [mirror.window frameRectForContentRect:content];
	if (!NSEqualRects(want, [mirror.window frame]))
		[mirror.window setFrame:want display:YES];

	NSString *title = nsstring_from_mac(w->title);
	if (!title || [title length] == 0)
		title = @"Untitled";
	if (![title isEqualToString:[mirror.window title]])
		[mirror.window setTitle:title];

	if (w->visible && ![mirror.window isVisible])
		[mirror.window orderFront:nil];
	else if (!w->visible && [mirror.window isVisible])
		[mirror.window orderOut:nil];

	// Record where this left the host window -- the *actual* frame, which Cocoa
	// may have adjusted -- so a later drag is measured against it
	mirror.appliedHostOrigin = [mirror.window frame].origin;
	mirror.appliedGuestOrigin = NSMakePoint(w->content_left, w->content_top);
	mirror.applyingSnapshot = NO;
}

/* =========================================================================
 *  Callbacks from toolbox_window.cpp  [CPU thread]
 * ====================================================================== */

/*
 * Builds a native content view for a guest dialog: the message as a real label,
 * the dialog's buttons as real NSButtons.
 *
 * This is what "mapping alerts across" means in practice -- rather than showing a
 * picture of a Classic alert, the host gets a panel it can style, focus and
 * key-navigate like any other, while the guest still believes an ordinary
 * ModalDialog is running and is driven by synthetic clicks.
 *
 * Returns:
 *   YES if the dialog was decoded and a native view installed.
 */
static BOOL build_native_dialog(MirrorWindow *mirror, const MacWindowSnapshot *w,
                                NSRect contentRect)
{
	std::vector<MacDialogItem> items;
	int defaultItem = 0;
	if (!Toolbox_SnapshotDialogItems(w->window_ptr, items, &defaultItem))
		return NO;

	// Collect the message text and the pressable buttons
	NSMutableArray<NSString *> *lines = [NSMutableArray array];
	std::vector<const MacDialogItem *> buttons;
	for (size_t i = 0; i < items.size(); i++) {
		const MacDialogItem &item = items[i];
		if (item.type == kDialogItem_StaticText && !item.text.empty())
			[lines addObject:nsstring_from_mac(item.text)];
		else if (item.type == kDialogItem_Button && item.enabled)
			buttons.push_back(&items[i]);
	}
	if (buttons.empty())
		return NO; // nothing actionable; fall back to mirroring the pixels

	NSView *content = [[NSView alloc] initWithFrame:
		NSMakeRect(0, 0, contentRect.size.width, contentRect.size.height)];

	const CGFloat pad = 20.0;
	const CGFloat buttonHeight = 32.0;
	const CGFloat buttonGap = 12.0;

	// Buttons along the bottom, right aligned. DITL item 1 is conventionally the
	// default action, and on macOS the default button sits rightmost.
	CGFloat x = contentRect.size.width - pad;
	for (size_t i = 0; i < buttons.size(); i++) {
		const MacDialogItem *item = buttons[i];
		NSButton *button = [[NSButton alloc] initWithFrame:NSMakeRect(0, 0, 90, buttonHeight)];
		[button setTitle:nsstring_from_mac(item->text)];
		[button setBezelStyle:NSBezelStyleRounded];
		[button setTarget:mirror];
		[button setAction:@selector(dialogButtonClicked:)];
		[button setTag:item->index];
		[button sizeToFit];

		NSRect frame = [button frame];
		frame.size.width = frame.size.width < 90 ? 90 : frame.size.width + 20;
		frame.size.height = buttonHeight;
		frame.origin.x = x - frame.size.width;
		frame.origin.y = pad;
		[button setFrame:frame];

		if (item->index == defaultItem)
			[button setKeyEquivalent:@"\r"];

		[content addSubview:button];
		x = frame.origin.x - buttonGap;
	}

	// Message text fills the space above the buttons
	CGFloat textBottom = pad + buttonHeight + buttonGap;
	NSTextField *label = [[NSTextField alloc] initWithFrame:
		NSMakeRect(pad, textBottom,
		           contentRect.size.width - pad * 2,
		           contentRect.size.height - textBottom - pad)];
	[label setStringValue:[lines componentsJoinedByString:@"\n\n"]];
	[label setEditable:NO];
	[label setSelectable:YES];
	[label setBordered:NO];
	[label setDrawsBackground:NO];
	[label setFont:[NSFont systemFontOfSize:13.0]];
	[[label cell] setWraps:YES];
	[content addSubview:label];

	[mirror.window setContentView:content];
	mirror.isDialog = YES;

	printf("[WIN-BRIDGE] dialog \"%s\" mapped to a native alert: %d button(s), default %d\n",
	       w->title.c_str(), (int)buttons.size(), defaultItem);
	fflush(stdout);
	return YES;
}

/*
 * Maps a classic window definition onto the nearest host window.
 *
 * A Mac OS window's procID says what kind of window it is, and each kind has a
 * fair macOS equivalent; without this every window came out as a resizable
 * document window, so alerts arrived with a grow box and modal dialogs could be
 * zoomed. The variation code is the low four bits of the procID:
 *
 *   0 documentProc     movable, sizable, no zoom box
 *   1 dBoxProc         alert box or modal dialog: a panel, fixed size
 *   2 plainDBox        plain box: no frame at all
 *   3 altDBoxProc      plain box with a shadow
 *   4 noGrowDocProc    movable, no size box or zoom box
 *   5 movableDBoxProc  movable modal dialog
 *   8 zoomDocProc      standard document window
 *  12 zoomNoGrow       zoomable but not resizable
 *
 * The close box is not implied by the type -- an application passes goAwayFlag
 * to _NewWindow separately -- so it is taken from the window record.
 *
 * Args:
 *   w: the window being mirrored.
 *   floating_out: set when the host window should sit above ordinary windows,
 *     which is the closest macOS has to a classic modal dialog.
 *   zoomable_out: set when the zoom button should be left enabled.
 */
static NSUInteger style_for_window(const MacWindowSnapshot *w,
                                   BOOL *floating_out, BOOL *zoomable_out)
{
	*floating_out = NO;
	*zoomable_out = NO;

	switch (w->variant) {
	case kWindowVariant_PlainDBox:
	case kWindowVariant_AltDBox:
		// No frame of any kind on the guest side, so none here either; the
		// shadow altDBoxProc draws is what every macOS window has anyway
		*floating_out = YES;
		return NSWindowStyleMaskBorderless;

	case kWindowVariant_DBox:
		// A modal dialog: it may not be moved, resized or closed by its frame
		*floating_out = YES;
		return NSWindowStyleMaskTitled;

	case kWindowVariant_MovableDBox:
		*floating_out = YES;
		return NSWindowStyleMaskTitled |
		       (w->go_away ? NSWindowStyleMaskClosable : 0);

	case kWindowVariant_NoGrowDoc:
		return NSWindowStyleMaskTitled | NSWindowStyleMaskMiniaturizable |
		       (w->go_away ? NSWindowStyleMaskClosable : 0);

	case kWindowVariant_ZoomNoGrow:
		*zoomable_out = YES;
		return NSWindowStyleMaskTitled | NSWindowStyleMaskMiniaturizable |
		       (w->go_away ? NSWindowStyleMaskClosable : 0);

	case kWindowVariant_ZoomDoc:
		*zoomable_out = YES;
		return NSWindowStyleMaskTitled | NSWindowStyleMaskMiniaturizable |
		       NSWindowStyleMaskResizable |
		       (w->go_away ? NSWindowStyleMaskClosable : 0);

	case kWindowVariant_Document:
	default:
		return NSWindowStyleMaskTitled | NSWindowStyleMaskMiniaturizable |
		       NSWindowStyleMaskResizable |
		       (w->go_away ? NSWindowStyleMaskClosable : 0);
	}
}

static void bridge_window_added(const MacWindowSnapshot *w)
{
	if (!g_windows || !w)
		return;
	NSNumber *key = @(w->window_ptr);
	if (g_windows[key])
		return;

	NSRect content = host_rect_for_guest(w->content_left, w->content_top,
	                                     w->content_right, w->content_bottom);

	BOOL floating = NO, zoomable = NO;
	NSUInteger style = style_for_window(w, &floating, &zoomable);

	NSWindow *window = [[NSWindow alloc] initWithContentRect:content
	                                              styleMask:style
	                                                backing:NSBackingStoreBuffered
	                                                  defer:NO];
	[window setReleasedWhenClosed:NO];
	if (floating)
		[window setLevel:NSFloatingWindowLevel];
	if ((style & NSWindowStyleMaskTitled) && !zoomable)
		[[window standardWindowButton:NSWindowZoomButton] setEnabled:NO];

	MirrorWindow *mirror = [[MirrorWindow alloc] init];
	mirror.window = window;
	mirror.windowPtr = w->window_ptr;
	mirror.isDialog = NO;
	// Baseline the geometry before the delegate is attached, so the first
	// notification is measured against something real rather than against zero
	mirror.appliedHostOrigin = [window frame].origin;
	mirror.appliedGuestOrigin = NSMakePoint(w->content_left, w->content_top);
	[window setDelegate:mirror];

	/*
	 * Everything is mirrored as pixels by default, dialogs included, so that one
	 * application's windows all look alike. Rebuilding a dialog out of host
	 * controls reads better in isolation but is inconsistent beside the windows
	 * that cannot be rebuilt, and it only works for item lists that decode --
	 * hence the pref rather than a rule.
	 */
	if (!w->is_dialog || !PrefsFindBool("native_alerts") ||
	    !build_native_dialog(mirror, w, content)) {
		GuestWindowView *view = [[GuestWindowView alloc] initWithFrame:
			NSMakeRect(0, 0, content.size.width, content.size.height)];
		[view setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
		view.windowPtr = w->window_ptr;
		[window setContentView:view];
		[window setAcceptsMouseMovedEvents:YES];
		[window makeFirstResponder:view];
		mirror.view = view;
	}

	g_windows[key] = mirror;
	apply_snapshot(mirror, w);

	printf("[WIN-BRIDGE] mirrored \"%s\" (ptr=0x%08X) at (%d,%d) %dx%d, variant %d%s\n",
	       w->title.c_str(), (unsigned)w->window_ptr,
	       w->content_left, w->content_top,
	       w->content_right - w->content_left,
	       w->content_bottom - w->content_top,
	       (int)w->variant, w->go_away ? ", close box" : "");
	fflush(stdout);
}

static void bridge_window_changed(const MacWindowSnapshot *w)
{
	if (!g_windows || !w)
		return;
	MirrorWindow *mirror = g_windows[@(w->window_ptr)];
	if (mirror)
		apply_snapshot(mirror, w);
}

static void bridge_window_removed(uint32 window_ptr)
{
	if (!g_windows)
		return;
	NSNumber *key = @(window_ptr);
	MirrorWindow *mirror = g_windows[key];
	if (!mirror)
		return;

	[mirror.window setDelegate:nil];
	[mirror.window orderOut:nil];
	[mirror.window close];
	[g_windows removeObjectForKey:key];

	printf("[WIN-BRIDGE] retired window ptr=0x%08X\n", (unsigned)window_ptr);
	fflush(stdout);
}

static void bridge_window_present(uint32 window_ptr, const MacWindowBuffer *buf)
{
	if (!g_windows || !buf)
		return;
	MirrorWindow *mirror = g_windows[@(window_ptr)];
	if (!mirror || !mirror.view)
		return; // natively rendered dialog: it has no pixel view to feed

	[mirror.view setPixels:Mac2HostAddr(buf->base_addr)
	              rowBytes:buf->row_bytes
	                 width:buf->width
	                height:buf->height
	             pixelSize:buf->pixel_size];
}

/* =========================================================================
 *  Public entry points
 * ====================================================================== */

void MacWindowBridge_Init(void)
{
	if (!g_windows)
		g_windows = [[NSMutableDictionary alloc] init];

	static ToolboxWindowCallbacks callbacks;
	callbacks.window_added = bridge_window_added;
	callbacks.window_changed = bridge_window_changed;
	callbacks.window_removed = bridge_window_removed;
	callbacks.window_present = bridge_window_present;
	ToolboxWindow_SetCallbacks(&callbacks);
}

void MacWindowBridge_RegisterWindowTraps(void)
{
	MacWindowBridge_Init();
	ToolboxWindow_RegisterTraps();
}
