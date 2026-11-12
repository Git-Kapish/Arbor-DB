# Build + test inside the dev container. Each (type, sanitizer) pair gets its own docker volume (ext4, real fsync).
# Usage: scripts\docker-test.ps1 [-Type Debug|Release] [-Sanitizer auto|address|thread|none] [-Cmd "bash to run INSTEAD of ctest"]
param([string]$Type = "Debug", [string]$Sanitizer = "auto", [string]$Cmd = "", [string[]]$DockerArgs = @())
$root = Split-Path $PSScriptRoot -Parent
$vol = "arbor-build-$($Type.ToLower())-$($Sanitizer.ToLower())"
if ($Cmd -eq "") { $run = "bash /src/scripts/ci-build.sh /build $Type $Sanitizer" }
else {
  # build only, then run the custom command from the build dir
  $run = "set -e; cmake -S /src -B /build -DCMAKE_BUILD_TYPE=$Type -DARBOR_SANITIZER=$Sanitizer >/dev/null; " +
    "if ! cmake --build /build -j`$(nproc) > /tmp/build.log 2>&1; then grep -E -B2 -A8 'error' /tmp/build.log | head -80; exit 1; fi; cd /build; $Cmd"
}
$extra = @($DockerArgs)
if ($Sanitizer -eq "thread") { $extra = @("--security-opt", "seccomp=unconfined") }  # needed for setarch -R, see ci-build.sh
docker run --rm @extra -v "${root}:/src" -v "${vol}:/build" arbordb-dev bash -c $run
