# Repairs an existing Vice City VR installation that is missing the port's
# replacement text and frontend assets.
#
# reVC does not run on retail Vice City data alone. Its frontend reads strings
# and button icons that a 2002 installation never shipped, and when they are
# absent the OPTIONS menus print "FET_GFX missing" and the like where the labels
# belong. Installations made before the build wizard copied these files show
# exactly that.
#
# This script copies nothing but those port assets, from the reVC source the
# wizard already downloaded. It touches no save, no setting and no model data.
[CmdletBinding()]
param(
    [string]$GameFilesDir,
    [string]$WorkDir = "C:\VCVRBuild",
    [string]$AndroidSdk,
    [string]$Serial,
    [string]$LogPath = (Join-Path $env:TEMP "ViceCityVR-Install-Game-Files.log"),
    [switch]$NonInteractive
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
$installerVersion = "0.5.2-gamefiles-1"
$packageName = "com.miamivr.quest"
$remoteGameData = "/sdcard/Android/data/$packageName/files/gamedata"
$testedRevcCommit = "026cd10f3fdbd92c089830e5067c4457c53c1b51"
$revcUrl = "https://github.com/mrxenginner/reVC.git"
$revcBranch = "miami"
$revcDirName = "reVC-public-026cd10"
$script:transcriptStarted = $false
$script:adb = $null

# The exact set the Quest build reads and a retail installation does not
# provide. CFont::Initialise loads MODELS/X360BTNS.TXD at start-up; the rest
# are reVC's replacements for the stock text and frontend art.
$portAssets = @(
    @{ Folder = "TEXT"; Files = @(
        "american.gxt", "french.gxt", "german.gxt",
        "italian.gxt", "russian.gxt", "spanish.gxt") },
    @{ Folder = "models"; Files = @(
        "fonts_r.txd", "frontend_ds2.txd", "frontend_ds3.txd",
        "frontend_ds4.txd", "frontend_x360.txd", "frontend_xone.txd",
        "generic.txd", "particle.txd", "ps3btns.txd", "x360btns.txd") }
)

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

function Invoke-NativeCapture {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )
    $savedErrorPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $lines = @(& $FilePath @Arguments 2>&1)
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $savedErrorPreference
    }
    return [PSCustomObject]@{ ExitCode = $exitCode; Lines = $lines }
}

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [string]$FailureMessage = "Command failed"
    )
    $result = Invoke-NativeCapture -FilePath $FilePath -Arguments $Arguments
    $result.Lines | ForEach-Object { Write-Host $_ }
    if ($result.ExitCode -ne 0) {
        throw "$FailureMessage (exit code $($result.ExitCode))."
    }
}

function Get-AdbArguments {
    param([string[]]$Arguments)
    if ([string]::IsNullOrWhiteSpace($Serial)) { return $Arguments }
    return @("-s", $Serial) + $Arguments
}

function Invoke-AdbChecked {
    param(
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [string]$FailureMessage = "ADB command failed",
        [switch]$Quiet
    )
    $result = Invoke-NativeCapture -FilePath $script:adb `
        -Arguments (Get-AdbArguments $Arguments)
    if (-not $Quiet) { $result.Lines | ForEach-Object { Write-Host $_ } }
    if ($result.ExitCode -ne 0) {
        $details = ($result.Lines | Out-String).Trim()
        throw "$FailureMessage (exit code $($result.ExitCode)). $details"
    }
}

function Find-Adb {
    $candidates = [System.Collections.Generic.List[string]]::new()
    if (-not [string]::IsNullOrWhiteSpace($AndroidSdk)) {
        $candidates.Add((Join-Path $AndroidSdk "platform-tools\adb.exe"))
    }
    if (-not [string]::IsNullOrWhiteSpace($env:ANDROID_HOME)) {
        $candidates.Add((Join-Path $env:ANDROID_HOME "platform-tools\adb.exe"))
    }
    if (-not [string]::IsNullOrWhiteSpace($env:ANDROID_SDK_ROOT)) {
        $candidates.Add((Join-Path $env:ANDROID_SDK_ROOT "platform-tools\adb.exe"))
    }
    $candidates.Add((Join-Path $WorkDir ".android-sdk\platform-tools\adb.exe"))
    $candidates.Add("C:\VCVRBuild\.android-sdk\platform-tools\adb.exe")
    if (-not [string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) {
        $candidates.Add((Join-Path $env:LOCALAPPDATA "Android\Sdk\platform-tools\adb.exe"))
    }
    $installed = Get-Command adb.exe -ErrorAction SilentlyContinue
    if ($null -ne $installed) { $candidates.Add($installed.Source) }

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw "ADB was not found. Run BUILD_AND_INSTALL.bat once, or pass -AndroidSdk with the SDK folder."
}

function Test-GameFilesFolder {
    param([string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path) -or
        -not (Test-Path -LiteralPath $Path -PathType Container)) { return $false }
    foreach ($asset in $portAssets) {
        foreach ($name in $asset.Files) {
            if (-not (Test-Path -LiteralPath (Join-Path $Path "$($asset.Folder)\$name") -PathType Leaf)) {
                return $false
            }
        }
    }
    return $true
}

function Find-Git {
    $installed = Get-Command git.exe -ErrorAction SilentlyContinue
    if ($null -ne $installed) { return $installed.Source }
    $portable = Get-ChildItem -Path (Join-Path $WorkDir ".tools") -Filter "MinGit-*" `
        -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending
    foreach ($candidate in $portable) {
        $exe = Join-Path $candidate.FullName "cmd\git.exe"
        if (Test-Path -LiteralPath $exe -PathType Leaf) { return $exe }
    }
    return $null
}

# The wizard leaves a clean pinned reVC checkout and an assembled tree behind,
# so a repair normally copies from disk and downloads nothing.
function Resolve-GameFilesFolder {
    if (-not [string]::IsNullOrWhiteSpace($GameFilesDir)) {
        $requested = [IO.Path]::GetFullPath($GameFilesDir.Trim('"'))
        foreach ($candidate in @($requested, (Join-Path $requested "gamefiles"))) {
            if (Test-GameFilesFolder -Path $candidate) {
                return (Resolve-Path -LiteralPath $candidate).Path
            }
        }
        throw "-GameFilesDir '$requested' does not contain the reVC gamefiles (TEXT\american.gxt and models\x360btns.txd)."
    }

    $work = [IO.Path]::GetFullPath($WorkDir)
    $candidates = [System.Collections.Generic.List[string]]::new()
    $candidates.Add((Join-Path $work "$revcDirName\gamefiles"))
    $assembled = Get-ChildItem -Path $work -Filter "vice-city-vr-build*" `
        -Directory -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending
    foreach ($tree in $assembled) {
        $candidates.Add((Join-Path $tree.FullName "gamefiles"))
    }
    foreach ($candidate in $candidates) {
        if (Test-GameFilesFolder -Path $candidate) {
            Write-Host "Using the port assets already on this PC: $candidate"
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    $gitExe = Find-Git
    if ($null -eq $gitExe) {
        throw "No reVC source was found under $work and Git is not available. Run BUILD_AND_INSTALL.bat once, or pass -GameFilesDir with a reVC checkout."
    }
    $revcDir = Join-Path $work $revcDirName
    if (Test-Path -LiteralPath $revcDir) {
        throw "$revcDir exists but has no usable gamefiles folder. Remove it and retry, or pass -GameFilesDir."
    }
    Write-Host "Downloading the tested reVC source for its replacement assets..." -ForegroundColor Yellow
    New-Item -ItemType Directory -Path $work -Force | Out-Null
    Invoke-Checked -FilePath $gitExe -Arguments @(
        "clone", "--no-checkout", "-b", $revcBranch, $revcUrl, $revcDir
    ) -FailureMessage "reVC clone failed"
    Invoke-Checked -FilePath $gitExe -Arguments @(
        "-c", "safe.directory=$($revcDir.Replace('\', '/'))", "-C", $revcDir,
        "checkout", "--detach", $testedRevcCommit
    ) -FailureMessage "Could not select the tested reVC commit"

    $downloaded = Join-Path $revcDir "gamefiles"
    if (-not (Test-GameFilesFolder -Path $downloaded)) {
        throw "The downloaded reVC source does not contain the expected gamefiles."
    }
    return (Resolve-Path -LiteralPath $downloaded).Path
}

function Select-QuestDevice {
    $result = Invoke-NativeCapture -FilePath $script:adb -Arguments @("devices", "-l")
    if ($result.ExitCode -ne 0) { throw "adb devices failed." }
    $devices = @()
    foreach ($line in $result.Lines) {
        if (([string]$line) -match '^(\S+)\s+device(?:\s|$)') { $devices += $Matches[1] }
    }
    if (-not [string]::IsNullOrWhiteSpace($Serial)) {
        if ($devices -notcontains $Serial) {
            throw "Quest '$Serial' is not connected and authorized. Accept the USB debugging prompt inside the headset."
        }
        return
    }
    if ($devices.Count -eq 0) {
        throw "No authorized Quest was found. Connect it by USB, enable USB debugging and accept the prompt inside the headset."
    }
    if ($devices.Count -eq 1) {
        $script:Serial = $devices[0]
        return
    }
    if ($NonInteractive) { throw "Multiple Android devices are connected; pass -Serial." }
    Write-Host "Connected Android devices:" -ForegroundColor Yellow
    for ($index = 0; $index -lt $devices.Count; $index++) {
        Write-Host "  $($index + 1). $($devices[$index])"
    }
    $answer = Read-Host "Select the Quest number"
    $selection = 0
    if (-not [int]::TryParse($answer, [ref]$selection) -or
        $selection -lt 1 -or $selection -gt $devices.Count) {
        throw "Invalid device selection."
    }
    $script:Serial = $devices[$selection - 1]
}

try {
    Write-Host "Vice City VR - port text and frontend asset repair ($installerVersion)" -ForegroundColor Green
    Write-Host "This replaces only the port's own text and frontend files. Saves, settings and models are untouched."
    Write-Host ""

    $source = Resolve-GameFilesFolder
    $script:adb = Find-Adb
    Write-Host "ADB: $script:adb"
    Select-QuestDevice
    Write-Host "Quest: $Serial"

    # A missing gamedata root means the game was never given its data; copying
    # the port assets into an empty tree would look like success and still fail
    # on launch, so say what is actually wrong instead.
    $probe = Invoke-NativeCapture -FilePath $script:adb `
        -Arguments (Get-AdbArguments @("shell", "ls", "$remoteGameData/models/gta3.img"))
    if ($probe.ExitCode -ne 0 -or (($probe.Lines | Out-String) -match "No such file")) {
        throw "No Vice City data was found at $remoteGameData. Install the game with BUILD_AND_INSTALL.bat first; this tool only repairs an existing installation."
    }

    $copied = 0
    foreach ($asset in $portAssets) {
        $localFolder = Join-Path $source $asset.Folder
        $remoteFolder = "$remoteGameData/$($asset.Folder)"
        Invoke-AdbChecked -Arguments @("shell", "mkdir", "-p", $remoteFolder) `
            -FailureMessage "Could not create $remoteFolder on the headset" -Quiet
        foreach ($name in $asset.Files) {
            Write-Host "  $($asset.Folder)/$name" -ForegroundColor Cyan
            Invoke-AdbChecked -Arguments @(
                "push", (Join-Path $localFolder $name), "$remoteFolder/$name"
            ) -FailureMessage "Failed to copy $($asset.Folder)/$name" -Quiet
            $copied++
        }
    }

    Invoke-AdbChecked -Arguments @("shell", "am", "force-stop", $packageName) `
        -FailureMessage "Could not leave the app stopped" -Quiet

    Write-Host ""
    Write-Host "$copied files installed. Launch Vice City VR again." -ForegroundColor Green
    Write-Host "Diagnostic log: $LogPath"
    Stop-DiagnosticLog
} catch {
    Write-Host ""
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "Nothing was silently ignored. Fix the reported item and rerun."
    Write-Host "Diagnostic log: $LogPath"
    Stop-DiagnosticLog
    exit 1
}
