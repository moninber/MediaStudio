[CmdletBinding()]
param(
    [switch]$Fresh
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$Preset = if (Test-Path (Join-Path $ProjectRoot "CMakeUserPresets.json")) {
    "mingw-release-local"
}
else {
    "mingw-release"
}

Push-Location $ProjectRoot
try {
    # 复用配置脚本的缓存重建和失败检查，配置失败时不继续使用旧产物构建。
    & (Join-Path $PSScriptRoot "configure-mingw.ps1") -Fresh:$Fresh
    & cmake --build --preset $Preset
    if ($LASTEXITCODE -ne 0) {
        throw "CMake build failed for preset '$Preset' (exit code: $LASTEXITCODE)."
    }
}
finally {
    Pop-Location
}
