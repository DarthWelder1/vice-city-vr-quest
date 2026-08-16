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

$testedRevcCommit = "026cd10f3fdbd92c089830e5067c4457c53c1b51"
$revcUrl = "https://github.com/mrxenginner/reVC.git"
$revcBranch = "miami"
$gitVersion = "2.55.0.3"
$gitUrl = "https://github.com/git-for-windows/git/releases/download/v2.55.0.windows.3/MinGit-2.55.0.3-64-bit.zip"
$gitSha256 = "F48E2D2DC74A24454ADC6D8FD0AC25BF9C2386F19CFB06202B9465AAAD4F9F05"
$androidCommandLineToolsVersion = "15859902"
$androidCommandLineToolsUrl = "https://dl.google.com/android/repository/commandlinetools-win-15859902_latest.zip"
$androidCommandLineToolsSha256 = "90AE805D20434428BFFCB699C290860F19BB5F66A67E6B330067E3DE801FB04A"
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

function Find-Git {
    $installed = Get-Command git.exe -ErrorAction SilentlyContinue
    if ($null -ne $installed) { return $installed.Source }

    $gitWorkRoot = [IO.Path]::GetFullPath($WorkDir)
    $gitDownloads = Join-Path $gitWorkRoot ".downloads"
    $gitTools = Join-Path $gitWorkRoot ".tools"
    $gitZip = Join-Path $gitDownloads "MinGit-$gitVersion-64-bit.zip"
    $gitHome = Join-Path $gitTools "MinGit-$gitVersion"
    $gitExe = Join-Path $gitHome "cmd\git.exe"
    New-Item -ItemType Directory -Path $gitDownloads,$gitTools -Force | Out-Null

    if (-not (Test-Path -LiteralPath $gitExe -PathType Leaf)) {
        if (-not (Test-Path -LiteralPath $gitZip -PathType Leaf)) {
            Write-Host "Git was not found; downloading official portable MinGit $gitVersion..." -ForegroundColor Yellow
            Invoke-WebRequest -UseBasicParsing -Uri $gitUrl -OutFile $gitZip
        }
        $observedHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $gitZip).Hash
        if ($observedHash -ne $gitSha256) {
            throw "MinGit archive hash mismatch. Expected $gitSha256, got $observedHash. Delete $gitZip and retry."
        }
        Expand-Archive -LiteralPath $gitZip -DestinationPath $gitHome -Force
    }
    if (-not (Test-Path -LiteralPath $gitExe -PathType Leaf)) {
        throw "Portable MinGit extraction did not create $gitExe"
    }
    return $gitExe
}

function Ensure-AndroidCommandLineTools {
    param([Parameter(Mandatory = $true)][string]$SdkRoot)

    $managerCandidates = @(
        (Join-Path $SdkRoot "cmdline-tools\latest\bin\sdkmanager.bat"),
        (Join-Path $SdkRoot "cmdline-tools\bin\sdkmanager.bat")
    )
    $existingManager = $managerCandidates | Where-Object {
        Test-Path -LiteralPath $_ -PathType Leaf
    } | Select-Object -First 1
    if ($null -ne $existingManager) { return $existingManager }

    $sdkWorkRoot = [IO.Path]::GetFullPath($WorkDir)
    $sdkDownloads = Join-Path $sdkWorkRoot ".downloads"
    $sdkTools = Join-Path $sdkWorkRoot ".tools"
    $toolsZip = Join-Path $sdkDownloads "commandlinetools-win-$androidCommandLineToolsVersion.zip"
    $extractRoot = Join-Path $sdkTools "android-command-line-tools-$androidCommandLineToolsVersion"
    $latestRoot = Join-Path $sdkRoot "cmdline-tools\latest"
    $sdkManager = Join-Path $latestRoot "bin\sdkmanager.bat"
    New-Item -ItemType Directory -Path $sdkDownloads,$sdkTools,$SdkRoot -Force | Out-Null

    if (-not (Test-Path -LiteralPath $sdkManager -PathType Leaf)) {
        if (-not (Test-Path -LiteralPath $toolsZip -PathType Leaf)) {
            Write-Host "Android SDK command-line tools are missing; downloading them from Google..." -ForegroundColor Yellow
            Invoke-WebRequest -UseBasicParsing -Uri $androidCommandLineToolsUrl -OutFile $toolsZip
        }
        $observedHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $toolsZip).Hash
        if ($observedHash -ne $androidCommandLineToolsSha256) {
            throw "Android command-line tools hash mismatch. Expected $androidCommandLineToolsSha256, got $observedHash. Delete $toolsZip and retry."
        }
        Expand-Archive -LiteralPath $toolsZip -DestinationPath $extractRoot -Force
        $extractedTools = Join-Path $extractRoot "cmdline-tools"
        if (-not (Test-Path -LiteralPath (Join-Path $extractedTools "bin\sdkmanager.bat") -PathType Leaf)) {
            throw "Android command-line tools archive has an unexpected layout."
        }
        New-Item -ItemType Directory -Path $latestRoot -Force | Out-Null
        Copy-Item -Path (Join-Path $extractedTools "*") -Destination $latestRoot -Recurse -Force
    }
    if (-not (Test-Path -LiteralPath $sdkManager -PathType Leaf)) {
        throw "Android SDK bootstrap did not create $sdkManager"
    }
    return $sdkManager
}

function Find-AndroidSdk {
    $candidates = [System.Collections.Generic.List[string]]::new()
    if (-not [string]::IsNullOrWhiteSpace($AndroidSdk)) { $candidates.Add($AndroidSdk) }
    if (-not [string]::IsNullOrWhiteSpace($env:ANDROID_HOME)) { $candidates.Add($env:ANDROID_HOME) }
    if (-not [string]::IsNullOrWhiteSpace($env:ANDROID_SDK_ROOT)) { $candidates.Add($env:ANDROID_SDK_ROOT) }
    $candidates.Add((Join-Path $env:LOCALAPPDATA "Android\Sdk"))

    foreach ($candidate in $candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate)) { continue }
        $hasAdb = Test-Path -LiteralPath (Join-Path $candidate "platform-tools\adb.exe") -PathType Leaf
        $hasManager = (Test-Path -LiteralPath (Join-Path $candidate "cmdline-tools\latest\bin\sdkmanager.bat") -PathType Leaf) -or
            (Test-Path -LiteralPath (Join-Path $candidate "cmdline-tools\bin\sdkmanager.bat") -PathType Leaf)
        if ($hasAdb -or $hasManager) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    $sdkRoot = Join-Path ([IO.Path]::GetFullPath($WorkDir)) ".android-sdk"
    New-Item -ItemType Directory -Path $sdkRoot -Force | Out-Null
    Ensure-AndroidCommandLineTools -SdkRoot $sdkRoot | Out-Null
    return $sdkRoot
}

function Get-MissingSdkPackages {
    param([string]$SdkRoot)
    $requirements = @(
        [PSCustomObject]@{ Path = "platforms\android-35\android.jar"; Package = "platforms;android-35" },
        [PSCustomObject]@{ Path = "build-tools\34.0.0"; Package = "build-tools;34.0.0" },
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
    $sdkManager = Ensure-AndroidCommandLineTools -SdkRoot $SdkRoot
    if ($NonInteractive) {
        throw "Required Android SDK components are missing; automatic installation needs an interactive license confirmation."
    }
    $answer = Read-Host "Install the missing SDK components now? You may need to accept Google licenses [Y/n]"
    if (-not [string]::IsNullOrWhiteSpace($answer) -and $answer -notmatch '^[Yy]') {
        throw "Required SDK components were not installed."
    }
    & $sdkManager "--sdk_root=$SdkRoot" --licenses
    if ($LASTEXITCODE -ne 0) { throw "Android SDK license step failed." }
    Invoke-Checked -FilePath $sdkManager -Arguments (@("--sdk_root=$SdkRoot") + @($missing.Package)) `
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

    Write-Step 1 "Preparing Git, JDK 21 and the Android SDK"
    $gitExe = Find-Git
    $gitCommandDirectory = Split-Path -Parent $gitExe
    if (($env:PATH -split ';') -notcontains $gitCommandDirectory) {
        $env:PATH = "$gitCommandDirectory;$env:PATH"
    }
    $resolvedJavaHome = Find-JavaHome
    $env:JAVA_HOME = $resolvedJavaHome
    $resolvedSdk = Find-AndroidSdk
    $env:ANDROID_HOME = $resolvedSdk
    $env:ANDROID_SDK_ROOT = $resolvedSdk
    Ensure-SdkPackages -SdkRoot $resolvedSdk
    $script:adb = Join-Path $resolvedSdk "platform-tools\adb.exe"
    Write-Host "Git: $gitExe"
    Write-Host "JDK: $resolvedJavaHome"
    Write-Host "Android SDK: $resolvedSdk"

    Write-Step 2 "Preparing the short local build directory"
    $resolvedWork = [IO.Path]::GetFullPath($WorkDir)
    New-Item -ItemType Directory -Path $resolvedWork -Force | Out-Null
    if ($resolvedWork.Length -gt 80) {
        Write-Host "Warning: '$resolvedWork' is long; C:\VCVRBuild is safer for native builds." -ForegroundColor Yellow
    }

    Write-Step 3 "Obtaining the exact tested reVC source"
    # A versioned public-base directory avoids reusing the private-fork checkout
    # created by the short-lived older wizard.
    $revcDir = Join-Path $resolvedWork "reVC-public-026cd10"
    $safeRevcDir = $revcDir.Replace('\', '/')
    if (-not (Test-Path -LiteralPath $revcDir)) {
        Invoke-Checked -FilePath $gitExe -Arguments @(
            "clone", "--no-checkout", "-b", $revcBranch, $revcUrl, $revcDir
        ) -FailureMessage "reVC clone failed"
        Invoke-Checked -FilePath $gitExe -Arguments @(
            "-c", "safe.directory=$safeRevcDir", "-C", $revcDir, "checkout", "--detach", $testedRevcCommit
        ) -FailureMessage "Could not select the tested reVC commit"
    } else {
        if (-not (Test-Path -LiteralPath (Join-Path $revcDir ".git"))) {
            throw "$revcDir exists but is not the wizard's reVC checkout. Choose another -WorkDir."
        }
        $savedErrorPreference = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        try {
            $headOutput = & $gitExe -c "safe.directory=$safeRevcDir" -C $revcDir rev-parse HEAD 2>&1
            $headExit = $LASTEXITCODE
            $dirtyOutput = & $gitExe -c "safe.directory=$safeRevcDir" -C $revcDir status --porcelain 2>&1
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
