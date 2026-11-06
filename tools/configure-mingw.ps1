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
    # 目录迁移后显式重建当前预设的缓存，不手动替换生成文件中的绝对路径。
    if ($Fresh) {
        & cmake --fresh --preset $Preset
    }
    else {
        & cmake --preset $Preset
    }

    # 原生命令的非零退出码不一定触发 PowerShell 的 Stop，必须单独检查。
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configure failed for preset '$Preset' (exit code: $LASTEXITCODE)."
    }
}
finally {
    Pop-Location
}
