plugins {
    id("com.android.application")
}

android {
    namespace = "dev.vkrender.renderer"
    compileSdk = 36
    ndkVersion = "27.0.12077973"

    defaultConfig {
        applicationId = "dev.vkrender.renderer"
        minSdk = 29
        targetSdk = 34
        versionCode = 1
        versionName = "0.1"
        ndk {
            // x86_64 for the emulator; arm64 for devices.
            abiFilters += listOf("x86_64", "arm64-v8a")
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }
    defaultConfig.externalNativeBuild {
        cmake {
            // Optimized native code even in the debug APK: the -O0 render
            // loop misses vsync deadlines on slower devices (measured ~37Hz
            // present rate on a 60Hz panel -> judder).
            arguments += "-DCMAKE_BUILD_TYPE=RelWithDebInfo"
            providers.gradleProperty("framelinkDir").orNull?.let {
                arguments += "-DFRAMELINK_DIR=$it"
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }
    buildFeatures {
        aidl = true
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
