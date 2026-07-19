#!/usr/bin/env bash
# Sync the dual-screen fork with upstream TwilitRealm/dusklight.
# See docs/dualscreen-fork.md for the conflict cheat-sheet and the aurora
# bump procedure.
set -euo pipefail

cd "$(dirname "$0")/.."

if ! git remote get-url upstream >/dev/null 2>&1; then
  git remote add upstream https://github.com/TwilitRealm/dusklight.git
fi

echo "== Fetching upstream =="
git fetch upstream --tags

BEHIND=$(git rev-list --count HEAD..upstream/main)
LATEST_TAG=$(git describe --tags --abbrev=0 upstream/main 2>/dev/null || echo "?")
echo "Behind upstream/main: ${BEHIND} commit(s) (latest tag: ${LATEST_TAG})"

if [[ "$BEHIND" -eq 0 ]]; then
  echo "Already up to date (nothing to merge)."
  if [[ "${1:-}" != "--merge" ]]; then
    exit 0
  fi
  echo "== Building (desktop) =="
  cmake --build --preset linux-default-relwithdebinfo -j "$(nproc)"
  echo "Build OK."
  exit 0
fi

# Warn before merging if upstream moved the aurora submodule pointer — that
# needs the manual rebase procedure from docs/dualscreen-fork.md.
if ! git diff --quiet HEAD upstream/main -- extern/aurora; then
  NEW_SHA=$(git rev-parse upstream/main:extern/aurora)
  echo ""
  echo "!! Upstream moved extern/aurora to ${NEW_SHA}."
  echo "!! After the merge, rebase the aux-window branch onto it:"
  echo "!!   cd extern/aurora && git fetch upstream && git rebase ${NEW_SHA} aux-window"
  echo "!!   git push -f origin aux-window && cd ../.."
  echo "!!   git add extern/aurora && git commit"
  echo "!! (Keep .gitmodules pointing at igawa6/aurora if the merge touches it.)"
  echo ""
fi

if [[ "${1:-}" != "--merge" ]]; then
  echo ""
  echo "Fetch only — this script does not merge (merging writes a commit)."
  echo "To merge yourself:"
  echo "  git merge upstream/main"
  echo "Conflicts: the touchpoint table in docs/dualscreen-fork.md lists every"
  echo "file the fork modifies and why. Then re-run with --merge to build."
  exit 0
fi

echo "== Merging upstream/main (--merge) =="
if ! git merge upstream/main --no-edit; then
  echo ""
  echo "Merge conflicts — the touchpoint table in docs/dualscreen-fork.md"
  echo "lists every file the fork modifies and why. Resolve, then re-run"
  echo "this script to build and smoke test."
  exit 1
fi

echo "== Building (desktop) =="
cmake --build --preset linux-default-relwithdebinfo -j "$(nproc)"

echo ""
echo "Merge + build OK."
echo "Next steps:"
echo "  1. Smoke test (docs/dualscreen-fork.md, Verification section)"
echo "  2. git push origin dual-screen-v2   # manual — this script never pushes"
echo "  3. <your APK build script>          # gradle assembleRelease + sign"
