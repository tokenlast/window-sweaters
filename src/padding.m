#import <Cocoa/Cocoa.h>
#import <ApplicationServices/ApplicationServices.h>
#include "padding.h"
#include "border.h"
#include "misc/extern.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

extern bool g_knit_on;
extern void _AXUIElementGetWindow(CFTypeRef window, uint32_t* wid);
extern void knit_padding_changed(void);

struct padded_screen {
  CGRect full;
  CGRect usable;
};

static struct padded_screen screens[16];
static int screen_count;
static bool g_padding_enabled = true;

static bool padding_trace(void) { return getenv("KNIT_PADDING_TRACE") != NULL; }

static void padding_request_accessibility(void) {
  if (AXIsProcessTrusted()) return;
  NSDictionary* options = @{(__bridge NSString*)kAXTrustedCheckOptionPrompt: @YES};
  AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)options);
}

bool padding_enabled(void) { return g_padding_enabled; }

void padding_set_enabled(bool enabled) {
  g_padding_enabled = enabled;
  [NSUserDefaults.standardUserDefaults setBool:enabled forKey:@"screenPadding"];
  if (enabled) padding_request_accessibility();
}

static CGRect screen_rect(NSRect rect, CGFloat primary_top) {
  return CGRectMake(NSMinX(rect), primary_top - NSMaxY(rect),
                    NSWidth(rect), NSHeight(rect));
}

static void padding_refresh_screens(void) {
  NSArray<NSScreen*>* available = NSScreen.screens;
  screen_count = 0;
  if (available.count == 0) return;
  // AX and SkyLight use a top-left origin; AppKit uses a bottom-left origin.
  // screens[0] is the primary display whose menu bar defines AX's origin.
  CGFloat primary_top = NSMaxY(available.firstObject.frame);
  for (NSScreen* screen in available) {
    if (screen_count == 16) break;
    screens[screen_count++] = (struct padded_screen){
      .full = screen_rect(screen.frame, primary_top),
      .usable = screen_rect(screen.visibleFrame, primary_top),
    };
  }
}

void padding_start(bool prompt_for_accessibility) {
  NSCAssert([NSThread isMainThread], @"Window padding must run on the main thread");
  [NSUserDefaults.standardUserDefaults registerDefaults:@{@"screenPadding": @YES}];
  g_padding_enabled = [NSUserDefaults.standardUserDefaults boolForKey:@"screenPadding"];
  padding_refresh_screens();
  [[NSNotificationCenter defaultCenter]
      addObserverForName:NSApplicationDidChangeScreenParametersNotification
                object:nil queue:NSOperationQueue.mainQueue
            usingBlock:^(__unused NSNotification* note) {
              padding_refresh_screens();
              knit_padding_changed();
            }];
  if (prompt_for_accessibility && g_padding_enabled) padding_request_accessibility();
  if (padding_trace())
    fprintf(stderr, "padding start enabled=%d trusted=%d screens=%d\n",
            g_padding_enabled, AXIsProcessTrusted(), screen_count);
}

static double intersection_area(CGRect a, CGRect b) {
  CGRect overlap = CGRectIntersection(a, b);
  return CGRectIsNull(overlap) ? 0 : overlap.size.width * overlap.size.height;
}

static struct padded_screen* screen_for_window(CGRect window) {
  struct padded_screen* chosen = NULL;
  double best_area = 0, best_distance = INFINITY;
  CGPoint center = CGPointMake(CGRectGetMidX(window), CGRectGetMidY(window));
  for (int i = 0; i < screen_count; i++) {
    double area = intersection_area(window, screens[i].full);
    double dx = center.x - CGRectGetMidX(screens[i].full);
    double dy = center.y - CGRectGetMidY(screens[i].full);
    double distance = dx * dx + dy * dy;
    if (!chosen || area > best_area || (area == best_area && distance < best_distance)) {
      chosen = &screens[i];
      best_area = area;
      best_distance = distance;
    }
  }
  return chosen;
}

static AXUIElementRef ax_window_for_border(struct border* border) {
  if (border->ax_window) return (AXUIElementRef)border->ax_window;
  AXUIElementRef app = AXUIElementCreateApplication(border->owner_pid);
  if (!app) return NULL;
  AXUIElementSetMessagingTimeout(app, 0.05f);
  CFArrayRef windows = NULL;
  AXError error = AXUIElementCopyAttributeValue(app, kAXWindowsAttribute,
                                                (CFTypeRef*)&windows);
  CFRelease(app);
  if (error != kAXErrorSuccess || !windows) {
    if (padding_trace()) fprintf(stderr, "padding AX windows pid=%d error=%d\n",
                                 border->owner_pid, error);
    return NULL;
  }
  AXUIElementRef found = NULL;
  for (CFIndex i = 0; i < CFArrayGetCount(windows); i++) {
    AXUIElementRef candidate = (AXUIElementRef)CFArrayGetValueAtIndex(windows, i);
    if (CFGetTypeID(candidate) != AXUIElementGetTypeID()) continue;
    uint32_t wid = 0;
    _AXUIElementGetWindow(candidate, &wid);
    if (wid == border->target_wid) {
      found = (AXUIElementRef)CFRetain(candidate);
      AXUIElementSetMessagingTimeout(found, 0.05f);
      break;
    }
  }
  CFRelease(windows);
  if (!found && padding_trace())
    fprintf(stderr, "padding AX window not found pid=%d wid=%u\n",
            border->owner_pid, border->target_wid);
  border->ax_window = found;
  return found;
}

static CGFloat clamp(CGFloat value, CGFloat low, CGFloat high) {
  return fmax(low, fmin(high, value));
}

bool padding_enforce(struct border* border, bool allow_resize) {
  NSCAssert([NSThread isMainThread], @"Window padding must run on the main thread");
  if (!border || border->is_proxy || border->external_proxy_wid
      || !g_knit_on || !g_padding_enabled)
    return false;
  struct settings* settings = border_get_settings(border);
  if (settings->border_style != BORDER_STYLE_KNIT || settings->border_width <= 0)
    return false;
  if (screen_count == 0) padding_refresh_screens();

  CGRect actual;
  if (SLSGetWindowBounds(border->cid, border->target_wid, &actual) != kCGErrorSuccess
      || !isfinite(actual.origin.x) || !isfinite(actual.origin.y)
      || !isfinite(actual.size.width) || !isfinite(actual.size.height)
      || actual.size.width <= 0 || actual.size.height <= 0) return false;
  // A move notification can also arrive during a native resize animation.
  // Leave those frames alone and resize once the window has gone quiet.
  if (!allow_resize && border->geometry_valid
      && (fabs(actual.size.width - border->target_bounds.size.width) > 0.5
          || fabs(actual.size.height - border->target_bounds.size.height) > 0.5))
    return false;
  struct padded_screen* screen = screen_for_window(actual);
  if (!screen) return false;

  CGRect safe = CGRectInset(screen->usable, settings->border_width,
                           settings->border_width);
  if (safe.size.width < 100 || safe.size.height < 100) return false;
  CGRect desired = actual;
  if (allow_resize) {
    desired.size.width = fmin(actual.size.width, safe.size.width);
    desired.size.height = fmin(actual.size.height, safe.size.height);
  }
  if (desired.size.width <= safe.size.width)
    desired.origin.x = clamp(actual.origin.x, CGRectGetMinX(safe),
                             CGRectGetMaxX(safe) - desired.size.width);
  if (desired.size.height <= safe.size.height)
    desired.origin.y = clamp(actual.origin.y, CGRectGetMinY(safe),
                             CGRectGetMaxY(safe) - desired.size.height);
  bool resize = fabs(desired.size.width - actual.size.width) > 0.5
                || fabs(desired.size.height - actual.size.height) > 0.5;
  bool move = fabs(desired.origin.x - actual.origin.x) > 0.5
              || fabs(desired.origin.y - actual.origin.y) > 0.5;
  if (!resize && !move) {
    border->padding_attempt_valid = false;
    border->padding_retry_pending = false;
    border->padding_retry_count = 0;
    return false;
  }
  bool live_move = !allow_resize;
  CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
  // AX writes run on the UI thread; never issue more than one per display
  // frame, even when Chrome reports a slightly different position each time.
  if (live_move && border->padding_attempt_valid
      && now - border->padding_last_attempt_at < 0.016) return false;
  if (border->padding_attempt_valid
      && CGRectEqualToRect(border->padding_last_attempt, actual)) {
    // A zoomed or non-movable app can accept an AX write but then restore the
    // same frame. Do not fight it indefinitely. During a drag, retry at most
    // once per display frame so the edge still feels firm.
    if (!live_move && !border->padding_retry_pending) return false;
  }
  if (padding_trace())
    fprintf(stderr, "padding request pid=%d wid=%u %.0f,%.0f %.0fx%.0f -> %.0f,%.0f %.0fx%.0f\n",
            border->owner_pid, border->target_wid,
            actual.origin.x, actual.origin.y, actual.size.width, actual.size.height,
            desired.origin.x, desired.origin.y, desired.size.width, desired.size.height);
  if (!AXIsProcessTrusted()) {
    if (padding_trace()) fprintf(stderr, "padding accessibility not trusted\n");
    return false;
  }

  border->padding_last_attempt = actual;
  border->padding_last_attempt_at = now;
  border->padding_attempt_valid = true;
  AXUIElementRef window = ax_window_for_border(border);
  if (!window) {
    border->padding_retry_pending = true;
    return false;
  }
  CFTypeRef fullscreen = NULL;
  if (AXUIElementCopyAttributeValue(window, CFSTR("AXFullScreen"), &fullscreen)
        == kAXErrorSuccess && fullscreen) {
    bool is_fullscreen = CFGetTypeID(fullscreen) == CFBooleanGetTypeID()
                         && CFBooleanGetValue(fullscreen);
    CFRelease(fullscreen);
    if (is_fullscreen) return false;
  }

  AXError error = kAXErrorSuccess;
  if (resize) {
    CFTypeRef zoomed = NULL;
    if (AXUIElementCopyAttributeValue(window, CFSTR("AXZoomed"), &zoomed)
          == kAXErrorSuccess && zoomed) {
      bool is_zoomed = CFGetTypeID(zoomed) == CFBooleanGetTypeID()
                       && CFBooleanGetValue(zoomed);
      CFRelease(zoomed);
      if (padding_trace()) fprintf(stderr, "padding zoomed wid=%u value=%d\n",
                                   border->target_wid, is_zoomed);
      if (is_zoomed) {
        AXError unzoom = AXUIElementSetAttributeValue(window, CFSTR("AXZoomed"),
                                                     kCFBooleanFalse);
        if (unzoom == kAXErrorSuccess) {
          border->padding_retry_pending = true;
          return true;
        }
      }
    }
    Boolean resizable = false;
    if (AXUIElementIsAttributeSettable(window, kAXSizeAttribute, &resizable)
          != kAXErrorSuccess || !resizable) return false;
    AXValueRef size = AXValueCreate(kAXValueCGSizeType, &desired.size);
    error = AXUIElementSetAttributeValue(window, kAXSizeAttribute, size);
    CFRelease(size);
  }
  if (error == kAXErrorSuccess && move) {
    Boolean movable = false;
    if (AXUIElementIsAttributeSettable(window, kAXPositionAttribute, &movable)
          != kAXErrorSuccess || !movable) return false;
    AXValueRef position = AXValueCreate(kAXValueCGPointType, &desired.origin);
    error = AXUIElementSetAttributeValue(window, kAXPositionAttribute, position);
    CFRelease(position);
  }
  if (error == kAXErrorInvalidUIElement || error == kAXErrorCannotComplete) {
    CFRelease(border->ax_window);
    border->ax_window = NULL;
    border->padding_retry_pending = true;
  } else if (error == kAXErrorSuccess) {
    border->padding_retry_pending = false;
    border->padding_retry_count = 0;
  }
  if (padding_trace()) fprintf(stderr, "padding result wid=%u error=%d\n",
                               border->target_wid, error);
  return error == kAXErrorSuccess;
}
