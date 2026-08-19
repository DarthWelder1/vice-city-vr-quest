import java.util.Properties
import org.gradle.api.DefaultTask
import org.gradle.api.file.DirectoryProperty
import org.gradle.api.file.RegularFileProperty
import org.gradle.api.tasks.InputFile
import org.gradle.api.tasks.OutputDirectory
import org.gradle.api.tasks.PathSensitive
import org.gradle.api.tasks.PathSensitivity
import org.gradle.api.tasks.TaskAction

abstract class StageNdkSharedStl : DefaultTask() {
	@get:InputFile
	@get:PathSensitive(PathSensitivity.NONE)
	abstract val sharedStl: RegularFileProperty

	@get:OutputDirectory
	abstract val outputDirectory: DirectoryProperty

	@TaskAction
	fun stage() {
		project.copy {
			from(sharedStl)
			into(outputDirectory.dir("arm64-v8a"))
		}
	}
}

plugins {
	id("com.android.application")
}

val questNdkVersion = "27.2.12479018"

// Optional out-of-tree build location.  This is useful on Windows machines
// where antivirus/indexing software transiently locks generated AGP jars in
// the source tree.  Normal builds keep Gradle's default app/build directory.
System.getenv("MIAMIVR_ANDROID_BUILD_DIR")
	?.takeIf { it.isNotBlank() }
	?.let { layout.buildDirectory.set(file(it)) }

val releaseSigningPropertiesFile = rootProject.file("release-signing.properties")
val releaseSigningProperties = Properties()
if (releaseSigningPropertiesFile.isFile) {
	releaseSigningPropertiesFile.inputStream().use {
		releaseSigningProperties.load(it)
	}
}

val releaseSigningKeys = listOf(
	"storeFile",
	"storePassword",
	"keyAlias",
	"keyPassword"
)
val hasReleaseSigning = releaseSigningKeys.all {
	!releaseSigningProperties.getProperty(it).isNullOrBlank()
}
val releaseBuildRequested = gradle.startParameter.taskNames.any {
	it.contains("Release", ignoreCase = true)
}
if (releaseBuildRequested && !hasReleaseSigning) {
	throw GradleException(
		"Release signing is not configured. Copy " +
			"release-signing.properties.example to release-signing.properties " +
			"and provide the private Quest release keystore values."
	)
}

android {
	namespace = "com.miamivr.quest"
	compileSdk = 35
	ndkVersion = questNdkVersion

	defaultConfig {
		applicationId = "com.miamivr.quest"
		// Quest 3 / 3S ship Android 12L or newer. Nothing below that is a target.
		minSdk = 32
		targetSdk = 35
		versionCode = 502
		versionName = "0.5.2-rc5-quest-alpha"

		ndk {
			abiFilters += "arm64-v8a"
		}

		externalNativeBuild {
			cmake {
				arguments += listOf(
					// Passed explicitly: CMake's option() caches its value, so
					// editing the default in CMakeLists.txt does not change an
					// already-configured build tree.
					"-DMIAMIVR_BRINGUP=OFF",
					// The gradle debug build type configures CMake as Debug,
					// which compiles at -O0. The game then holds ~60 of the
					// 72 Hz the headset wants and every miss reads as the
					// world lurching forward. Optimised code with debug info
					// is what this stage actually needs.
					"-DCMAKE_BUILD_TYPE=RelWithDebInfo",
					// ASan found the palette double-free and is done. Off: it
					// costs half the CPU and gigabytes of shadow memory, and
					// the device rebooted under combined load once.
					"-DMIAMIVR_ASAN=OFF",
					"-DANDROID_STL=c++_shared",
					// reVC is 2003-era C++ read through a 2024 clang. The
					// original code relies on things clang now diagnoses by
					// default; treating them as errors would stop the port
					// before any of it can be evaluated on the device.
					"-DANDROID_CPP_FEATURES=exceptions rtti"
				)
				cppFlags += listOf("-std=c++17")
			}
		}
	}

	buildFeatures {
		prefab = true
	}

	externalNativeBuild {
		cmake {
			path = file("src/main/cpp/CMakeLists.txt")
			version = "3.22.1"
		}
	}

	packaging {
		jniLibs {
			// Kept true: switching to embedded libs on top of an existing
			// install fails with INSTALL_FAILED_INVALID_APK, and uninstalling
			// would wipe the staged game data. Revisit on a clean device.
			useLegacyPackaging = true
			// Prefab also contributes this library. Keep exactly one copy even
			// when a clean/incremental build resolves both inputs.
			pickFirsts += "**/libopenxr_loader.so"
			// A normal externalNativeBuild also contributes this STL. The
			// generated jniLibs source below deliberately contributes a second
			// copy so cached/reused libmiamivr.so builds cannot produce an APK
			// without its runtime dependency.
			pickFirsts += "**/libc++_shared.so"
			// ASan is intentionally disabled for public builds.  The runtime
			// remains in the source tree for dedicated diagnostics, but must
			// not be shipped in a normal SideQuest APK.
			excludes += "**/libclang_rt.asan-*-android.so"
		}
		resources {
			excludes += "lib/*/wrap.sh"
		}
	}

	buildTypes {
		debug {
			isJniDebuggable = true
			isMinifyEnabled = false
		}
		release {
			isMinifyEnabled = false
			if (hasReleaseSigning) {
				signingConfig = signingConfigs.create("questRelease") {
					storeFile = rootProject.file(
						releaseSigningProperties.getProperty("storeFile")
					)
					storePassword = releaseSigningProperties.getProperty(
						"storePassword"
					)
					keyAlias = releaseSigningProperties.getProperty("keyAlias")
					keyPassword = releaseSigningProperties.getProperty(
						"keyPassword"
					)
				}
			}
		}
	}

	compileOptions {
		sourceCompatibility = JavaVersion.VERSION_17
		targetCompatibility = JavaVersion.VERSION_17
	}
}

// Do not rely on externalNativeBuild to package the shared C++ runtime. AGP can
// legitimately reuse a fresh libmiamivr.so while skipping buildCMake, and its
// implicit libc++_shared.so contribution then disappears. Registering the NDK
// runtime as a generated jniLibs source makes it an explicit input of every APK
// variant, including a release assembled entirely from Gradle's build cache.
val ndkPrebuiltHost = when {
	System.getProperty("os.name").startsWith("Windows", ignoreCase = true) ->
		"windows-x86_64"
	System.getProperty("os.name").startsWith("Mac", ignoreCase = true) ->
		"darwin-x86_64"
	else -> "linux-x86_64"
}

androidComponents {
	onVariants(selector().all()) { variant ->
		val taskSuffix = variant.name.replaceFirstChar {
			if (it.isLowerCase()) it.titlecase() else it.toString()
		}
		val stageSharedStl = tasks.register<StageNdkSharedStl>(
			"stage${taskSuffix}NdkSharedStl"
		) {
			sharedStl.set(
				sdkComponents.sdkDirectory.map { sdkDirectory ->
					sdkDirectory.file(
						"ndk/$questNdkVersion/toolchains/llvm/prebuilt/$ndkPrebuiltHost/" +
							"sysroot/usr/lib/aarch64-linux-android/libc++_shared.so"
					)
				}
			)
			outputDirectory.set(
				layout.buildDirectory.dir("generated/miamivr-jniLibs/${variant.name}")
			)
		}
		variant.sources.jniLibs?.addGeneratedSourceDirectory(
			stageSharedStl,
			StageNdkSharedStl::outputDirectory
		)
	}
}

dependencies {
	// Khronos OpenXR loader for Android. Ships the arm64 loader plus a prefab
	// package, so the runtime on the headset is discovered through the standard
	// Android broker rather than a vendor SDK.
	implementation("org.khronos.openxr:openxr_loader_for_android:1.1.43")
}

// Keep javac out of the Gradle daemon.  On Windows/JDK 21 the in-process
// compiler can leave AGP's generated R.jar mounted through ZipFS and then fail
// the otherwise successful task with AccessDeniedException while closing it.
// A short-lived compiler process releases the archive deterministically.
tasks.withType<org.gradle.api.tasks.compile.JavaCompile>().configureEach {
	options.isFork = true
}
