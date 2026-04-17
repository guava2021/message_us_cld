FROM catthehacker/ubuntu:act-22.04

# Pre-install all tools needed by every CI job.
# Rebuild this image only when tool versions change:
#   docker build -t spsc-ci:local -f .github/ci.Dockerfile .

RUN apt-get update -qq && \
    apt-get install -y --no-install-recommends \
        cmake \
        ninja-build \
        gcc \
        g++ \
        clang \
        clang-format \
        clang-tidy && \
    rm -rf /var/lib/apt/lists/*
