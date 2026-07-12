# Canonical version constant for the .cpfont binary format.
#
# This is the single source of truth for the build tooling
# (fontconvert_sdcard.py reads from here).
#
# The firmware C++ header (SdCardFont.h) carries its own copy — it must be
# bumped manually when the firmware is updated to support a new version.

# .cpfont binary format version. Bump when the on-disk struct layout changes.
CPFONT_VERSION = 4
