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
#   bash scripts/run-ci-local.sh rebuild  # rebuild the CI Docker image

set -euo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT"

# ── Docker image ──────────────────────────────────────────────────────────────
# Pre-built image with all CI tools installed. Built once; reused on every run.
RUNNER_IMAGE="spsc-ci:local"
DOCKERFILE=".github/ci.Dockerfile"

build_image() {
    echo "[ci-local] Building CI image '${RUNNER_IMAGE}' (one-time, ~2 min)..."
    docker build -t "$RUNNER_IMAGE" -f "$DOCKERFILE" .
    echo "[ci-local] Image built. Future runs will skip this step."
}

# Build image automatically if it doesn't exist yet.
if ! docker image inspect "$RUNNER_IMAGE" &>/dev/null; then
    build_image
fi

# ── act ───────────────────────────────────────────────────────────────────────
ACT="${HOME}/.local/bin/act"
if ! command -v act &>/dev/null && [ ! -x "$ACT" ]; then
    echo "ERROR: act not found. Install with:"
    echo "  curl -sL https://github.com/nektos/act/releases/latest/download/act_Linux_x86_64.tar.gz | tar -xz -C ~/.local/bin"
    exit 1
fi
ACT=$(command -v act 2>/dev/null || echo "$ACT")

# ── run ───────────────────────────────────────────────────────────────────────
run_act() {
    "$ACT" "$@" \
        --platform ubuntu-24.04="$RUNNER_IMAGE" \
        --artifact-server-path /tmp/act-artifacts \
        --pull=false \
        --rm
}

case "${1:-all}" in
    rebuild)
        build_image
        ;;
    all)
        echo "[ci-local] Simulating push to master — running all jobs..."
        run_act push
        ;;
    pr)
        echo "[ci-local] Simulating pull_request event..."
        run_act pull_request
        ;;
    *)
        echo "[ci-local] Running job: $1"
        run_act push --job "$1"
        ;;
esac
