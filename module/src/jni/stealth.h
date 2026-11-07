#ifndef ZYGISKFRIDA_STEALTH_H
#define ZYGISKFRIDA_STEALTH_H

#include "config.h"

// Post-injection hardening, executed inside the target process once the
// requested libraries have been loaded:
//
//   * remap   - replaces every file-backed mapping of the injected library
//               with a private anonymous copy, so /proc/self/maps no longer
//               exposes the .so path (and the file can be unlinked).
//   * threads - renames Frida's worker threads (gum-js-loop, gmain, gdbus,
//               ...) so /proc/self/task/*/comm looks like a normal ART app.
//   * unlink  - deletes the localized payload from the app sandbox.
void stealth_apply(const struct target_config *cfg, const char *app_data_dir);

#endif  // ZYGISKFRIDA_STEALTH_H
