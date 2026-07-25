# Injects -DFW_VERSION="<git describe>" at build time.
#
# CI builds are triggered by pushing a release tag, so `git describe --tags`
# resolves to exactly that tag (e.g. "v1.3.0"); local/dev builds get
# "<last-tag>-<n>-g<hash>[-dirty]" or a bare hash before any tag exists.
# No manual version bumping — the tag that names the image names the firmware.
import subprocess

Import("env")

try:
    version = subprocess.check_output(
        ["git", "describe", "--tags", "--always", "--dirty"],
        cwd=env.subst("$PROJECT_DIR"),
        text=True,
    ).strip()
except Exception:
    version = "unknown"

env.Append(CPPDEFINES=[("FW_VERSION", env.StringifyMacro(version))])
print("FW_VERSION = %s" % version)
