// M18: AirPlay receiver client app. NOTE: this module vendors the UxPlay
// mirroring core (GPLv3) - distributing THIS APK publicly triggers GPLv3
// source obligations for the app; the other vkrender APKs are unaffected
// (separate processes communicating over Binder).
plugins {
    id("com.android.application")
}

android {
    namespace = "dev.vkrender.airplay"
    compileSdk = 36
    ndkVersion = "27.0.12077973"

    defaultConfig {
        applicationId = "dev.vkrender.airplay"
        minSdk = 29
        targetSdk = 34
        versionCode = 1
        versionName = "0.1"
        ndk {
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
            arguments += "-DCMAKE_BUILD_TYPE=RelWithDebInfo"
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
