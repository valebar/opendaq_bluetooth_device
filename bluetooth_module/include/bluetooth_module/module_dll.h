#pragma once

#include <opendaq/module_exports.h>

// The macro's OPENDAQ_MODULE_API already expands to `extern "C"` (via EXPORT_C), so an extra
// `extern "C"` prefix here produces a -Wduplicate-decl-specifier warning on every build of a
// consuming project. openDAQ's own modules declare theirs without it.
DECLARE_MODULE_EXPORTS_WITHOUT_ID()
