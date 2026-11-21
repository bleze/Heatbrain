<#
.SYNOPSIS
  Build, test and run heatbrain from a plain terminal (Visual Studio does this for you
  when you open the folder; this is for the command line).

.EXAMPLE
  ./scripts/dev.ps1 build
  ./scripts/dev.ps1 test
  ./scripts/dev.ps1 run prices
#>
param(
    [Parameter(Position = 0)][ValidateSet('configure', 'build', 'test', 'run')][string]$Action = 'build',
    [string]$Preset = 'windows-debug',
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Rest
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

if (-not $env:VSCMD_VER) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'Visual Studio with C++ tools not found' }
    Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
}

Push-Location $root
try {
    $buildDir = "out/build/$Preset"
    if ($Action -eq 'configure' -or -not (Test-Path "$buildDir/CMakeCache.txt")) {
        cmake --preset $Preset; if ($LASTEXITCODE) { exit $LASTEXITCODE }
    }
    if ($Action -in 'build', 'test', 'run') {
        cmake --build --preset $Preset; if ($LASTEXITCODE) { exit $LASTEXITCODE }
    }
    if ($Action -eq 'test') {
        & "$buildDir/heatbrain_tests.exe" @Rest; exit $LASTEXITCODE
    }
    if ($Action -eq 'run') {
        & "$buildDir/heatbrain.exe" @Rest; exit $LASTEXITCODE
    }
}
finally { Pop-Location }
