import java.util.Properties
import java.io.FileInputStream

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// Load keystore properties from local.properties or gradle.properties
val keystorePropertiesFile = rootProject.file("local.properties")
val keystoreProperties = Properties()
if (keystorePropertiesFile.exists()) {
    FileInputStream(keystorePropertiesFile).use {
        keystoreProperties.load(it)
    }
}

// Helper to extract version from C header
fun getDSVersion(): String {
    val headerFile = file("../../src/droidspace.h")
    if (!headerFile.exists()) return "1.0.0"
    
    val pattern = Regex("#define\\s+DS_VERSION\\s+\"([^\"]+)\"")
    var foundVersion = "1.0.0"
    
    headerFile.useLines { lines ->
        for (line in lines) {
            val match = pattern.find(line)
            if (match != null) {
                foundVersion = match.groupValues[1]
                break
            }
        }
    }
    return foundVersion
}

val dsVersionName = getDSVersion()
val dsVersionCodeVal = dsVersionName.split(".").let { parts ->
    try {
        val major = parts.getOrNull(0)?.toInt() ?: 1
        val minor = parts.getOrNull(1)?.toInt() ?: 0
        val patch = parts.getOrNull(2)?.toInt() ?: 0
        major * 10000 + minor * 100 + patch
    } catch (e: Exception) {
        1
    }
}

android {
    namespace = "com.droidspaces.app"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.droidspaces.app"
        minSdk = 26
        targetSdk = 34
        versionCode = dsVersionCodeVal
        versionName = dsVersionName

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        vectorDrawables {
            useSupportLibrary = true
        }
    }

    signingConfigs {
        // Use droidspaces.keystore for both debug and release
        // Passwords are loaded from local.properties, gradle.properties, or environment variables
        val keystoreFile = file("../../droidspaces.keystore")
        val keystorePassword = keystoreProperties["KEYSTORE_PASSWORD"] as String? 
            ?: project.findProperty("KEYSTORE_PASSWORD") as String? 
            ?: ""
        val keyAliasName = keystoreProperties["KEY_ALIAS"] as String?
            ?: project.findProperty("KEY_ALIAS") as String?
            ?: "droidspaces"
        val actualKeyPassword = keystoreProperties["KEY_PASSWORD"] as String?
            ?: project.findProperty("KEY_PASSWORD") as String?
            ?: keystorePassword

        if (keystoreFile.exists() && keystorePassword.isNotEmpty()) {
            getByName("debug") {
                storeFile = keystoreFile
                storePassword = keystorePassword
                keyAlias = keyAliasName
                keyPassword = actualKeyPassword
                enableV2Signing = true
                enableV3Signing = true
                enableV4Signing = true
            }
            create("release") {
                storeFile = keystoreFile
                storePassword = keystorePassword
                keyAlias = keyAliasName
                keyPassword = actualKeyPassword
                enableV2Signing = true
                enableV3Signing = true
                enableV4Signing = true
            }
        } else {
            // Fallback to default debug keystore or generate a temporary one
            var fallbackKeystore = file(System.getProperty("user.home") + "/.android/debug.keystore")

            if (!keystoreFile.exists() && !fallbackKeystore.exists()) {
                println("WARNING: No keystore found. Generating temporary CI keystore...")
                val ciKeystore = file("../../ci-debug.keystore")
                if (!ciKeystore.exists()) {
                    val dn = "CN=John Doe, OU=Mobile, O=Anonymized, L=Earth, ST=Galaxy, C=UN"
                    project.exec {
                        commandLine(
                            "keytool", "-genkeypair",
                            "-v",
                            "-keystore", ciKeystore.absolutePath,
                            "-alias", "droidspaces",
                            "-keyalg", "RSA",
                            "-keysize", "2048",
                            "-validity", "10000",
                            "-storetype", "JKS",
                            "-storepass", "android",
                            "-keypass", "android",
                            "-dname", dn,
                            "-sigalg", "SHA256withRSA"
                        )
                    }
                }
                fallbackKeystore = ciKeystore
            }

            println("Using fallback keystore: ${fallbackKeystore.absolutePath}")

            getByName("debug") {
                storeFile = fallbackKeystore
                storePassword = "android"
                keyAlias = if (fallbackKeystore.name.contains("ci-debug")) "droidspaces" else "androiddebugkey"
                keyPassword = "android"
            }
            create("release") {
                initWith(getByName("debug"))
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
            // Enable R8 full mode for maximum optimization
            isDebuggable = false
            isJniDebuggable = false
            signingConfig = signingConfigs.getByName("release")
        }
        debug {
            // Disable minification in debug for faster builds
            isMinifyEnabled = false
            isDebuggable = true
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
        // Aggressive Kotlin compiler optimizations for maximum performance
        freeCompilerArgs += listOf(
            "-opt-in=kotlin.RequiresOptIn",
            "-Xinline-classes", // Enable inline classes for better performance
            "-Xno-param-assertions", // Remove parameter assertions in release builds
            "-Xno-call-assertions", // Remove call assertions in release builds
            "-Xno-receiver-assertions", // Remove receiver assertions in release builds
            "-Xjvm-default=all", // Use JVM default methods for better interop
            "-Xbackend-threads=0" // Use all available threads for compilation
        )
    }

    buildFeatures {
        compose = true
    }

    composeOptions {
        kotlinCompilerExtensionVersion = "1.5.8"
    }

    packaging {
        resources {
            excludes += "/META-INF/{AL2.0,LGPL2.1}"
        }
    }
}

dependencies {
    // Compose BOM
    implementation(platform("androidx.compose:compose-bom:2024.02.00"))
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-graphics")
    implementation("androidx.compose.ui:ui-tooling-preview")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.material:material-icons-extended")

    // Core Android
    implementation("androidx.core:core-ktx:1.12.0")
    implementation("androidx.core:core-splashscreen:1.0.1")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.7.0")
    implementation("androidx.activity:activity-compose:1.8.2")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("com.google.android.material:material:1.11.0")

    // Removed App Startup library - direct initialization in Application.onCreate() is faster
    // Eliminates ContentProvider overhead (~5-10ms saved)

    // Navigation
    implementation("androidx.navigation:navigation-compose:2.7.7")

    // ViewModel
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.7.0")

    // Root execution - libsu
    implementation("com.github.topjohnwu.libsu:core:5.2.1")
    implementation("com.github.topjohnwu.libsu:service:5.2.1")
    implementation("com.github.topjohnwu.libsu:io:5.2.1")

    // Coroutines - latest version for better performance
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.7.3")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.7.3")

    // Terminal emulator (ported from LXC-Manager / termux-app)
    implementation("com.github.termux.termux-app:terminal-view:0.118.1")
    implementation("com.github.termux.termux-app:terminal-emulator:0.118.1")

    // Debug
    debugImplementation("androidx.compose.ui:ui-tooling")
    debugImplementation("androidx.compose.ui:ui-test-manifest")
}

