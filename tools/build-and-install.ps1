# Builds and optionally installs a personal Vice City VR Quest APK.
# No APK, reVC source, retail data, saves, or third-party model pack is
# distributed by this repository. The user supplies/downloads each input.
[CmdletBinding()]
param(
    [string]$GameDir,
    [string]$WorkDir = "C:\VCVRBuild",
    [string]$AndroidSdk,
    [string]$JavaHome,
    [string]$Serial,
    [string]$LogPath = (Join-Path $env:TEMP "ViceCityVR-Build-And-Install.log"),
    [switch]$BuildOnly,
    [switch]$SkipGameData,
    [switch]$NonInteractive
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$testedRevcCommit = "06d3ca5a7cce0021b84e6b7e1320a4f4e0ad3c87"
$revcUrl = "https://github.com/dubrovskiy-yevhen-stakelogic/re3-miami-vr.git"
$jdkVersion = "21.0.11+10"
$jdkUrl = "https://github.com/adoptium/temurin21-binaries/releases/download/jdk-21.0.11%2B10/OpenJDK21U-jdk_x64_windows_hotspot_21.0.11_10.zip"
$jdkSha256 = "D3625E7CADF23787EA540229544B6E2AB494B3B54DA1801879E583E1DFEE0A64"
$gradleVersion = "8.13"
$gradleUrl = "https://services.gradle.org/distributions/gradle-$gradleVersion-bin.zip"
$gradleSha256 = "20F1B1176237254A6FC204D8434196FA11A4CFB387567519C61556E8710AED78"
$ndkVersion = "27.2.12479018"
$cmakeVersion = "3.22.1"
$remoteGameData = "/sdcard/Android/data/com.miamivr.quest/files/gamedata"
$saveProviderUri = "content://com.miamivr.quest.saves/slot/1"
$repoRoot = Split-Path $PSScriptRoot -Parent
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

function Write-Step {
    param([int]$Number, [string]$Text)
    Write-Host ""
    Write-Host "[$Number/8] $Text" -ForegroundColor Cyan
}

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [string]$FailureMessage = "Command failed"
    )
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$FailureMessage (exit code $LASTEXITCODE)."
    }
}

function Find-JavaHome {
    $candidates = [System.Collections.Generic.List[string]]::new()
    if (-not [string]::IsNullOrWhiteSpace($JavaHome)) { $candidates.Add($JavaHome) }
    if (-not [string]::IsNullOrWhiteSpace($env:JAVA_HOME)) { $candidates.Add($env:JAVA_HOME) }
    $candidates.Add("C:\Program Files\Android\Android Studio\jbr")
    $candidates.Add((Join-Path $env:LOCALAPPDATA "Programs\Android Studio\jbr"))

    foreach ($candidate in $candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate)) { continue }
        $java = Join-Path $candidate "bin\java.exe"
        if (-not (Test-Path -LiteralPath $java -PathType Leaf)) { continue }
        # java.exe deliberately writes its version banner to stderr. Windows
        # PowerShell 5.1 turns that into an ErrorRecord under our strict global
        # preference even though java exits successfully, so inspect it under
        # Continue and restore the caller's policy immediately afterward.
        $savedErrorPreference = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        try {
            $versionText = (& $java -version 2>&1 | Out-String)
        } finally {
            $ErrorActionPreference = $savedErrorPreference
        }
        if ($versionText -match 'version "21(?:\.|\")') {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    # Android Studio installations do not always expose their bundled JBR in
    # a standard location. Keep the one-click path deterministic by fetching a
    # pinned official Eclipse Temurin JDK when no suitable local Java exists.
    $javaWorkRoot = [IO.Path]::GetFullPath($WorkDir)
    $javaDownloads = Join-Path $javaWorkRoot ".downloads"
    $javaTools = Join-Path $javaWorkRoot ".tools"
    $javaZip = Join-Path $javaDownloads "OpenJDK21U-jdk_x64_windows_hotspot_21.0.11_10.zip"
    $downloadedJavaHome = Join-Path $javaTools "jdk-$jdkVersion"
    $downloadedJava = Join-Path $downloadedJavaHome "bin\java.exe"
    New-Item -ItemType Directory -Path $javaDownloads,$javaTools -Force | Out-Null

    if (-not (Test-Path -LiteralPath $downloadedJava -PathType Leaf)) {
        if (-not (Test-Path -LiteralPath $javaZip -PathType Leaf)) {
            Write-Host "JDK 21 was not found; downloading official Eclipse Temurin $jdkVersion..." -ForegroundColor Yellow
            Invoke-WebRequest -UseBasicParsing -Uri $jdkUrl -OutFile $javaZip
        }
        $observedHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $javaZip).Hash
        if ($observedHash -ne $jdkSha256) {
            throw "Temurin JDK archive hash mismatch. Expected $jdkSha256, got $observedHash. Delete $javaZip and retry."
        }
        Expand-Archive -LiteralPath $javaZip -DestinationPath $javaTools -Force
    }
    if (-not (Test-Path -LiteralPath $downloadedJava -PathType Leaf)) {
        throw "Temurin JDK extraction did not create $downloadedJava"
    }
    return $downloadedJavaHome
}

function Find-AndroidSdk {
    $candidates = [System.Collections.Generic.List[string]]::new()
    if (-not [string]::IsNullOrWhiteSpace($AndroidSdk)) { $candidates.Add($AndroidSdk) }
    if (-not [string]::IsNullOrWhiteSpace($env:ANDROID_HOME)) { $candidates.Add($env:ANDROID_HOME) }
    if (-not [string]::IsNullOrWhiteSpace($env:ANDROID_SDK_ROOT)) { $candidates.Add($env:ANDROID_SDK_ROOT) }
    $candidates.Add((Join-Path $env:LOCALAPPDATA "Android\Sdk"))

    foreach ($candidate in $candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate)) { continue }
        if (Test-Path -LiteralPath (Join-Path $candidate "platform-tools\adb.exe") -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw @"
Android SDK/Platform-Tools were not found. In Android Studio open:
  More Actions > SDK Manager
Install Android SDK Platform 35 and, under SDK Tools:
  Android SDK Build-Tools 35.0.0
  Android SDK Platform-Tools
  Android SDK Command-line Tools (latest)
  NDK (Side by side) 27.2.12479018
  CMake 3.22.1
Then rerun BUILD_AND_INSTALL.bat.
"@
}

function Get-MissingSdkPackages {
    param([string]$SdkRoot)
    $requirements = @(
        [PSCustomObject]@{ Path = "platforms\android-35\android.jar"; Package = "platforms;android-35" },
        [PSCustomObject]@{ Path = "build-tools\35.0.0"; Package = "build-tools;35.0.0" },
        [PSCustomObject]@{ Path = "platform-tools\adb.exe"; Package = "platform-tools" },
        [PSCustomObject]@{ Path = "ndk\$ndkVersion"; Package = "ndk;$ndkVersion" },
        [PSCustomObject]@{ Path = "cmake\$cmakeVersion"; Package = "cmake;$cmakeVersion" }
    )
    return @($requirements | Where-Object {
        -not (Test-Path -LiteralPath (Join-Path $SdkRoot $_.Path))
    })
}

function Ensure-SdkPackages {
    param([string]$SdkRoot)
    $missing = @(Get-MissingSdkPackages -SdkRoot $SdkRoot)
    if ($missing.Count -eq 0) { return }

    Write-Host "Missing Android SDK components:" -ForegroundColor Yellow
    $missing | ForEach-Object { Write-Host "  $($_.Package)" }
    $sdkManagerCandidates = @(
        (Join-Path $SdkRoot "cmdline-tools\latest\bin\sdkmanager.bat"),
        (Join-Path $SdkRoot "cmdline-tools\bin\sdkmanager.bat")
    )
    $sdkManager = $sdkManagerCandidates | Where-Object {
        Test-Path -LiteralPath $_ -PathType Leaf
    } | Select-Object -First 1

    if ($null -eq $sdkManager) {
        throw "Install the listed components in Android Studio SDK Manager, including Android SDK Command-line Tools (latest), then rerun."
    }
    if ($NonInteractive) {
        throw "Required Android SDK components are missing; automatic installation needs an interactive license confirmation."
    }
    $answer = Read-Host "Install the missing SDK components now? You may need to accept Google licenses [Y/n]"
    if (-not [string]::IsNullOrWhiteSpace($answer) -and $answer -notmatch '^[Yy]') {
        throw "Required SDK components were not installed."
    }
    & $sdkManager --licenses
    if ($LASTEXITCODE -ne 0) { throw "Android SDK license step failed." }
    Invoke-Checked -FilePath $sdkManager -Arguments @($missing.Package) `
        -FailureMessage "Android SDK component installation failed"
    $stillMissing = @(Get-MissingSdkPackages -SdkRoot $SdkRoot)
    if ($stillMissing.Count -ne 0) {
        throw "Android SDK Manager completed, but required components are still missing: $($stillMissing.Package -join ', ')"
    }
}

function Get-AdbArguments {
    param([string[]]$Arguments)
    if ([string]::IsNullOrWhiteSpace($Serial)) { return $Arguments }
    return @("-s", $Serial) + $Arguments
}

function Invoke-AdbChecked {
    param([string[]]$Arguments, [string]$FailureMessage = "ADB command failed")
    Invoke-Checked -FilePath $script:adb -Arguments (Get-AdbArguments $Arguments) `
        -FailureMessage $FailureMessage
}

function Select-QuestDevice {
    $output = & $script:adb devices -l
    if ($LASTEXITCODE -ne 0) { throw "adb devices failed." }
    $devices = @()
    foreach ($line in $output) {
        if ($line -match '^(\S+)\s+device(?:\s|$)') { $devices += $Matches[1] }
    }
    if (-not [string]::IsNullOrWhiteSpace($Serial)) {
        if ($devices -notcontains $Serial) {
            throw "Quest '$Serial' is not connected and authorized. Check the USB debugging prompt inside the headset."
        }
        return
    }
    if ($devices.Count -eq 0) {
        throw "No authorized Android device found. Connect the Quest, enable USB debugging, and accept the prompt inside the headset."
    }
    if ($devices.Count -eq 1) {
        $script:Serial = $devices[0]
        return
    }
    if ($NonInteractive) {
        throw "More than one Android device is connected. Rerun with -Serial DEVICE_SERIAL."
    }
    Write-Host "Connected devices:"
    for ($index = 0; $index -lt $devices.Count; $index++) {
        Write-Host "  $($index + 1). $($devices[$index])"
    }
    $selection = Read-Host "Select the Quest device number"
    $parsed = 0
    if (-not [int]::TryParse($selection, [ref]$parsed) -or
        $parsed -lt 1 -or $parsed -gt $devices.Count) {
        throw "Invalid device selection."
    }
    $script:Serial = $devices[$parsed - 1]
}

function Resolve-GameFolder {
    param([string]$Requested)
    if ([string]::IsNullOrWhiteSpace($Requested)) {
        if ($NonInteractive) { throw "-GameDir is required unless -SkipGameData or -BuildOnly is used." }
        Write-Host ""
        Write-Host "Enter your legally owned GTA Vice City PC installation folder." -ForegroundColor Yellow
        Write-Host "Press Enter to skip copying data if it is already installed on the Quest."
        $Requested = Read-Host "Vice City folder"
        if ([string]::IsNullOrWhiteSpace($Requested)) { return $null }
    }
    if (-not (Test-Path -LiteralPath $Requested -PathType Container)) {
        throw "Vice City folder does not exist: $Requested"
    }
    return (Resolve-Path -LiteralPath $Requested).Path
}

function Find-ChildDirectory {
    param([string]$Parent, [string]$Name)
    return Get-ChildItem -LiteralPath $Parent -Directory | Where-Object {
        $_.Name.Equals($Name, [StringComparison]::OrdinalIgnoreCase)
    } | Select-Object -First 1
}

try {
    Write-Host "Vice City VR v0.5.1 - personal Quest build wizard" -ForegroundColor Green
    Write-Host "This repository supplies source/build tooling only; no APK or GTA data is bundled."

    Write-Step 1 "Checking Git, JDK 21 and the Android SDK"
    $gitCommand = Get-Command git.exe -ErrorAction SilentlyContinue
    if ($null -eq $gitCommand) { throw "Git was not found. Install Git for Windows, then rerun." }
    $resolvedJavaHome = Find-JavaHome
    $resolvedSdk = Find-AndroidSdk
    Ensure-SdkPackages -SdkRoot $resolvedSdk
    $env:JAVA_HOME = $resolvedJavaHome
    $env:ANDROID_HOME = $resolvedSdk
    $env:ANDROID_SDK_ROOT = $resolvedSdk
    $script:adb = Join-Path $resolvedSdk "platform-tools\adb.exe"
    Write-Host "JDK: $resolvedJavaHome"
    Write-Host "Android SDK: $resolvedSdk"

    Write-Step 2 "Preparing the short local build directory"
    $resolvedWork = [IO.Path]::GetFullPath($WorkDir)
    New-Item -ItemType Directory -Path $resolvedWork -Force | Out-Null
    if ($resolvedWork.Length -gt 80) {
        Write-Host "Warning: '$resolvedWork' is long; C:\VCVRBuild is safer for native builds." -ForegroundColor Yellow
    }

    Write-Step 3 "Obtaining the exact tested reVC source"
    $revcDir = Join-Path $resolvedWork "reVC-base"
    if (-not (Test-Path -LiteralPath $revcDir)) {
        Invoke-Checked -FilePath $gitCommand.Source -Arguments @(
            "clone", "--recursive", "-b", "dev", $revcUrl, $revcDir
        ) -FailureMessage "reVC clone failed"
        Invoke-Checked -FilePath $gitCommand.Source -Arguments @(
            "-C", $revcDir, "checkout", "--detach", $testedRevcCommit
        ) -FailureMessage "Could not select the tested reVC commit"
        Invoke-Checked -FilePath $gitCommand.Source -Arguments @(
            "-C", $revcDir, "submodule", "update", "--init", "--recursive"
        ) -FailureMessage "reVC submodule setup failed"
    } else {
        if (-not (Test-Path -LiteralPath (Join-Path $revcDir ".git"))) {
            throw "$revcDir exists but is not the wizard's reVC checkout. Choose another -WorkDir."
        }
        $savedErrorPreference = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        try {
            $headOutput = & $gitCommand.Source -C $revcDir rev-parse HEAD 2>&1
            $headExit = $LASTEXITCODE
            $dirtyOutput = & $gitCommand.Source -C $revcDir status --porcelain 2>&1
            $dirtyExit = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $savedErrorPreference
        }
        if ($headExit -ne 0 -or $dirtyExit -ne 0) {
            throw "Could not verify the existing reVC checkout at $revcDir. Git said: $($headOutput | Out-String) $($dirtyOutput | Out-String)"
        }
        $head = ($headOutput | Out-String).Trim()
        $dirty = ($dirtyOutput | Out-String).Trim()
        if ($head -ne $testedRevcCommit -or $dirty.Length -ne 0) {
            throw "$revcDir is not a clean checkout of $testedRevcCommit. Preserve your work and choose another -WorkDir."
        }
        Invoke-Checked -FilePath $gitCommand.Source -Arguments @(
            "-C", $revcDir, "submodule", "update", "--init", "--recursive"
        ) -FailureMessage "reVC submodule verification failed"
        Write-Host "Reusing verified clean reVC source."
    }

    Write-Step 4 "Assembling the private Quest source tree"
    $buildName = "vice-city-vr-build"
    $assembledDir = Join-Path $resolvedWork $buildName
    if (Test-Path -LiteralPath $assembledDir) {
        $assembledDir = Join-Path $resolvedWork ($buildName + "-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
        Write-Host "Previous build preserved; using $assembledDir"
    }
    & (Join-Path $PSScriptRoot "assemble.ps1") -Revc $revcDir -Out $assembledDir
    if (-not (Test-Path -LiteralPath (Join-Path $assembledDir "android"))) {
        throw "Quest source assembly failed."
    }

    Write-Step 5 "Preparing verified Gradle $gradleVersion"
    $downloadsDir = Join-Path $resolvedWork ".downloads"
    $toolsDir = Join-Path $resolvedWork ".tools"
    $gradleZip = Join-Path $downloadsDir "gradle-$gradleVersion-bin.zip"
    $gradleHome = Join-Path $toolsDir "gradle-$gradleVersion"
    $gradle = Join-Path $gradleHome "bin\gradle.bat"
    New-Item -ItemType Directory -Path $downloadsDir,$toolsDir -Force | Out-Null
    if (-not (Test-Path -LiteralPath $gradle -PathType Leaf)) {
        if (-not (Test-Path -LiteralPath $gradleZip -PathType Leaf)) {
            Write-Host "Downloading Gradle $gradleVersion from services.gradle.org..."
            Invoke-WebRequest -UseBasicParsing -Uri $gradleUrl -OutFile $gradleZip
        }
        $observedHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $gradleZip).Hash
        if ($observedHash -ne $gradleSha256) {
            throw "Gradle archive hash mismatch. Expected $gradleSha256, got $observedHash. Delete $gradleZip and retry."
        }
        Expand-Archive -LiteralPath $gradleZip -DestinationPath $toolsDir -Force
    }
    if (-not (Test-Path -LiteralPath $gradle -PathType Leaf)) {
        throw "Gradle extraction did not create $gradle"
    }

    Write-Step 6 "Building the personal debug-signed APK"
    Push-Location (Join-Path $assembledDir "android")
    try {
        Invoke-Checked -FilePath $gradle -Arguments @(
            ":app:assembleDebug", "--no-daemon"
        ) -FailureMessage "Android/ARM64 build failed"
    } finally {
        Pop-Location
    }
    $apk = Join-Path $assembledDir "android\app\build\outputs\apk\debug\app-debug.apk"
    if (-not (Test-Path -LiteralPath $apk -PathType Leaf)) {
        throw "Build completed without the expected APK: $apk"
    }
    $apkHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $apk).Hash
    Write-Host "APK: $apk" -ForegroundColor Green
    Write-Host "SHA256: $apkHash"

    if ($BuildOnly) {
        Write-Host "Build-only mode complete." -ForegroundColor Green
        Write-Host "Diagnostic log: $LogPath"
        Stop-DiagnosticLog
        return
    }

    Write-Step 7 "Installing on the connected Quest without clearing data"
    Select-QuestDevice
    Write-Host "Quest: $Serial"
    Invoke-AdbChecked -Arguments @("install", "-r", $apk) `
        -FailureMessage "APK installation failed"
    $providerArguments = Get-AdbArguments @(
        "shell", "content", "query", "--uri", $saveProviderUri,
        "--projection", "_display_name:_size"
    )
    $providerOutput = & $script:adb @providerArguments
    if ($LASTEXITCODE -ne 0 -or ($providerOutput | Out-String) -notmatch '_display_name=GTAVCsf1\.b') {
        throw "The APK installed, but the safe external-data bootstrap failed. Output: $($providerOutput | Out-String)"
    }
    Write-Host "Application storage bootstrap verified."

    Write-Step 8 "Copying the user's game data and bundled VR hands"
    if (-not $SkipGameData) {
        $resolvedGame = Resolve-GameFolder -Requested $GameDir
        if ($null -ne $resolvedGame) {
            $required = @("data", "TEXT", "anim", "txd", "skins", "mp3", "movies", "models", "Audio")
            $resolvedFolders = @{}
            foreach ($name in $required) {
                $folder = Find-ChildDirectory -Parent $resolvedGame -Name $name
                if ($null -eq $folder) {
                    throw "Required game-data folder '$name' is missing from $resolvedGame. Use a complete PC installation."
                }
                $resolvedFolders[$name] = $folder
            }
            foreach ($name in $required) {
                $folder = $resolvedFolders[$name]
                Write-Host "Copying $($folder.Name)..." -ForegroundColor Cyan
                Invoke-AdbChecked -Arguments @("push", $folder.FullName, $remoteGameData) `
                    -FailureMessage "Failed to copy $($folder.FullName)"
            }
        } else {
            Write-Host "Game-data copy skipped. The app requires existing data under $remoteGameData." -ForegroundColor Yellow
        }
    } else {
        Write-Host "Game-data copy skipped by -SkipGameData."
    }

    $handsSource = Join-Path $assembledDir "gamefiles\models\vrhands"
    $handsRemote = "$remoteGameData/models/vrhands"
    $handFiles = @("BigHandLeft.uxrh", "BigHandRight.uxrh", "BigHandsAlbedo.png")
    foreach ($name in $handFiles) {
        if (-not (Test-Path -LiteralPath (Join-Path $handsSource $name) -PathType Leaf)) {
            throw "Bundled VR hand file is missing: $name"
        }
    }
    Invoke-AdbChecked -Arguments @("shell", "mkdir", "-p", $handsRemote) `
        -FailureMessage "Could not create the VR hand data directory"
    foreach ($name in $handFiles) {
        Invoke-AdbChecked -Arguments @(
            "push", (Join-Path $handsSource $name), "$handsRemote/$name"
        ) -FailureMessage "Failed to copy VR hand asset $name"
    }
    Invoke-AdbChecked -Arguments @("shell", "am", "force-stop", "com.miamivr.quest") `
        -FailureMessage "Could not leave the app stopped after installation"

    Write-Host ""
    Write-Host "VICE CITY VR IS READY." -ForegroundColor Green
    Write-Host "The app was left stopped. Put on the headset and launch Vice City VR."
    Write-Host "Build directory: $assembledDir"
    Write-Host "APK SHA256: $apkHash"
    Write-Host "Diagnostic log: $LogPath"
    Stop-DiagnosticLog
} catch {
    Write-Host ""
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "No failure was treated as success. Fix the reported item and rerun."
    Write-Host "Diagnostic log: $LogPath"
    Stop-DiagnosticLog
    exit 1
}
