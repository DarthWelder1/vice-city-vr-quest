# One-click preparation of the optional Modern overlay from a legal GTA Vice
# City installation and two externally hosted source packs. The packs are
# downloaded for the individual user and are never bundled in this repository.
[CmdletBinding()]
param(
    [string]$GameDir,
    [string]$WorkDir = "C:\VCVRBuild\modern-assets",
    [string]$OutputDir,
    [string]$AndroidSdk,
    [string]$Serial,
    [string]$HdArchive,
    [string]$ModsArchive,
    [switch]$BuildOnly,
    [switch]$AcceptDownloads,
    [switch]$NonInteractive,
    [string]$LogPath = (Join-Path $env:TEMP "ViceCityVR-Prepare-Modern-Models.log")
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
$hdUrl = "https://drive.usercontent.google.com/download?id=1Swe1dVWDnKz8ad51y8L0ihPWVCxmFRYj&export=download&confirm=t"
$modsUrl = "https://drive.usercontent.google.com/download?id=1y9KpKjLSna76bjz1Lf2DzP0G4AnkN_2d&export=download&confirm=t"
$hdSize = 1878280127L
$modsSize = 2377186981L
$hdSha256 = "81A7962479752F3A07004A2E12964815435CAF4B0A0F637EE982460171A5D94C"
$modsSha256 = "F33031091DBE50A3BEDCEEFAF3FDE6F6DDD96F9841AABE1ECD3713818DED9725"
$repoRoot = Split-Path $PSScriptRoot -Parent
$builder = Join-Path $PSScriptRoot "modelsets\build-modern-modelset.ps1"
$questInstaller = Join-Path $PSScriptRoot "install-modern-models.ps1"
$script:transcriptStarted = $false

try {
    $logDirectory = Split-Path -Parent $LogPath
    if (-not [string]::IsNullOrWhiteSpace($logDirectory)) {
        New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
    }
    Start-Transcript -Path $LogPath -Force | Out-Null
    $script:transcriptStarted = $true
} catch {
    Write-Host "Warning: diagnostic logging could not start: $($_.Exception.Message)" -ForegroundColor Yellow
}

function Stop-DiagnosticLog {
    if (-not $script:transcriptStarted) { return }
    try { Stop-Transcript | Out-Null } catch { }
    $script:transcriptStarted = $false
}

function Invoke-NativeChecked {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [string]$FailureMessage = "Command failed"
    )
    # Windows PowerShell 5.1 can turn ordinary native stderr (for example,
    # curl's progress meter) into a terminating ErrorRecord when the caller
    # uses Stop. Judge native tools by their exit code instead.
    $previousErrorAction = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        & $FilePath @Arguments
        $nativeExitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorAction
    }
    if ($nativeExitCode -ne 0) {
        throw "$FailureMessage (exit code $nativeExitCode)."
    }
}

function Resolve-GameFolder {
    param([string]$Requested)
    if ([string]::IsNullOrWhiteSpace($Requested)) {
        if ($NonInteractive) { throw "-GameDir is required in non-interactive mode." }
        try {
            Add-Type -AssemblyName System.Windows.Forms
            $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
            $dialog.Description = "Select the original GTA Vice City installation folder"
            $dialog.ShowNewFolderButton = $false
            if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) {
                throw "No GTA Vice City folder was selected."
            }
            $Requested = $dialog.SelectedPath
        } catch [System.Management.Automation.RuntimeException] {
            throw
        } catch {
            Write-Host "Folder picker was unavailable. Paste the original GTA Vice City folder path." -ForegroundColor Yellow
            $Requested = Read-Host "GTA Vice City folder"
        }
    }
    if ([string]::IsNullOrWhiteSpace($Requested) -or
        -not (Test-Path -LiteralPath $Requested -PathType Container)) {
        throw "The GTA Vice City folder does not exist: $Requested"
    }
    $resolved = (Resolve-Path -LiteralPath $Requested).Path
    foreach ($relative in @("models\gta3.img", "models\gta3.dir", "models\generic.txd")) {
        if (-not (Test-Path -LiteralPath (Join-Path $resolved $relative) -PathType Leaf)) {
            throw "The selected folder is not a complete original GTA Vice City installation. Missing: $relative"
        }
    }
    return $resolved
}

function Assert-FreeSpace {
    param([string]$Game, [string]$Work)
    $workRoot = [IO.Path]::GetPathRoot([IO.Path]::GetFullPath($Work))
    $gameRoot = [IO.Path]::GetPathRoot([IO.Path]::GetFullPath($Game))
    $workDrive = New-Object IO.DriveInfo($workRoot)
    $gameDrive = New-Object IO.DriveInfo($gameRoot)
    if ($workRoot -eq $gameRoot) {
        if ($workDrive.AvailableFreeSpace -lt 24GB) {
            throw ("At least 24 GB free is required on {0} for downloads, extraction and the staged build; only {1:N1} GB is available." -f $workRoot, ($workDrive.AvailableFreeSpace / 1GB))
        }
    } else {
        if ($workDrive.AvailableFreeSpace -lt 14GB) {
            throw ("At least 14 GB free is required on {0} for downloads and extraction; only {1:N1} GB is available." -f $workRoot, ($workDrive.AvailableFreeSpace / 1GB))
        }
        if ($gameDrive.AvailableFreeSpace -lt 12GB) {
            throw ("At least 12 GB free is required on {0} for the staged model build; only {1:N1} GB is available." -f $gameRoot, ($gameDrive.AvailableFreeSpace / 1GB))
        }
    }
}

function Test-FileIdentity {
    param([string]$Path, [long]$Size, [string]$Sha256)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    if ((Get-Item -LiteralPath $Path).Length -ne $Size) { return $false }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -eq $Sha256
}

function Ensure-Download {
    param(
        [string]$Name,
        [string]$Url,
        [string]$Destination,
        [long]$Size,
        [string]$Sha256,
        [string]$Provided
    )
    if (-not [string]::IsNullOrWhiteSpace($Provided)) {
        $providedPath = [IO.Path]::GetFullPath($Provided)
        Write-Host "Verifying supplied $Name archive..." -ForegroundColor Cyan
        if (-not (Test-FileIdentity -Path $providedPath -Size $Size -Sha256 $Sha256)) {
            throw "The supplied $Name archive does not match the tested size/SHA256: $providedPath"
        }
        return $providedPath
    }
    if (Test-FileIdentity -Path $Destination -Size $Size -Sha256 $Sha256) {
        Write-Host "Reusing verified download: $Destination" -ForegroundColor Green
        return $Destination
    }
    if (Test-Path -LiteralPath $Destination) {
        Remove-Item -LiteralPath $Destination -Force
    }
    $partial = "$Destination.partial"
    if ((Test-Path -LiteralPath $partial) -and
        (Get-Item -LiteralPath $partial).Length -gt $Size) {
        Remove-Item -LiteralPath $partial -Force
    }
    $curl = Get-Command curl.exe -ErrorAction SilentlyContinue
    if ($null -eq $curl) {
        throw "Windows curl.exe was not found. Install current Windows updates, then rerun."
    }
    Write-Host "Downloading $Name ($([Math]::Round($Size / 1GB, 2)) GB). Interrupted downloads resume automatically..." -ForegroundColor Cyan
    Invoke-NativeChecked -FilePath $curl.Source -Arguments @(
        "--fail", "--location", "--retry", "5", "--retry-all-errors",
        "--continue-at", "-", "--output", $partial, $Url
    ) -FailureMessage "$Name download failed"
    Write-Host "Verifying $Name SHA256..." -ForegroundColor Cyan
    if (-not (Test-FileIdentity -Path $partial -Size $Size -Sha256 $Sha256)) {
        throw "$Name download completed but failed the pinned size/SHA256 check. Delete '$partial' and retry."
    }
    Move-Item -LiteralPath $partial -Destination $Destination -Force
    return $Destination
}

function Ensure-Extracted {
    param([string]$Name, [string]$Archive, [string]$Destination, [string]$Sha256)
    $marker = Join-Path $Destination ".vcvr-source-sha256"
    if ((Test-Path -LiteralPath $marker -PathType Leaf) -and
        ((Get-Content -LiteralPath $marker -Raw).Trim() -eq $Sha256)) {
        Write-Host "Reusing verified extraction: $Destination" -ForegroundColor Green
        return $Destination
    }
    if (Test-Path -LiteralPath $Destination) {
        Remove-Item -LiteralPath $Destination -Recurse -Force
    }
    $parent = Split-Path -Parent $Destination
    New-Item -ItemType Directory -Path $parent,$Destination -Force | Out-Null
    $tar = Get-Command tar.exe -ErrorAction SilentlyContinue
    if ($null -eq $tar) { throw "Windows tar.exe was not found; it is required to extract the verified ZIP archives." }
    Write-Host "Extracting $Name..." -ForegroundColor Cyan
    Invoke-NativeChecked -FilePath $tar.Source -Arguments @(
        "-xf", $Archive, "-C", $Destination
    ) -FailureMessage "$Name extraction failed"
    [IO.File]::WriteAllText($marker, $Sha256, (New-Object Text.UTF8Encoding($false)))
    return $Destination
}

try {
    Write-Host "Vice City VR - download, build and install Modern models" -ForegroundColor Green
    Write-Host "Required input: only a legal original GTA Vice City PC installation."
    Write-Host "The wizard downloads two external packs used by the tested build; no pack is bundled or redistributed by this repository."
    Write-Host "Modern vegetation/palms are deliberately excluded and remain Classic on Quest."

    foreach ($requiredTool in @($builder, $questInstaller)) {
        if (-not (Test-Path -LiteralPath $requiredTool -PathType Leaf)) {
            throw "Required source-kit tool is missing: $requiredTool"
        }
    }
    $game = Resolve-GameFolder -Requested $GameDir
    $work = [IO.Path]::GetFullPath($WorkDir)
    New-Item -ItemType Directory -Path $work -Force | Out-Null
    if ([string]::IsNullOrWhiteSpace($OutputDir)) {
        $OutputDir = Join-Path $game "modelsets\modern"
    }
    $output = [IO.Path]::GetFullPath($OutputDir)
    Assert-FreeSpace -Game $game -Work $work

    if (-not $AcceptDownloads -and
        ([string]::IsNullOrWhiteSpace($HdArchive) -or [string]::IsNullOrWhiteSpace($ModsArchive))) {
        if ($NonInteractive) {
            throw "-AcceptDownloads is required in non-interactive mode when local verified archives are not supplied."
        }
        Write-Host ""
        Write-Host "About 4.0 GB will be downloaded from the two external links supplied for this project." -ForegroundColor Yellow
        Write-Host "The extracted/build workspace can temporarily use about 20-24 GB." -ForegroundColor Yellow
        $answer = Read-Host "Download, verify and build the Modern overlay now? [Y/n]"
        if (-not [string]::IsNullOrWhiteSpace($answer) -and $answer -notmatch '^[Yy]') {
            throw "Modern model download was cancelled; nothing was changed on the Quest."
        }
    }

    $downloads = Join-Path $work "downloads"
    $sources = Join-Path $work "sources"
    New-Item -ItemType Directory -Path $downloads,$sources -Force | Out-Null
    $hdZip = Ensure-Download -Name "GTA VC HD + Weapons" -Url $hdUrl `
        -Destination (Join-Path $downloads "GTA VC HD + Weapons.zip") `
        -Size $hdSize -Sha256 $hdSha256 -Provided $HdArchive
    $modsZip = Ensure-Download -Name "Mods / Atmosphere" -Url $modsUrl `
        -Destination (Join-Path $downloads "Mods.zip") `
        -Size $modsSize -Sha256 $modsSha256 -Provided $ModsArchive

    $hdSource = Ensure-Extracted -Name "GTA VC HD + Weapons" -Archive $hdZip `
        -Destination (Join-Path $sources "hd-pack") -Sha256 $hdSha256
    $modsSource = Ensure-Extracted -Name "Mods / Atmosphere" -Archive $modsZip `
        -Destination (Join-Path $sources "mods-pack") -Sha256 $modsSha256

    Write-Host "Building the optimized Quest Modern overlay. This can take several minutes..." -ForegroundColor Cyan
    $builderArguments = @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $builder,
        "-GameDir", $game,
        "-HdPack", $hdSource,
        "-AtmospherePack", $modsSource,
        "-Out", $output,
        "-Force"
    )
    Invoke-NativeChecked -FilePath "powershell.exe" -Arguments $builderArguments `
        -FailureMessage "Modern overlay build failed"

    if ($BuildOnly) {
        Write-Host ""
        Write-Host "MODERN OVERLAY BUILT: $output" -ForegroundColor Green
        Write-Host "Build-only mode complete."
        Write-Host "Diagnostic log: $LogPath"
        Stop-DiagnosticLog
        exit 0
    }

    Write-Host "Installing the completed overlay on the connected Quest..." -ForegroundColor Cyan
    $installArguments = @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $questInstaller,
        "-ModernDir", $output
    )
    if (-not [string]::IsNullOrWhiteSpace($AndroidSdk)) {
        $installArguments += @("-AndroidSdk", $AndroidSdk)
    }
    if (-not [string]::IsNullOrWhiteSpace($Serial)) {
        $installArguments += @("-Serial", $Serial)
    }
    if ($NonInteractive) { $installArguments += "-NonInteractive" }
    Invoke-NativeChecked -FilePath "powershell.exe" -Arguments $installArguments `
        -FailureMessage "Quest Modern model installation failed"

    Write-Host ""
    Write-Host "DOWNLOAD, BUILD AND QUEST INSTALL COMPLETED." -ForegroundColor Green
    Write-Host "Fully restart Vice City VR. Modern World/Weapons are selected by default; Vehicles/Peds/Vegetation remain Classic."
    Write-Host "Diagnostic log: $LogPath"
    Stop-DiagnosticLog
    exit 0
} catch {
    Write-Host ""
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "Existing verified downloads are kept for resume/reuse. No model pack is uploaded until the complete build succeeds."
    Write-Host "Diagnostic log: $LogPath"
    Stop-DiagnosticLog
    exit 1
}
