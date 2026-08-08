// M27: RTSP receiver client app. Pulls from IP cameras / encoders using the
// RTSP plugins in the GStreamer Android prebuilts (rtspsrc + rtp/rtpmanager/
// udp), then decodes H.264 with MediaCodec straight into a renderer slot
// Surface. All LGPL/permissive - unlike the AirPlay APK's GPLv3.
plugins {
    id("com.android.application")
}

android {
    namespace = "dev.vkrender.rtsp"
    compileSdk = 36
    ndkVersion = "27.0.12077973"

    defaultConfig {
        applicationId = "dev.vkrender.rtsp"
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
