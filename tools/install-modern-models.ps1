# Installs a locally generated optional Modern model overlay on a connected
# Quest. No APK or third-party assets are downloaded by this script.
[CmdletBinding()]
param(
    [string]$ModernDir,
    [string]$AndroidSdk,
    [string]$Serial,
    [string]$LogPath = (Join-Path $env:TEMP "ViceCityVR-Install-Modern-Models.log"),
    [switch]$NonInteractive
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
$installerVersion = "0.5.1-models-2"
$packageName = "com.miamivr.quest"
$remoteModelSets = "/sdcard/Android/data/$packageName/files/gamedata/modelsets"
$repoRoot = Split-Path $PSScriptRoot -Parent
$script:transcriptStarted = $false
$script:adb = $null
$script:stagingRemote = $null

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

function Get-AdbArguments {
    param([string[]]$Arguments)
    if ([string]::IsNullOrWhiteSpace($Serial)) { return $Arguments }
    return @("-s", $Serial) + $Arguments
}

function Invoke-AdbCapture {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)
    return Invoke-NativeCapture -FilePath $script:adb -Arguments (Get-AdbArguments $Arguments)
}

function Invoke-AdbChecked {
    param(
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [string]$FailureMessage = "ADB command failed",
        [switch]$Quiet
    )
    $result = Invoke-AdbCapture -Arguments $Arguments
    if (-not $Quiet) { $result.Lines | ForEach-Object { Write-Host $_ } }
    if ($result.ExitCode -ne 0) {
        $details = ($result.Lines | Out-String).Trim()
        throw "$FailureMessage (exit code $($result.ExitCode)). $details"
    }
    return $result
}

function Get-RelativeRemotePath {
    param([string]$Root, [string]$Path)
    return $Path.Substring($Root.Length).TrimStart([char[]]"\/").Replace("\", "/")
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

function Test-ModernFolder {
    param([string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path) -or
        -not (Test-Path -LiteralPath $Path -PathType Container)) { return $false }
    $required = @(
        "vegetation_models.txt",
        "models\gta3.img",
        "models\gta3.dir"
    )
    foreach ($relative in $required) {
        if (-not (Test-Path -LiteralPath (Join-Path $Path $relative) -PathType Leaf)) {
            return $false
        }
    }
    return $true
}

function Resolve-ModernFolder {
    param([string]$Requested)
    if ([string]::IsNullOrWhiteSpace($Requested)) {
        if ($NonInteractive) { throw "-ModernDir is required in non-interactive mode." }
        try {
            Add-Type -AssemblyName System.Windows.Forms
            $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
            $dialog.Description = "Select the generated modelsets\modern folder"
            $dialog.ShowNewFolderButton = $false
            if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) {
                throw "No Modern model folder was selected."
            }
            $Requested = $dialog.SelectedPath
        } catch [System.Management.Automation.RuntimeException] {
            throw
        } catch {
            Write-Host "Folder picker was unavailable. Paste the generated modern folder path." -ForegroundColor Yellow
            $Requested = Read-Host "Modern folder"
        }
    }

    $root = [IO.Path]::GetFullPath($Requested.Trim('"'))
    $candidates = @(
        $root,
        (Join-Path $root "modern"),
        (Join-Path $root "modelsets\modern")
    )
    foreach ($candidate in $candidates) {
        if (Test-ModernFolder -Path $candidate) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw "The selected folder is not a generated Modern overlay. Required: vegetation_models.txt, models\gta3.img and models\gta3.dir."
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
    Write-Host "Vice City VR - Modern model installer ($installerVersion)" -ForegroundColor Green
    Write-Host "This copies a model overlay generated locally by the player; it downloads no model assets."

    $modern = Resolve-ModernFolder -Requested $ModernDir
    $script:adb = Find-Adb
    Select-QuestDevice

    Write-Host "Modern folder: $modern"
    Write-Host "ADB: $script:adb"
    Write-Host "Quest: $Serial"

    $packageResult = Invoke-AdbCapture -Arguments @("shell", "pm", "path", $packageName)
    if ($packageResult.ExitCode -ne 0 -or
        (($packageResult.Lines | Out-String) -notmatch "package:")) {
        throw "Vice City VR is not installed on the selected Quest. Run BUILD_AND_INSTALL.bat first."
    }

    # Starting the provider creates the application-owned external directory on
    # a fresh install before the ADB shell writes any children into it.
    Invoke-AdbChecked -Arguments @(
        "shell", "content", "query", "--uri", "content://com.miamivr.quest.saves/slot/1",
        "--projection", "_display_name:_size"
    ) -FailureMessage "Could not bootstrap Vice City VR application storage" -Quiet | Out-Null
    Invoke-AdbChecked -Arguments @("shell", "am", "force-stop", $packageName) `
        -FailureMessage "Could not stop Vice City VR before copying" -Quiet | Out-Null

    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $script:stagingRemote = "$remoteModelSets/.modern-incoming-$stamp"
    $backupRemote = "$remoteModelSets/.modern-backup-$stamp"
    $finalRemote = "$remoteModelSets/modern"

    Invoke-AdbChecked -Arguments @("shell", "mkdir", "-p", $script:stagingRemote) `
        -FailureMessage "Could not create the temporary Modern directory" -Quiet | Out-Null

    $directories = @(Get-ChildItem -LiteralPath $modern -Directory -Recurse -Force |
        Sort-Object { $_.FullName.Length })
    foreach ($directory in $directories) {
        $relative = Get-RelativeRemotePath -Root $modern -Path $directory.FullName
        Invoke-AdbChecked -Arguments @("shell", "mkdir", "-p", "$script:stagingRemote/$relative") `
            -FailureMessage "Could not create temporary Modern directory: $relative" -Quiet | Out-Null
    }

    $files = @(Get-ChildItem -LiteralPath $modern -File -Recurse -Force)
    if ($files.Count -eq 0) { throw "The selected Modern folder is empty." }
    Write-Host "Copying $($files.Count) Modern files into pre-created app storage. This can take several minutes..." -ForegroundColor Cyan
    $fileIndex = 0
    foreach ($file in $files) {
        $fileIndex++
        $relative = Get-RelativeRemotePath -Root $modern -Path $file.FullName
        $destinationRemote = "$script:stagingRemote/$relative"
        Write-Host "  [$fileIndex/$($files.Count)] $relative"
        Invoke-AdbChecked -Arguments @("push", $file.FullName, $destinationRemote) `
            -FailureMessage "Failed while transferring $relative" | Out-Null
        $sizeResult = Invoke-AdbChecked -Arguments @("shell", "stat", "-c", "%s", $destinationRemote) `
            -FailureMessage "Could not verify copied size for $relative" -Quiet
        $remoteSizeText = (($sizeResult.Lines | Out-String).Trim() -replace '[^0-9]', '')
        $remoteSize = 0L
        if (-not [long]::TryParse($remoteSizeText, [ref]$remoteSize) -or
            $remoteSize -ne $file.Length) {
            throw "Copied size mismatch for $relative (PC $($file.Length), Quest $remoteSizeText)."
        }
    }

    foreach ($requiredRemote in @(
        "$script:stagingRemote/vegetation_models.txt",
        "$script:stagingRemote/models/gta3.img",
        "$script:stagingRemote/models/gta3.dir"
    )) {
        Invoke-AdbChecked -Arguments @("shell", "test", "-f", $requiredRemote) `
            -FailureMessage "Required copied file is missing: $requiredRemote" -Quiet | Out-Null
    }

    $existingResult = Invoke-AdbCapture -Arguments @("shell", "test", "-d", $finalRemote)
    $hadExisting = $existingResult.ExitCode -eq 0
    if ($hadExisting) {
        Invoke-AdbChecked -Arguments @("shell", "rm", "-rf", $backupRemote) `
            -FailureMessage "Could not prepare the Modern backup path" -Quiet | Out-Null
        Invoke-AdbChecked -Arguments @("shell", "mv", $finalRemote, $backupRemote) `
            -FailureMessage "Could not preserve the previous Modern folder" -Quiet | Out-Null
    }

    $commitResult = Invoke-AdbCapture -Arguments @("shell", "mv", $script:stagingRemote, $finalRemote)
    if ($commitResult.ExitCode -ne 0) {
        if ($hadExisting) {
            Invoke-AdbCapture -Arguments @("shell", "mv", $backupRemote, $finalRemote) | Out-Null
        }
        throw "Could not activate the copied Modern folder. The previous folder was restored when possible. $($commitResult.Lines | Out-String)"
    }
    $script:stagingRemote = $null
    if ($hadExisting) {
        Invoke-AdbChecked -Arguments @("shell", "rm", "-rf", $backupRemote) `
            -FailureMessage "New Modern models are active, but the temporary old backup could not be removed" -Quiet | Out-Null
    }

    Invoke-AdbChecked -Arguments @("shell", "ls", "$finalRemote/models/gta3.img") `
        -FailureMessage "Final Modern archive verification failed" | Out-Null
    Invoke-AdbChecked -Arguments @("shell", "ls", "$finalRemote/vegetation_models.txt") `
        -FailureMessage "Final vegetation manifest verification failed" | Out-Null
    Invoke-AdbChecked -Arguments @("shell", "am", "force-stop", $packageName) `
        -FailureMessage "Could not leave Vice City VR stopped" -Quiet | Out-Null

    Write-Host ""
    Write-Host "MODERN MODELS INSTALLED." -ForegroundColor Green
    Write-Host "Fully restart Vice City VR. Default: Modern World/Weapons; Classic Vehicles/Peds/Vegetation."
    Write-Host "Diagnostic log: $LogPath"
    Stop-DiagnosticLog
    exit 0
} catch {
    if ($null -ne $script:adb -and -not [string]::IsNullOrWhiteSpace($script:stagingRemote)) {
        try {
            Invoke-AdbCapture -Arguments @("shell", "rm", "-rf", $script:stagingRemote) | Out-Null
        } catch { }
    }
    Write-Host ""
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "The active Modern folder was not replaced before a complete verified copy was ready."
    Write-Host "Diagnostic log: $LogPath"
    Stop-DiagnosticLog
    exit 1
}
