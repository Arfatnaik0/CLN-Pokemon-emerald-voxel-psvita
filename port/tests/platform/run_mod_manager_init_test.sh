#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
test_dir="$(mktemp -d)"
trap 'rm -rf "$test_dir"' EXIT

cc -std=gnu99 -fno-builtin -fno-strict-aliasing \
  -DPORTABLE -DNONMATCHING -DMODERN=1 -DUBFIX \
  -DPLATFORM_VITA -DPLATFORM_SDL2 -DRENDERER_EASY_DRAW \
  -iquote "$repo_root" -iquote "$repo_root/include" -iquote "$repo_root/gflib" \
  -I"$repo_root/src/platform/mods" \
  -ffunction-sections -fdata-sections \
  "$repo_root/src/platform/mods/mod_manager.c" \
  "$repo_root/tests/platform/mod_manager_init_test.c" \
  -Wl,--gc-sections -Wl,--wrap=malloc \
  -o "$test_dir/mod_manager_init_test"

"$test_dir/mod_manager_init_test"
