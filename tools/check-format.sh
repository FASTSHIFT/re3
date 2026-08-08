#!/usr/bin/env bash
# check-format.sh - run clang-format on staged C/C++ files that belong to us
#
# Usage:
#   ./tools/check-format.sh           # check all staged files (scoped)
#   ./tools/check-format.sh [files]   # check specific files (scoped)
#
# Install as a git pre-commit hook:
#   ln -sf ../../tools/check-format.sh .git/hooks/pre-commit
#
# IMPORTANT: the repo root .clang-format encodes *our* project style. The
# upstream re3 sources were never formatted to it, so running clang-format
# over a file we merely touched (e.g. a one-line warning fix in an upstream
# .cpp) would report the whole file as mis-formatted. To avoid that noise we
# only enforce formatting on code we own:
#     src/skel/gbm/**    drivers/st7789/**    tools/**
# Everything else (upstream re3, vendor/, generated *_gl.inc) is skipped.

set -euo pipefail

# Paths we own and want kept clang-format clean.
OWNED_REGEX='^(src/skel/gbm/|drivers/st7789/|tools/)'

if [ $# -gt 0 ]; then
    FILES=$(printf '%s\n' "$@" | grep -E '\.(cpp|c|h)$' | grep -E "$OWNED_REGEX" || true)
else
    FILES=$(git diff --cached --name-only --diff-filter=ACM \
            | grep -E '\.(cpp|c|h)$' \
            | grep -E "$OWNED_REGEX" \
            || true)
fi

if [ -z "$FILES" ]; then
    echo "[format] No owned C/C++ files to check."
    exit 0
fi

if ! command -v clang-format &>/dev/null; then
    echo "[format] clang-format not found, skipping check."
    exit 0
fi

echo "[format] Checking: $FILES"
FAILED=0
for f in $FILES; do
    if ! clang-format --dry-run --Werror "$f" 2>/dev/null; then
        echo "[format] FAIL: $f"
        FAILED=1
    fi
done

if [ $FAILED -ne 0 ]; then
    echo ""
    echo "[format] Run:  clang-format -i <file>  to fix violations."
    exit 1
fi

echo "[format] All files OK."
