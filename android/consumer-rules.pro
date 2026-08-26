# JNI and platform callbacks are invisible to R8 reachability analysis.
-keep class com.opendaq.bt.** { *; }

# Preserve mangled native method names.
-keepclasseswithmembernames class com.opendaq.bt.** {
    native <methods>;
}
