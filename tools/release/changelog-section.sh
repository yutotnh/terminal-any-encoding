#!/bin/sh
# Prints the body of CHANGELOG.md's "## [<version>]" section (the lines up
# to the next "## [" heading, without leading/trailing blank lines). Fails
# if there's no such section or it's empty, so a release can't go out
# without notes.
#
# Usage: tools/release/changelog-section.sh <version> [CHANGELOG.md]
set -eu

version="$1"
changelog="${2:-CHANGELOG.md}"

notes="$(awk -v heading="## [${version}]" '
  index($0, heading) == 1 { found = 1; next }
  found && /^## \[/ { exit }
  found { print }
' "$changelog" | sed -e '/./,$!d')"  # $(...) drops the trailing blank lines

if [ -z "$notes" ]; then
  echo "${changelog} has no notes under \"## [${version}]\"" >&2
  exit 1
fi
printf '%s\n' "$notes"
