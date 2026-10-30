plugins {
    alias(libs.plugins.android.application)
}

// Keep package and native versions tied to the CMake project declaration.
val emulatorVersion = requireNotNull(
    Regex("""(?m)^project\(iLEmu\s+VERSION\s+(\d+)\.(\d+)\.(\d+)\s""")
        .find(providers.fileContents(layout.projectDirectory.file("../../CMakeLists.txt")).asText.get())
) { "CMake must declare an iLEmu major.minor.patch version" }
    .groupValues.drop(1).map(String::toInt)
require(emulatorVersion[1] in 0..999 && emulatorVersion[2] in 0..999)
val emulatorVersionCode = emulatorVersion[0].toLong() * 1_000_000 +
    emulatorVersion[1] * 1_000 + emulatorVersion[2]
require(emulatorVersionCode in 1..2_100_000_000)

android {
    namespace = "com.xxiao.ilemu"
    compileSdk { version = release(37) }
    ndkVersion = "30.0.16248370"

    defaultConfig {
        applicationId = "com.xxiao.ilemu"
        minSdk = 28
        targetSdk = 37
        versionCode = emulatorVersionCode.toInt()
        versionName = emulatorVersion.joinToString(".")
        ndk { abiFilters += "arm64-v8a" }
        externalNativeBuild {
            cmake {
                arguments += listOf("-DCMAKE_BUILD_TYPE=RelWithDebInfo")
                targets += listOf("ilemu", "SDL2", "firmware_dmg", "firmware_decrypt", "firmware_hfstar")
            }
        }
    }
    buildTypes {
        release {
            optimization {
                enable = true
                packageScope = setOf("androidx.**", "kotlin.**", "kotlinx.**")
            }
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.31.6"
        }
    }
    packaging { jniLibs.useLegacyPackaging = true }
    buildFeatures {
        viewBinding = true
        buildConfig = true
    }
}

dependencies {
    implementation(project(":sdl"))
    implementation(libs.androidx.appcompat)
    implementation(libs.androidx.core.ktx)
    implementation(libs.material)
    implementation("androidx.documentfile:documentfile:1.0.1")
    implementation("com.liulishuo.okdownload:okdownload:1.0.7")
    implementation("com.liulishuo.okdownload:sqlite:1.0.7")
    implementation("org.apache.commons:commons-compress:1.27.1")
}
