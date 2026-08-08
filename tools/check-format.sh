#!/usr/bin/env bash
# check-format.sh – run clang-format on staged C/C++ files
#
# Usage:
#   ./tools/check-format.sh           # check all staged files
#   ./tools/check-format.sh [files]   # check specific files
#
# Install as a git pre-commit hook:
#   ln -sf ../../tools/check-format.sh .git/hooks/pre-commit
#
# Excludes vendor/ and generated shader .inc files.

set -euo pipefail

if [ $# -gt 0 ]; then
    FILES="$*"
else
    FILES=$(git diff --cached --name-only --diff-filter=ACM \
            | grep -E '\.(cpp|c|h)$' \
            | grep -v vendor/ \
            | grep -v '_gl\.inc$' \
            || true)
fi

if [ -z "$FILES" ]; then
    echo "[format] No C/C++ files to check."
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
