#!/bin/bash
# run-ci-local.sh — simulate the full GitHub Actions CI pipeline locally using act.
#
# Prerequisites:
#   - Docker running
#   - act installed (~/.local/bin/act or in PATH)
#     Install: curl -sL https://github.com/nektos/act/releases/latest/download/act_Linux_x86_64.tar.gz \
#                | tar -xz -C ~/.local/bin
#
# Usage:
#   bash scripts/run-ci-local.sh          # run all jobs (simulate push to master)
#   bash scripts/run-ci-local.sh format   # run a single job by name
#   bash scripts/run-ci-local.sh pr       # simulate a pull_request event

set -euo pipefail

ACT="${HOME}/.local/bin/act"
if ! command -v act &>/dev/null && [ ! -x "$ACT" ]; then
    echo "ERROR: act not found. Install with:"
    echo "  curl -sL https://github.com/nektos/act/releases/latest/download/act_Linux_x86_64.tar.gz | tar -xz -C ~/.local/bin"
    exit 1
fi
ACT=$(command -v act 2>/dev/null || echo "$ACT")

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT"

# act uses ubuntu-latest → map to the medium image for faster startup.
# First run will pull ~500 MB; subsequent runs use the cached image.
RUNNER_IMAGE="catthehacker/ubuntu:act-22.04"

case "${1:-all}" in
    all)
        echo "[ci-local] Simulating push to master — running all jobs..."
        "$ACT" push \
            --platform ubuntu-24.04="$RUNNER_IMAGE" \
            --artifact-server-path /tmp/act-artifacts \
            --rm
        ;;
    pr)
        echo "[ci-local] Simulating pull_request event..."
        "$ACT" pull_request \
            --platform ubuntu-24.04="$RUNNER_IMAGE" \
            --artifact-server-path /tmp/act-artifacts \
            --rm
        ;;
    *)
        echo "[ci-local] Running job: $1"
        "$ACT" push \
            --job "$1" \
            --platform ubuntu-24.04="$RUNNER_IMAGE" \
            --artifact-server-path /tmp/act-artifacts \
            --rm
        ;;
esac
