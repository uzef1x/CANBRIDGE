#pragma once
// Firmware version, semver (vMAJOR.MINOR.PATCH). Bump by hand when releasing:
// MINOR +1 for new features, PATCH +1 for bugfix-only, MAJOR for breaking
// changes. Must match the git tag pushed for the release (the tag push
// triggers the CI image build). Shown in the web dashboard and the boot log.
#define FW_VERSION "v1.5.1"
