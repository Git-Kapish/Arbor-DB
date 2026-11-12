# Build + run tests inside the dev container. Build dir lives in a docker volume (ext4, real fsync).
# Usage: scripts\docker-test.ps1 [-Cmd "bash command to run after build"]
param([string]$Cmd = "cd /build && ctest --output-on-failure")
$root = Split-Path $PSScriptRoot -Parent
$script = "set -e; cmake -S /src -B /build -DCMAKE_BUILD_TYPE=Debug >/dev/null; " +
  "if ! cmake --build /build -j`$(nproc) > /tmp/build.log 2>&1; then grep -E -B2 -A6 'error' /tmp/build.log | head -80; exit 1; fi; " +
  "grep -E 'warning' /tmp/build.log | head -20 || true; $Cmd"
docker run --rm -v "${root}:/src" -v arbor-build:/build arbordb-dev bash -c $script
