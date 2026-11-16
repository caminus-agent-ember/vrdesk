package com.caminus.vrdesk;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;

/**
 * Thin Java shim: android_native_app_glue owns the real work in libvrdesk_xr.so.
 * We use a plain Activity (not NativeActivity) so we can log and keep the
 * manifest simple; the native side gets the Activity via XR_KHR_android_create_instance.
 */
public class MainActivity extends Activity {
    private static final String TAG = "vrdesk";

    static {
        System.loadLibrary("openxr_loader");
        System.loadLibrary("vrdesk_xr");
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Log.i(TAG, "MainActivity.onCreate");
    }

    @Override
    protected void onResume() {
        super.onResume();
        Log.i(TAG, "MainActivity.onResume");
    }
}
