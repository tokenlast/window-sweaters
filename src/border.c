#include "border.h"
#include "misc/autoyarn.h"
#include "misc/overrides.h"
// A weak default so the test targets, which compile this file without the
// ObjC module, still link. src/autoyarn.m provides the real implementation
// and overrides this at link time in the app build.
__attribute__((weak))
bool knit_auto_yarn(const char* app, pid_t pid, uint32_t* yarn, int* chart) {
  (void)app; (void)pid; (void)yarn; (void)chart; return false;
}
__attribute__((weak))
bool knit_zigzag_active(void) { return false; }
__attribute__((weak))
bool knit_zigzag_yarn(const char* app, pid_t pid, uint32_t* yarn, int* chart) {
  (void)app; (void)pid; (void)yarn; (void)chart; return false;
}
// The menu module supplies saved overrides in the app build. Native renderer
// probes link without AppKit and keep the collection's ordinary behaviour.
__attribute__((weak))
unsigned knit_app_override(pid_t pid, uint32_t* yarn, int* chart) {
  (void)pid; (void)yarn; (void)chart; return 0;
}
__attribute__((weak))
void knit_auto_recolor(const char* app, pid_t pid, uint32_t yarn,
                       int* chart, bool chart_overridden) {
  (void)app; (void)pid; (void)yarn; (void)chart; (void)chart_overridden;
}
#include "misc/apps.h"
#include "misc/chart.h"
#include <math.h>
#include "hashtable.h"
#include "misc/extern.h"
#include "windows.h"
#include <pthread.h>
#include <time.h>

extern struct settings g_settings;

static uint32_t border_surface_id(const struct border* border, int index) {
  return index ? border->extra_segments[index - 1].wid : border->wid;
}

static CGContextRef border_surface_context(const struct border* border, int index) {
  return index ? border->extra_segments[index - 1].context : border->context;
}

static int border_surface_count(const struct border* border) {
  return border->segmented_knit ? 4 : 1;
}

static bool border_surfaces_complete(const struct border* border) {
  for (int i = 0; i < border_surface_count(border); i++) {
    CGRect rect = border->segmented_knit ? border->segment_rects[i] : border->frame;
    if (rect.size.width > 0 && rect.size.height > 0
        && (!border_surface_id(border, i) || !border_surface_context(border, i)))
      return false;
  }
  return true;
}

static CGPoint border_surface_origin(const struct border* border, CGPoint origin, int index) {
  if (!border->segmented_knit) return origin;
  CGRect rect = border->segment_rects[index];
  return (CGPoint){origin.x + rect.origin.x,
                   origin.y + border->frame.size.height - CGRectGetMaxY(rect)};
}

static void border_split_knit(struct border* border, CGRect frame, float band) {
  // At a rounded corner the ring can reach farther inward than its straight
  // edge. Include that diagonal reach and one antialiasing point in each strip.
  float radius = fmaxf(0.f, fminf(border->radius,
                       fminf(frame.size.width, frame.size.height) * .5f));
  float inner_radius = fmaxf(0.f, radius - band - 1.f);
  float reach = ceilf(band + 2.f + inner_radius * (1.f - (float)M_SQRT1_2));
  float depth = fminf(reach, fminf(frame.size.width, frame.size.height) * .5f);
  float middle = frame.size.height - 2.f * depth;
  border->segment_rects[0] = CGRectMake(0, frame.size.height - depth, frame.size.width, depth);
  border->segment_rects[1] = CGRectMake(0, 0, frame.size.width, depth);
  border->segment_rects[2] = CGRectMake(0, depth, depth, middle);
  border->segment_rects[3] = CGRectMake(frame.size.width - depth, depth, depth, middle);
}

// The knit now lives within the target's rectangle. It must be ordered above
// that window to remain visible; the legacy outline styles retain their order.
static int border_display_order(const struct settings* settings) {
  return settings->border_style == BORDER_STYLE_KNIT
         ? BORDER_ORDER_ABOVE : settings->border_order;
}

struct settings* border_get_settings(struct border* border) {
  assert(pthread_main_np() != 0);
  return border->setting_override.enabled
         ? &border->setting_override
         : &g_settings;
}

static void border_destroy_window(struct border* border) {
  for (int i = 0; i < 3; i++) {
    if (border->extra_segments[i].context) CGContextRelease(border->extra_segments[i].context);
    if (border->extra_segments[i].wid) SLSReleaseWindow(border->cid, border->extra_segments[i].wid);
    border->extra_segments[i].wid = 0;
    border->extra_segments[i].context = NULL;
  }
  if (border->context) CGContextRelease(border->context);
  if (border->wid) SLSReleaseWindow(border->cid, border->wid);
  border->wid = 0;
  border->context = NULL;
  border->segmented_knit = false;
  border->segment_band = 0;
  border->segment_radius = 0;
}

// Last-resort recovery for an onscreen target whose overlay stays absent.
// Recreate only our surface; preserve the target, app settings and Space cache.
void border_reset_surface(struct border* border) {
  assert(pthread_main_np());
  if (border->is_proxy || border->external_proxy_wid) return;
  pthread_mutex_lock(&border->mutex);
  border_destroy_window(border);
  border->visible = false;
  border->geometry_valid = false;
  border->metadata_dirty = true;
  border->needs_redraw = true;
  // A newly created WindowServer window starts at full opacity.
  border->opacity = 1;
  pthread_mutex_unlock(&border->mutex);
}

static bool border_check_too_small(struct border* border, CGRect window_frame) {
  CGRect smallest_rect = CGRectInset(window_frame, 1.0, 1.0);
  if (smallest_rect.size.width < 2.f * border->inner_radius
      || smallest_rect.size.height < 2.f * border->inner_radius) {
    return true;
  }
  return false;
}

static bool border_calculate_bounds(struct border* border, CGRect* frame, struct settings* settings,
                                    const CGRect* observed_bounds) {
  CGRect window_frame = CGRectZero;
  if (observed_bounds) window_frame = *observed_bounds;
  else if (border->is_proxy) window_frame = border->target_bounds;
  else if (SLSGetWindowBounds(border->cid, border->target_wid, &window_frame)
           != kCGErrorSuccess) {
    border_hide(border);
    return false;
  }
  if (!isfinite(window_frame.origin.x) || !isfinite(window_frame.origin.y)
      || !isfinite(window_frame.size.width) || !isfinite(window_frame.size.height)
      || window_frame.size.width <= 0 || window_frame.size.height <= 0) {
    border_hide(border);
    return false;
  }

  border->target_bounds = window_frame;
  border->too_small = border_check_too_small(border, window_frame);
  if (settings->border_style == BORDER_STYLE_KNIT
      && (window_frame.size.width <= 2.f * settings->border_width + 2.f
          || window_frame.size.height <= 2.f * settings->border_width + 2.f))
    border->too_small = true;
  if (border->too_small) {
    border_hide(border);
    return false;
  }

  if (settings->border_style == BORDER_STYLE_KNIT) {
    // Keep the whole overlay in the native window's bounds. macOS already
    // stops that window at the menu bar and display edges when it is dragged.
    // The ring is drawn inward from this rect, so none of its pixels extend
    // into the menu bar or over adjacent desktop content.
    *frame = window_frame;
    border->origin = frame->origin;
    frame->origin = CGPointZero;
    border->drawing_bounds = *frame;
  } else {
    float border_offset = -settings->border_width - BORDER_PADDING;
    *frame = CGRectInset(window_frame, border_offset, border_offset);
    border->origin = frame->origin;
    frame->origin = CGPointZero;
    window_frame.origin = (CGPoint){ -border_offset, -border_offset };
    border->drawing_bounds = window_frame;
  }

  return true;
}

static void border_draw(struct border* border, CGRect frame, struct settings* settings) {
  border->needs_redraw = false;

  if (settings->border_style == BORDER_STYLE_KNIT) {
    uint32_t yarn = 0;
    int chart = 0;
    if (g_knit_on) {
      // A per-app rule wins over the colour this window would otherwise be
      // handed, and may carry its own pattern.
      yarn = knit_color_for_app(border->app);
      chart = knit_pattern_for_app(border->app);
      const struct app_rule* rule = knit_app_rule(border->app);
      if (rule) yarn = rule->color;
      if (knit_zigzag_active()) {
        // One shared pattern, every app in its icon's colour, the 37
        // hand-designed sweaters included; their own colours stay in By App.
        knit_zigzag_yarn(border->app, border->owner_pid, &yarn, &chart);
      } else if (!rule) {
        // No hand-picked sweater: borrow the app's own colour from its icon
        // rather than hashing its name into an arbitrary one.
        uint32_t auto_yarn; int auto_chart;
        if (knit_auto_yarn(border->app, border->owner_pid, &auto_yarn, &auto_chart)) {
          yarn = auto_yarn;
          if (auto_chart >= 0) chart = auto_chart;
        }
      }

      unsigned overrides = knit_app_override(border->owner_pid, &yarn, &chart);
      if (overrides & KNIT_OVERRIDE_COLOR)
        knit_auto_recolor(border->app, border->owner_pid, yarn, &chart,
                          overrides & KNIT_OVERRIDE_CHART);
    }
    // Inset the input so the outer edge follows the actual window. Pass the
    // native outer radius: a 12 pt band around a 9 pt corner still needs a
    // 9 pt outer arc, not a 12 pt one with transparent corner gaps.
    CGRect inner = CGRectInset(border->drawing_bounds,
                               settings->border_width, settings->border_width);
    for (int i = 0; i < border_surface_count(border); i++) {
      uint32_t wid = border_surface_id(border, i);
      CGContextRef context = border_surface_context(border, i);
      if (!wid || !context) continue;
      CGRect segment = border->segmented_knit ? border->segment_rects[i] : frame;
      CGRect local = CGRectMake(0, 0, segment.size.width, segment.size.height);
      CGContextSaveGState(context);
      CGContextClearRect(context, local);
      if (g_knit_on) {
        CGContextClipToRect(context, local);
        CGContextTranslateCTM(context, -segment.origin.x, -segment.origin.y);
        knit_draw_inside(context, inner, border->radius, settings->border_width,
                         yarn, chart, border->focused ? 0.f : g_knit_dim);
      }
      CGContextFlush(context);
      CGContextRestoreGState(context);
      SLSFlushWindowContentRegion(border->cid, wid, NULL);
      SLSWindowThaw(border->cid, wid);
    }
    return;
  }

  CGContextSaveGState(border->context);

  struct color_style color_style = border->focused
                                   ? settings->active_window
                                   : settings->inactive_window;

  CGGradientRef gradient = NULL;
  CGPoint gradient_dir[2];
  if (color_style.stype == COLOR_STYLE_SOLID
     || color_style.stype == COLOR_STYLE_GLOW) {
    bool glow = color_style.stype == COLOR_STYLE_GLOW;
    drawing_set_stroke_and_fill(border->context, color_style.color, glow);
  } else if (color_style.stype == COLOR_STYLE_GRADIENT) {
    CGAffineTransform trans = CGAffineTransformMakeScale(frame.size.width,
                                                         frame.size.height);
    gradient = drawing_create_gradient(&color_style.gradient,
                                       trans,
                                       gradient_dir          );
  }

  CGContextSetLineWidth(border->context, settings->border_width);
  CGContextClearRect(border->context, frame);

  CGRect path_rect = border->drawing_bounds;
  CGMutablePathRef inner_clip_path = CGPathCreateMutable();
  if (settings->border_style == BORDER_STYLE_SQUARE
      && settings->border_order == BORDER_ORDER_ABOVE
      && settings->border_width >= BORDER_TSMW) {
    // Inset the frame to overlap the rounding of macOS windows to create a
    // truly square border
    path_rect = CGRectInset(border->drawing_bounds,
                            BORDER_TSMN,
                            BORDER_TSMN            );

    CGPathAddRect(inner_clip_path, NULL, path_rect);
  } else {
    CGPathAddRoundedRect(inner_clip_path,
                         NULL,
                         CGRectInset(path_rect, 1.0, 1.0),
                         border->inner_radius,
                         border->inner_radius             );
  }
  drawing_clip_between_rect_and_path(border->context, frame, inner_clip_path);

  if (settings->border_style == BORDER_STYLE_SQUARE) {
    if (color_style.stype == COLOR_STYLE_SOLID
       || color_style.stype == COLOR_STYLE_GLOW) {
      drawing_draw_square_with_inset(border->context,
                                     path_rect,
                                     -settings->border_width / 2.f);
    }
    else if (color_style.stype == COLOR_STYLE_GRADIENT) {
      drawing_draw_square_gradient_with_inset(border->context,
                                              gradient,
                                              gradient_dir,
                                              path_rect,
                                              -settings->border_width / 2.f);
    }
  } else {
    float corner_radius = settings->border_style == BORDER_STYLE_ROUND_UNIFORM ? 9.0 : border->radius;

    if (settings->border_style == BORDER_STYLE_ROUND_UNIFORM) {
      drawing_draw_rounded_rect_with_inset(border->context,
                                           path_rect,
                                           corner_radius,
                                           true            );
    }

    if (color_style.stype == COLOR_STYLE_SOLID
       || color_style.stype == COLOR_STYLE_GLOW) {
      drawing_draw_rounded_rect_with_inset(border->context,
                                           path_rect,
                                           corner_radius,
                                           false           );
    } else if (color_style.stype == COLOR_STYLE_GRADIENT) {
      drawing_draw_rounded_gradient_with_inset(border->context,
                                               gradient,
                                               gradient_dir,
                                               path_rect,
                                               corner_radius  );
    }
  }
  CGGradientRelease(gradient);

  if (settings->show_background && settings->border_order != 1) {
    CGContextRestoreGState(border->context);
    CGContextSaveGState(border->context);
    color_style = settings->background;
    if (color_style.stype == COLOR_STYLE_SOLID
       || color_style.stype == COLOR_STYLE_GLOW) {
      drawing_draw_filled_path(border->context,
                               inner_clip_path,
                               color_style.color);
    }
  }
  CFRelease(inner_clip_path);
  CGContextFlush(border->context);
  CGContextRestoreGState(border->context);
  SLSFlushWindowContentRegion(border->cid, border->wid, NULL);
  SLSWindowThaw(border->cid, border->wid);
}

void border_create_window(struct border* border, CGRect frame, bool unmanaged, bool hidpi) {
  pthread_mutex_lock(&border->mutex);
  int cid = border->cid;
  border->frame = frame;
  border->needs_redraw = true;
  struct settings* settings = border_get_settings(border);
  border->segmented_knit = settings->border_style == BORDER_STYLE_KNIT && !unmanaged;
  border->segment_band = settings->border_width;
  border->segment_radius = border->radius;
  if (border->segmented_knit) border_split_knit(border, frame, settings->border_width);
  else border->segment_rects[0] = frame;

  bool complete = true;
  for (int i = 0; i < border_surface_count(border); i++) {
    CGRect segment = border->segment_rects[i];
    if (segment.size.width <= 0 || segment.size.height <= 0) continue;
    CGRect local = CGRectMake(0, 0, segment.size.width, segment.size.height);
    uint32_t wid = window_create(cid, local, hidpi, unmanaged);
    CGContextRef context = wid ? SLWindowContextCreate(cid, wid, NULL) : NULL;
    if (!wid || !context) {
      if (context) CGContextRelease(context);
      if (wid) SLSReleaseWindow(cid, wid);
      complete = false;
      break;
    }
    CGContextSetInterpolationQuality(context, kCGInterpolationNone);
    if (i == 0) { border->wid = wid; border->context = context; }
    else { border->extra_segments[i - 1].wid = wid;
           border->extra_segments[i - 1].context = context; }
  }
  if (!border->sid) border->sid = window_space_id(cid, border->target_wid);
  if (complete && border->wid) {
    border->opacity = 1;
    for (int i = 0; i < border_surface_count(border); i++) {
      uint32_t wid = border_surface_id(border, i);
      if (wid) window_send_to_space(cid, wid, border->sid);
    }
  } else border_destroy_window(border);
  pthread_mutex_unlock(&border->mutex);
}

// A display handoff need not emit a Space-change notification. Verify actual
// membership as well as the cache so an incomplete transfer is retried.
// Called by the bounded snapshot repair, never for every mouse-move event.
void border_refresh_space(struct border* border) {
  if (border->is_proxy || border->external_proxy_wid || border->sticky) return;
  pthread_mutex_lock(&border->mutex);
  uint64_t sid = window_space_id(border->cid, border->target_wid);
  if (sid) {
    bool mismatch = sid != border->sid;
    for (int i = 0; i < border_surface_count(border); i++) {
      uint32_t wid = border_surface_id(border, i);
      if (wid && window_space_id(border->cid, wid) != sid) mismatch = true;
    }
    if (mismatch) {
      for (int i = 0; i < border_surface_count(border); i++) {
        uint32_t wid = border_surface_id(border, i);
        if (wid) window_send_to_space(border->cid, wid, sid);
      }
      border->sid = sid;
      border->metadata_dirty = true;
      border->geometry_valid = false;
      border->needs_redraw = true; // restore geometry/order after the transfer
    }
  }
  pthread_mutex_unlock(&border->mutex);
}

void border_update_internal(struct border* border, struct settings* settings, const CGRect* observed_bounds) {
  if (border->external_proxy_wid || border->resize_suppressed) return;
  border->geometry_valid = false;

  int cid = border->cid;
  CGRect frame;
  if (!border_calculate_bounds(border, &frame, settings, observed_bounds)) return;

  // Geometry events are frequent; stacking and space metadata change only on
  // their own notifications. Avoid these synchronous server queries per stitch
  // redraw while dragging a resize handle.
  if (border->metadata_dirty) {
    uint64_t tags = window_tags(cid, border->target_wid);
    border->sticky = tags & WINDOW_TAG_STICKY;
    border->level = window_level(cid, border->target_wid);
    border->sub_level = window_sub_level(cid, border->target_wid);
    uint64_t sid = window_space_id(cid, border->target_wid);
    if (sid && sid != border->sid) {
      border->sid = sid;
      for (int i = 0; i < border_surface_count(border); i++) {
        uint32_t wid = border_surface_id(border, i);
        if (wid) window_send_to_space(cid, wid, sid);
      }
    }
    border->metadata_dirty = false;
  }
  if (!border->sticky && !is_space_visible(cid, border->sid)) return;


  bool shown = false;
  SLSWindowIsOrderedIn(cid, border->target_wid, &shown);
  if (!shown && !border->is_proxy) {
    border_hide(border);
    return;
  } 

  if (!border->wid) {
    border_create_window(border,
                         frame,
                         border->is_proxy,
                         settings->hidpi  );
  }
  if (!border->wid || !border->context) return;
  bool wants_segments = settings->border_style == BORDER_STYLE_KNIT && !border->is_proxy;
  if (wants_segments != border->segmented_knit
      || !border_surfaces_complete(border)
      || (wants_segments && (!CGSizeEqualToSize(frame.size, border->frame.size)
          || border->segment_band != settings->border_width
          || border->segment_radius != border->radius))) {
    // A live resize already hid the knit. Recreate its small strips once at
    // the settled size, rather than retaining four oversized backing stores.
    border_destroy_window(border);
    border_create_window(border, frame, border->is_proxy, settings->hidpi);
    if (!border->wid || !border->context) return;
  }
  if (!CGRectEqualToRect(frame, border->frame)) border->needs_redraw = true;

  // Acquire this before disabling updates: every failure path below must
  // release the server's update lock and thaw a reshaped window.
  CFTypeRef transaction = SLSTransactionCreate(cid);
  if (!transaction) return;

  bool disabled_update = false;
  if (!CGRectEqualToRect(frame, border->frame)) {
    CFTypeRef frame_region = NULL;
    CGSNewRegionWithRect(&frame, &frame_region);
    if (!frame_region) {
      CFRelease(transaction);
      return;
    }
    disabled_update = true;
    SLSDisableUpdate(cid);
    SLSWindowFreezeWithOptions(border->cid, border->wid, NULL);
    CGError shape_error = SLSSetWindowShape(border->cid, border->wid,
                                           border->origin.x, border->origin.y,
                                           frame_region);
    CFRelease(frame_region);
    if (shape_error != kCGErrorSuccess) {
      SLSWindowThaw(cid, border->wid);
      SLSReenableUpdate(cid);
      CFRelease(transaction);
      return;
    }

    // The drawing context is created once, against the window's backing store
    // as it was at creation time. Reshaping the window does not grow it, so
    // after a window grows, the newly exposed area cannot be painted — and
    // since a CGContext is bottom-left origin, that area is the TOP of the
    // window on screen. Left alone this shows as the knit detaching from the
    // top edge whenever a window is resized larger. Rebind it to the reshaped
    // window before drawing.
    CGContextRef resized_context = SLWindowContextCreate(cid, border->wid, NULL);
    if (!resized_context) {
      SLSWindowThaw(cid, border->wid);
      SLSReenableUpdate(cid);
      CFRelease(transaction);
      return;
    }
    CGContextRelease(border->context);
    border->context = resized_context;
    CGContextSetInterpolationQuality(border->context, kCGInterpolationNone);

    border->needs_redraw = true;
    border->frame = frame;
  }

  if (border->needs_redraw) border_draw(border, frame, settings);

  for (int i = 0; i < border_surface_count(border); i++) {
    uint32_t wid = border_surface_id(border, i);
    if (!wid) continue;
    CGPoint origin = border_surface_origin(border, border->origin, i);
    SLSTransactionMoveWindowWithGroup(transaction, wid, origin);

    if (!border->is_proxy) {
      CGAffineTransform transform = CGAffineTransformIdentity;
      transform.tx = -origin.x;
      transform.ty = -origin.y;
      SLSTransactionSetWindowTransform(transaction, wid, 0, 0, transform);
    }
    SLSTransactionSetWindowLevel(transaction, wid, border->level);
    SLSTransactionSetWindowSubLevel(transaction, wid, border->sub_level);
    SLSTransactionOrderWindow(transaction, wid, border_display_order(settings),
                              border->target_wid);
  }
  SLSTransactionCommit(transaction, 0);
  CFRelease(transaction);

  uint64_t set_tags = (1ULL << 1) | (1ULL << 9);
  uint64_t clear_tags = 0;

  if (border->sticky) {
    set_tags |= WINDOW_TAG_STICKY;
    clear_tags |= (1ULL << 45);
  } else {
    clear_tags |= WINDOW_TAG_STICKY;
  }

  for (int i = 0; i < border_surface_count(border); i++) {
    uint32_t wid = border_surface_id(border, i);
    if (!wid) continue;
    SLSSetWindowTags(cid, wid, &set_tags, 0x40);
    SLSClearWindowTags(cid, wid, &clear_tags, 0x40);
  }

  if (disabled_update) SLSReenableUpdate(cid);
  border->geometry_valid = true;
  border->visible = true;
}

void border_init(struct border* border, int cid) {
  memset(border, 0, sizeof(struct border));
  pthread_mutexattr_t mattr;
  pthread_mutexattr_init(&mattr);
  pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&border->mutex, &mattr);
  pthread_mutexattr_destroy(&mattr);
  border->metadata_dirty = true;
  border->opacity = 1;
  animation_init(&border->animation);
  if (cid) border->cid = cid;
  else border->cid = SLSMainConnectionID();
}

struct border* border_create() {
  struct border* border = malloc(sizeof(struct border));
  int cid = 0;
  SLSNewConnection(0, &cid);
  border_init(border, cid);
  return border;
}

void border_destroy(struct border* border) {
  border_hide(border);
  dispatch_async(dispatch_get_main_queue(), ^{
    pthread_mutex_lock(&border->mutex);
    border_destroy_window(border);
    if (border->proxy) border_destroy(border->proxy);
    animation_stop(&border->animation);
    if (!border->is_proxy && border->cid != SLSMainConnectionID())
      SLSReleaseConnection(border->cid);
    pthread_mutex_unlock(&border->mutex);
    pthread_mutex_destroy(&border->mutex);
    free(border);
  });
}

bool border_suppress_live_resize(struct border* border, CGRect bounds) {
  assert(pthread_main_np());
  if (!g_knit_on || border->is_proxy || border->external_proxy_wid) return false;
  if (!border->resize_suppressed && (!border->visible
      || CGSizeEqualToSize(bounds.size, border->drawing_bounds.size))) return false;
  bool held = CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState, kCGMouseButtonLeft);
  CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
  if (!border->resize_suppressed || held
      || !CGSizeEqualToSize(bounds.size, border->resize_observed_size)) {
    border->resize_observed_size = bounds.size;
    border->resize_settle_after = now + .06;
  }
  if (!held && now >= border->resize_settle_after) {
    border->resize_suppressed = false;
    return false;
  }
  border->resize_suppressed = true;
  if (border->visible) border_hide(border);
  return true;
}

static void border_apply_geometry(struct border* border, CGRect window_frame) {
  if (border_suppress_live_resize(border, window_frame)) return;
  struct settings* settings = border_get_settings(border);
  pthread_mutex_lock(&border->mutex);
  if (border->external_proxy_wid) {
    pthread_mutex_unlock(&border->mutex);
    return;
  }
  // Keep movement in the same main-queue order as resize, hide and destroy.
  // A queued worker could otherwise move a hidden border or access it after
  // close, and bursts of workers add visible trailing latency to a drag.
  // AppKit may issue a move before the matching size event when resizing
  // from the top or left. Reconcile both dimensions from the same geometry
  // path instead of moving the old-size sweater to the new origin.
  if (!border->geometry_valid || !border_surfaces_complete(border)
      || border->needs_redraw || border->too_small || border->metadata_dirty
      || !isfinite(window_frame.origin.x) || !isfinite(window_frame.origin.y)
      || !CGSizeEqualToSize(window_frame.size, border->drawing_bounds.size)) {
    border_update_internal(border, settings, &window_frame);
    pthread_mutex_unlock(&border->mutex);
    return;
  }
  // MOVE and RESIZE can describe the same change. Keep reading fresh bounds,
  // but do not repeat server queries or submit another transaction for it.
  if (CGRectEqualToRect(window_frame, border->target_bounds)) {
    pthread_mutex_unlock(&border->mutex);
    return;
  }
  CGPoint origin = settings->border_style == BORDER_STYLE_KNIT
                    ? window_frame.origin
                    : (CGPoint){ .x = window_frame.origin.x - settings->border_width - BORDER_PADDING,
                                 .y = window_frame.origin.y - settings->border_width - BORDER_PADDING };

  CFTypeRef transaction = SLSTransactionCreate(border->cid);
  if (transaction) {
    for (int i = 0; i < border_surface_count(border); i++) {
      uint32_t wid = border_surface_id(border, i);
      if (!wid) continue;
      SLSTransactionMoveWindowWithGroup(transaction, wid,
                                        border_surface_origin(border, origin, i));
    }

    SLSTransactionCommit(transaction, 0);
    CFRelease(transaction);
    border->target_bounds = window_frame;
    border->origin = origin;
  }
  pthread_mutex_unlock(&border->mutex);
}

static bool border_observe_window(struct border* border, CGRect* bounds, double* opacity) {
  *opacity = border->opacity;
  if (border->native_transform) return false;
  if (border->is_proxy) { *bounds = border->target_bounds; return true; }
  // Keep synchronous dictionary creation out of MOVE/RESIZE callbacks. The
  // recovery snapshot independently verifies actual visibility and opacity.
  return SLSGetWindowBounds(border->cid, border->target_wid, bounds) == kCGErrorSuccess;
}

static void border_apply_opacity(struct border* border, double opacity) {
  if (!isfinite(opacity)) return;
  opacity = fmax(0, fmin(1, opacity));
  if (border->wid && fabs(border->opacity - opacity) > .001) {
    bool complete = true;
    for (int i = 0; i < border_surface_count(border); i++) {
      uint32_t wid = border_surface_id(border, i);
      if (wid && SLSSetWindowAlpha(border->cid, wid, opacity) != kCGErrorSuccess)
        complete = false;
    }
    if (complete) border->opacity = opacity;
  }
}

void border_update_geometry(struct border* border) {
  CGRect bounds;
  double opacity;
  if (!border_observe_window(border, &bounds, &opacity)) {
    border_hide(border);
    return;
  }
  border_update_geometry_from_snapshot(border, bounds, opacity);
}

void border_update_geometry_from_snapshot(struct border* border, CGRect bounds, double opacity) {
  border_apply_geometry(border, bounds);
  border_apply_opacity(border, opacity);
}

void border_update(struct border* border, bool try_async) {
  (void)try_async; // Geometry and cached drawing state are main-queue owned.
  pthread_mutex_lock(&border->mutex);
  struct settings* settings = border_get_settings(border);
  CGRect bounds;
  double opacity;
  if (border_observe_window(border, &bounds, &opacity)
      && !border_suppress_live_resize(border, bounds)) {
    border_update_internal(border, settings, &bounds);
    border_apply_opacity(border, opacity);
  } else border_hide(border);
  pthread_mutex_unlock(&border->mutex);
}

// An app can raise a whole group of windows without changing its focused
// window. Re-read stacking after that operation has settled; do not resize,
// redraw, or unhide an overlay as a side effect of repairing its depth.
void border_reorder(struct border* border) {
  struct settings* settings = border_get_settings(border);
  pthread_mutex_lock(&border->mutex);
  if (border->resize_suppressed || !border->wid || !border->context || border->too_small
      || border->is_proxy || border->external_proxy_wid
      || (!border->sticky && !is_space_visible(border->cid, border->sid))) {
    pthread_mutex_unlock(&border->mutex);
    return;
  }
  bool target_shown = false, border_shown = false;
  if (SLSWindowIsOrderedIn(border->cid, border->target_wid, &target_shown) != kCGErrorSuccess
      || !target_shown) {
    border_hide(border);
    pthread_mutex_unlock(&border->mutex);
    return;
  }
  if (SLSWindowIsOrderedIn(border->cid, border->wid, &border_shown) != kCGErrorSuccess
      || !border_shown) {
    pthread_mutex_unlock(&border->mutex);
    return;
  }
  int level = window_level(border->cid, border->target_wid);
  int sub_level = window_sub_level(border->cid, border->target_wid);
  CFTypeRef transaction = SLSTransactionCreate(border->cid);
  if (transaction) {
    for (int i = 0; i < border_surface_count(border); i++) {
      uint32_t wid = border_surface_id(border, i);
      if (!wid) continue;
      SLSTransactionSetWindowLevel(transaction, wid, level);
      SLSTransactionSetWindowSubLevel(transaction, wid, sub_level);
      SLSTransactionOrderWindow(transaction, wid, border_display_order(settings),
                               border->target_wid);
    }
    SLSTransactionCommit(transaction, 0);
    // Match the existing transaction paths: the private commit return is not
    // a reliable acknowledgement. Cache the target metadata we actually read.
    border->level = level;
    border->sub_level = sub_level;
    CFRelease(transaction);
  }
  pthread_mutex_unlock(&border->mutex);
}

void border_hide(struct border* border) {
  pthread_mutex_lock(&border->mutex);
  border->geometry_valid = false;
  border->visible = false;
  if (border->wid) {
    CFTypeRef transaction = SLSTransactionCreate(border->cid);
    if (transaction) {
      for (int i = 0; i < border_surface_count(border); i++) {
        uint32_t wid = border_surface_id(border, i);
        if (wid) SLSTransactionOrderWindow(transaction, wid, 0, border->target_wid);
      }
      SLSTransactionCommit(transaction, 0);
      CFRelease(transaction);
    }
  }
  pthread_mutex_unlock(&border->mutex);
}

void border_unhide(struct border* border) {
  pthread_mutex_lock(&border->mutex);
  if (border->resize_suppressed || border->native_transform || border->too_small
      || border->external_proxy_wid
      || (!border->sticky && !is_space_visible(border->cid, border->sid))) {
    pthread_mutex_unlock(&border->mutex);
    return;
  }

  if (border->wid) {
    struct settings* settings = border_get_settings(border);
    CFTypeRef transaction = SLSTransactionCreate(border->cid);
    if (transaction) {
      for (int i = 0; i < border_surface_count(border); i++) {
        uint32_t wid = border_surface_id(border, i);
        if (wid) SLSTransactionOrderWindow(transaction, wid,
                         border_display_order(settings), border->target_wid);
      }
      SLSTransactionCommit(transaction, 0);
      CFRelease(transaction);
      border->visible = true;
    }
  }
  pthread_mutex_unlock(&border->mutex);
}
