plugins {
	id("com.android.application")
}

android {
	namespace = "com.miamivr.quest"
	compileSdk = 35
	ndkVersion = "27.2.12479018"

	defaultConfig {
		applicationId = "com.miamivr.quest"
		// Quest 3 / 3S ship Android 12L or newer. Nothing below that is a target.
		minSdk = 32
		targetSdk = 35
		versionCode = 1
		versionName = "0.1.0-quest-alpha"

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
		}
	}

	buildTypes {
		debug {
			isJniDebuggable = true
			isMinifyEnabled = false
		}
		release {
			isMinifyEnabled = false
			signingConfig = signingConfigs.getByName("debug")
		}
	}

	compileOptions {
		sourceCompatibility = JavaVersion.VERSION_17
		targetCompatibility = JavaVersion.VERSION_17
	}
}

dependencies {
	// Khronos OpenXR loader for Android. Ships the arm64 loader plus a prefab
	// package, so the runtime on the headset is discovered through the standard
	// Android broker rather than a vendor SDK.
	implementation("org.khronos.openxr:openxr_loader_for_android:1.1.43")
}
