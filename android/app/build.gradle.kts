plugins {
    alias(libs.plugins.android.application)
}

android {
    namespace = "com.xxiao.ilemu"
    compileSdk { version = release(37) }
    ndkVersion = "30.0.16248370"

    defaultConfig {
        applicationId = "com.xxiao.ilemu"
        minSdk = 28
        targetSdk = 37
        versionCode = 1
        versionName = "0.1.0"
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
