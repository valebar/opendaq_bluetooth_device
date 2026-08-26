package com.opendaq.bt;

import android.Manifest;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothManager;
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Build;

/** Android entry point for the openDAQ Bluetooth JNI bridge. */
public final class OpenDaqBluetooth
{
    private static final String LIBRARY_NAME = "bluetooth_module.module";

    private static boolean libraryLoaded = false;

    private OpenDaqBluetooth()
    {
    }

    /** Loads the bridge and stores the application context. Safe to call repeatedly. */
    public static synchronized void initialize(Context context)
    {
        if (context == null)
            throw new IllegalArgumentException("context must not be null");

        if (!libraryLoaded)
        {
            System.loadLibrary(LIBRARY_NAME);
            libraryLoaded = true;
        }

        nativeInitialize(context.getApplicationContext());
    }

    /** Native library directory for {@code InstanceBuilder.addModulePath}. */
    public static String moduleDirectory(Context context)
    {
        return context.getApplicationInfo().nativeLibraryDir;
    }

    /** Runtime permissions required on the current Android version. */
    public static String[] requiredPermissions()
    {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S)
            return new String[] { Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT };

        return new String[] { Manifest.permission.ACCESS_FINE_LOCATION };
    }

    /** Whether every permission {@link #requiredPermissions} lists has been granted. */
    public static boolean permissionsGranted(Context context)
    {
        return missingPermission(context) == null;
    }

    /** First missing permission, or null. */
    public static String missingPermission(Context context)
    {
        for (String permission : requiredPermissions())
        {
            if (context.checkSelfPermission(permission) != PackageManager.PERMISSION_GRANTED)
                return permission;
        }

        return null;
    }

    /** Human-readable unavailability reason, or null when Bluetooth is usable. */
    public static String unavailableReason(Context context)
    {
        final BluetoothManager manager = context.getSystemService(BluetoothManager.class);
        if (manager == null)
            return "this device has no Bluetooth service";

        final String missing = missingPermission(context);
        if (missing != null)
        {
            return "the " + missing + " permission has not been granted to this application. "
                 + "Native code cannot request it: the application must, from an Activity";
        }

        final BluetoothAdapter adapter = manager.getAdapter();
        if (adapter == null)
            return "this device has no Bluetooth adapter";

        if (!adapter.isEnabled())
            return "Bluetooth is turned off";

        if (adapter.getBluetoothLeScanner() == null)
            return "this device's Bluetooth adapter does not offer Low Energy scanning";

        return null;
    }

    /** Whether native initialization resolved the Java bridge classes. */
    public static native boolean nativeBridgeReady();

    private static native void nativeInitialize(Context context);
}
