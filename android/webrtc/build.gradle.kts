// M20: WebRTC (WHIP) receiver client app. Uses the WebRTC plugins bundled in
// the GStreamer Android prebuilts (webrtcbin + rswebrtc whipserversrc +
// nice/dtls/srtp). GStreamer is LGPL; the gst-plugins-rs (whipserversrc) is
// MPL-2.0 - both permissive, unlike the AirPlay APK's GPLv3.
plugins {
    id("com.android.application")
}

android {
    namespace = "dev.vkrender.webrtc"
    compileSdk = 36
    ndkVersion = "27.0.12077973"

    defaultConfig {
        applicationId = "dev.vkrender.webrtc"
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
