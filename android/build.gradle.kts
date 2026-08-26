plugins {
    id("com.android.library")
}

android {
    namespace = "com.opendaq.bt"
    compileSdk = 36

    defaultConfig {
        minSdk = 26

        // Each ABI is a full openDAQ + Boost cross-build, so a cold two-ABI build takes
        // roughly twice as long and twice the disk. Narrow it while iterating with
        // -PabiFilters=arm64-v8a; a release AAR is complete by default.
        ndk {
            abiFilters += (project.findProperty("abiFilters") as String?)
                ?.split(",")
                ?.map { it.trim() }
                ?.filter { it.isNotEmpty() }
                ?: listOf("arm64-v8a", "armeabi-v7a")
        }

        externalNativeBuild {
            cmake {
                // Test executables are built separately for adb.
                targets += "bluetooth_module"

                arguments += listOf(
                    "-DOPENDAQ_BT_BUILD_TESTS=OFF",
                    "-DOPENDAQ_BT_BUILD_APP=OFF"
                )

                // Optional local sources avoid FetchContent downloads.
                (project.findProperty("opendaqSourceDir") as String?)?.let {
                    arguments += "-DFETCHCONTENT_SOURCE_DIR_OPENDAQ=$it"
                }
                (project.findProperty("boostSourceDir") as String?)?.let {
                    arguments += "-DFETCHCONTENT_SOURCE_DIR_BOOST=$it"
                }
            }
        }

        consumerProguardFiles("consumer-rules.pro")

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
    }

    externalNativeBuild {
        cmake {
            path = file("../CMakeLists.txt")
            version = "3.31.0"
        }
    }

    packaging {
        jniLibs {
            // ModuleManager needs a real .so file to scan and dlopen.
            useLegacyPackaging = true
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

// Instrumented-test dependencies are not exposed by the AAR.
dependencies {
    androidTestImplementation("androidx.test.ext:junit:1.3.0")
    androidTestImplementation("androidx.test:runner:1.3.0")
}
