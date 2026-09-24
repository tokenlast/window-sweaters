#pragma once
#include <stdbool.h>
#include <sys/types.h>

struct border;

// Window Sweaters' floating-window work area. The system's visibleFrame is
// read-only, so this applies the same margin to decorated windows through AX.
void padding_start(bool prompt_for_accessibility);
bool padding_enabled(void);
void padding_set_enabled(bool enabled);
bool padding_enforce(struct border* border, bool allow_resize);
