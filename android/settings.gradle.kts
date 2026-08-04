pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}
dependencyResolutionManagement {
    repositories {
        google()
        mavenCentral()
    }
}
rootProject.name = "vkrender"
// M10: two independent APKs — the renderer service app and the decoder
// client app — talking only through Binder (AIDL Surface handoff).
include(":renderer")
include(":decoder")
