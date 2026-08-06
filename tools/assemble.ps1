# Assembles a buildable source tree for the MiamiVR Quest port.
#
#   .\tools\assemble.ps1 -Revc <path to reVC source> -Out <output dir>
#
# The reVC source tree is not part of this repository and is not distributed
# with it. Obtain it separately; the patch targets the final (September 2021)
# state of the reVC master branch.
param(
    [Parameter(Mandatory=$true)][string]$Revc,
    [Parameter(Mandatory=$true)][string]$Out
)
$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent

if (-not (Test-Path (Join-Path $Revc "src\core\main.cpp"))) {
    Write-Error "'$Revc' does not look like a reVC source tree (src\core\main.cpp not found)"
}
if (Test-Path $Out) { Write-Error "'$Out' already exists; refusing to overwrite" }

Write-Host "[1/4] Copying the reVC tree..."
New-Item -ItemType Directory -Path $Out | Out-Null
robocopy $Revc $Out /E /NFL /NDL /NJH /NJS /XD ".git" | Out-Null

Write-Host "[2/4] Applying the port patch..."
Push-Location $Out
git init -q 2>$null
git apply --whitespace=nowarn (Join-Path $repo "patches\revc-quest.patch")
if ($LASTEXITCODE -ne 0) {
    Pop-Location
    Write-Error "Patch did not apply. Make sure the reVC tree is the final master state and unmodified."
}
Remove-Item -Recurse -Force (Join-Path $Out ".git")
Pop-Location

Write-Host "[3/4] Copying the port sources..."
robocopy (Join-Path $repo "overlay") $Out /E /NFL /NDL /NJH /NJS | Out-Null

Write-Host "[4/4] Copying librw with the Vulkan backend..."
robocopy (Join-Path $repo "librw") (Join-Path $Out "vendor\librw") /E /NFL /NDL /NJH /NJS | Out-Null

Write-Host ""
Write-Host "Done. Next steps:"
Write-Host "  cd $Out\android"
Write-Host "  gradle wrapper   (first time only, or open in Android Studio)"
Write-Host "  .\gradlew assembleDebug"
Write-Host "See BUILDING.md for prerequisites and installing to the headset."
