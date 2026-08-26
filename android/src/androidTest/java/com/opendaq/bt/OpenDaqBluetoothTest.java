package com.opendaq.bt;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

import android.content.Context;
import android.os.Build;

import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;

import org.junit.BeforeClass;
import org.junit.Test;
import org.junit.runner.RunWith;

/** Device-side Java/JNI wiring tests; no radio is required. */
@RunWith(AndroidJUnit4.class)
public class OpenDaqBluetoothTest
{
    private static Context context()
    {
        return InstrumentationRegistry.getInstrumentation().getTargetContext();
    }

    @BeforeClass
    public static void loadTheModule()
    {
        OpenDaqBluetooth.initialize(context());
    }

    @Test
    public void initializeIsIdempotent()
    {
        OpenDaqBluetooth.initialize(context());
    }

    @Test
    public void nativeSideResolvedTheJavaBridge()
    {
        assertTrue("The native module could not resolve the com.opendaq.bt classes. Either a "
                   + "method signature drifted from the descriptor the backend looks up, or a "
                   + "shrinker removed them.",
                   OpenDaqBluetooth.nativeBridgeReady());
    }

    @Test
    public void requiredPermissionsMatchTheRunningApiLevel()
    {
        final String[] permissions = OpenDaqBluetooth.requiredPermissions();

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S)
        {
            assertEquals(2, permissions.length);
            assertTrue(permissions[0].endsWith("BLUETOOTH_SCAN"));
            assertTrue(permissions[1].endsWith("BLUETOOTH_CONNECT"));
        }
        else
        {
            assertEquals(1, permissions.length);
            assertTrue(permissions[0].endsWith("ACCESS_FINE_LOCATION"));
        }
    }

    @Test
    public void unavailableReasonAnswersWithoutThrowing()
    {
        final String reason = OpenDaqBluetooth.unavailableReason(context());

        if (reason != null)
            assertTrue("A reason should be human-readable, got: " + reason, reason.length() > 10);
    }

    @Test
    public void moduleDirectoryIsWhereTheNativeLibraryLanded()
    {
        final String directory = OpenDaqBluetooth.moduleDirectory(context());

        assertNotNull(directory);
        assertTrue("openDAQ's ModuleManager scans this directory for the module, so it has to be a "
                   + "real path -- if it is not, extractNativeLibs was turned off.",
                   new java.io.File(directory).isDirectory());
    }

    @Test
    public void theModuleIsExtractedUnderTheNameOpenDaqLooksFor()
    {
        final java.io.File module =
            new java.io.File(OpenDaqBluetooth.moduleDirectory(context()), "libbluetooth_module.module.so");

        assertTrue("Expected the module at " + module + ". openDAQ discovers modules by file "
                   + "extension, so the .module.so suffix is what makes it visible at all.",
                   module.isFile());
    }
}
