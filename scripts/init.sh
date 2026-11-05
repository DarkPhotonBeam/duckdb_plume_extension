#!/usr/bin/env bash

duckdb_version="v1.5.4"
duckdb_patch_directory="third_party/plume/patches/"
duckdb_patches=(
  "0001-expose-contextless-expression-executor-ctor.patch"
  "0002-extractplan-resolve-types-before-bindings.patch"
)
plume_patch_directory="patches/plume/"
plume_patches=(
  "0001_root_cmakelists_get_plume_remote_fn.patch"
)

fetch_checkout() {
  git -C duckdb fetch --depth 1 origin tag "$duckdb_version" &>/dev/null
  git -C duckdb checkout "$duckdb_version" &>/dev/null

  git -C extension-ci-tools fetch origin &>/dev/null
  git -C extension-ci-tools checkout origin/"$duckdb_version" &>/dev/null

  git add duckdb extension-ci-tools
}

apply_patches() {
  local submodule="$1"
  local patch_dir="$2"

  # check that submodule exists
  [[ -d "$submodule" ]] || {
    printf "submodule %s doesn't exist\n" "$submodule" >&2
    exit 2
  }

  shift 2

  for patch in "$@"; do
    local path="$(pwd)/$patch_dir$patch"
    # check that patch file exists
    [[ -f "$path" ]] || {
      printf "error: patch %s not found in %s\n" "$patch" "$patch_dir" >&2
      exit 2
    }
    printf 'Applying patch %s to %s\n' "$patch_dir$patch" "$submodule"
    if git -C "$submodule" apply --reverse --check "$path" &>/dev/null; then
      printf 'Skipped. Already applied.\n'
    elif git -C "$submodule" apply --check "$path" &>/dev/null; then
      git -C "$submodule" apply "$path"
      printf 'Patch applied.\n'
    else
      printf 'Could not apply patch: Conflicts or partially applied.\n'
    fi
  done
}

### MAIN ###

# Make sure script is called from the project root
[[ -f "scripts/init.sh" ]] || { printf 'Make sure you call this script from the project root. Exiting...\n' >&2; exit 2; }

# Fetch correct submodule versions for duckdb and extension-ci-tools (for plume compatibility):
echo "Fetching correct submodule versions..."
fetch_checkout
echo "Fetched correct submodule versions."

# Apply patches to duckdb for plume
printf "\nApplying patches...\n"
apply_patches "duckdb" "$duckdb_patch_directory" "${duckdb_patches[@]}"
apply_patches "third_party/plume" "$plume_patch_directory" "${plume_patches[@]}"
echo "Applied patches."

printf "\nDone.\n"

# Check VCPKG_TOOLCHAIN_PATH
if [[ -z "$VCPKG_ROOT" ]]; then
  printf 'Warning: VCPKG_ROOT env var is empty. Make sure vcpkg is installed before running make.\n'
else
  printf '\nReady to build:\n\nVCPKG_TOOLCHAIN_PATH=%s/scripts/buildsystems/vcpkg.cmake \\\nmake\n' "$VCPKG_ROOT"
fi

exit 0
