#pragma once
// FW_VERSION is normally injected by tools/git_version.py (git describe).
// Fallback covers builds outside a git checkout (e.g. a bare source export).
#ifndef FW_VERSION
#define FW_VERSION "unknown"
#endif
