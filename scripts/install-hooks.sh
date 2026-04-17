#!/bin/bash
# install-hooks.sh — symlink committed hook scripts into .git/hooks/.
# Run once after cloning the repo.
#
# Usage: bash scripts/install-hooks.sh

set -euo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel)"
HOOKS_SRC="$REPO_ROOT/scripts/hooks"
HOOKS_DST="$REPO_ROOT/.git/hooks"

hooks=(pre-commit pre-push)

for hook in "${hooks[@]}"; do
    src="$HOOKS_SRC/$hook"
    dst="$HOOKS_DST/$hook"

    if [ ! -f "$src" ]; then
        echo "ERROR: $src not found"
        exit 1
    fi

    chmod +x "$src"
    ln -sf "$src" "$dst"
    echo "Installed: $dst -> $src"
done

echo "Git hooks installed successfully."
echo "To skip hooks temporarily: git commit --no-verify / git push --no-verify"
