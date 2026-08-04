plugins {
    id("com.android.application")
}

android {
    namespace = "dev.vkrender.decoder"
    compileSdk = 36
    ndkVersion = "27.0.12077973"

    defaultConfig {
        applicationId = "dev.vkrender.decoder"
        minSdk = 29
        targetSdk = 34
        versionCode = 1
        versionName = "0.1"
        ndk {
            abiFilters += listOf("x86_64", "arm64-v8a")
        }
    }

    // Six installable client packages from the same sources: the renderer
    // composites one stream per client app (Binder UID), one per cube face,
    // so the full demo needs six distinct packages (M11).
    flavorDimensions += "instance"
    productFlavors {
        create("primary") {
            dimension = "instance"
            // keeps applicationId dev.vkrender.decoder
        }
        for (n in 2..6) {
            create("client$n") {
                dimension = "instance"
                applicationId = "dev.vkrender.decoder$n"
            }
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
