plugins {
    id("com.android.application")
    id("kotlin-android")
    // The Flutter Gradle Plugin must be applied after the Android and Kotlin Gradle plugins.
    id("dev.flutter.flutter-gradle-plugin")
}

// 产品要求覆盖 Android 6/API 23；Flutter 3.41 对 23 给出退役预警，但仍允许构建。
val yxMinimumSdk = 23

android {
    namespace = "com.yx.yx_gateway_timer"
    compileSdk = flutter.compileSdkVersion
    ndkVersion = flutter.ndkVersion

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = JavaVersion.VERSION_17.toString()
    }

    defaultConfig {
        applicationId = "com.yx.yx_gateway_timer"
        minSdk = yxMinimumSdk
        targetSdk = flutter.targetSdkVersion
        versionCode = flutter.versionCode
        versionName = flutter.versionName
    }

    buildTypes {
        release {
            // TODO: Add your own signing config for the release build.
            // Signing with the debug keys for now, so `flutter run --release` works.
            signingConfig = signingConfigs.getByName("debug")
        }
    }
}

flutter {
    source = "../.."
}

dependencies {
    // flutter_reactive_ble 内部使用 RxJava；APP 需要安装全局错误处理器，
    // 防止通知流取消后迟到的断线异常使 Android 进程崩溃。
    implementation("io.reactivex.rxjava2:rxjava:2.2.21")
}
