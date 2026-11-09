# Keep the JNI entry points reachable by their exact Java names when minification
# is enabled in a future release build.
-keepclasseswithmembernames class com.dreamguard.app.audio.NativeAudioEngine {
    native <methods>;
}
