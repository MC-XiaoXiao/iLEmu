// Keep the pinned SDL Java frontend in its own dependency module.
// Its native library is built by the app's NDK CMake project from the same tree.
plugins {
    alias(libs.plugins.android.library)
}

android {
    namespace = "org.libsdl.app"
    compileSdk { version = release(37) }
    defaultConfig { minSdk = 28 }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
}

androidComponents {
    onVariants(selector().all()) { variant ->
        variant.sources.java?.addStaticSourceDirectory(
            "../../external/sdl/android-project/app/src/main/java")
    }
}
