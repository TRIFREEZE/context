#!/usr/bin/env bash
# Fetches exactly the JUCE commit pinned in dependencies/JUCE_COMMIT into .deps/JUCE (shallow).
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
commit="$(head -n1 "$repo_root/dependencies/JUCE_COMMIT" | tr -d '[:space:]')"
dest="$repo_root/.deps/JUCE"

[[ "$commit" =~ ^[0-9a-f]{40}$ ]] || { echo "dependencies/JUCE_COMMIT must hold a full commit hash" >&2; exit 1; }

if [[ -d "$dest/.git" ]]; then
  if [[ "$(git -C "$dest" rev-parse HEAD 2>/dev/null || true)" == "$commit" ]]; then
    echo "JUCE $commit already present at $dest"; exit 0
  fi
  rm -rf "$dest"
fi

mkdir -p "$dest"
git -C "$dest" init -q
git -C "$dest" remote add origin https://github.com/juce-framework/JUCE.git
git -C "$dest" fetch -q --depth 1 origin "$commit"
git -C "$dest" checkout -q FETCH_HEAD
echo "JUCE $commit fetched to $dest"
