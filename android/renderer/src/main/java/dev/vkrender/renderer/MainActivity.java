package dev.vkrender.renderer;

import android.app.NativeActivity;

/**
 * The renderer app: a NativeActivity whose native loop (android_main.cpp)
 * does the Vulkan work - AImageReader input, zero-copy AHardwareBuffer
 * import, video on a spinning cube. Producers are OTHER APPS (M10): they
 * bind RendererService for the input Surface. This activity starts nothing.
 *
 * Launch:
 *   adb shell am start -n dev.vkrender.renderer/.MainActivity
 */
public class MainActivity extends NativeActivity {
}
