package com.opendaq.bt;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanFilter;
import android.bluetooth.le.ScanRecord;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.content.Context;
import android.os.ParcelUuid;
import android.util.SparseArray;

import java.util.ArrayList;
import java.util.List;

/** Scan callback that forwards flattened advertisements to native code. */
public final class ScanBridge extends ScanCallback
{
    private static final String[] NO_UUIDS = new String[0];
    private static final int[] NO_IDS = new int[0];
    private static final byte[][] NO_DATA = new byte[0][];

    private final Context context;
    private final long token;

    private boolean scanning;

    public ScanBridge(Context context, long token)
    {
        this.context = context;
        this.token = token;
    }

    private BluetoothLeScanner scanner()
    {
        final BluetoothManager manager = context.getSystemService(BluetoothManager.class);
        if (manager == null)
            return null;

        final BluetoothAdapter adapter = manager.getAdapter();
        return adapter == null ? null : adapter.getBluetoothLeScanner();
    }

    /** Idempotence avoids Android's scan-start quota. */
    public synchronized boolean startScan(String[] serviceUuids)
    {
        if (scanning)
            return true;

        final BluetoothLeScanner leScanner = scanner();
        if (leScanner == null)
            return false;

        final List<ScanFilter> filters = new ArrayList<>();
        if (serviceUuids != null)
        {
            for (String uuid : serviceUuids)
            {
                try
                {
                    filters.add(new ScanFilter.Builder()
                                    .setServiceUuid(ParcelUuid.fromString(uuid))
                                    .build());
                }
                catch (IllegalArgumentException ignored) {}
            }
        }

        final ScanSettings settings =
            new ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build();

        try
        {
            leScanner.startScan(filters.isEmpty() ? null : filters, settings, this);
        }
        catch (SecurityException | IllegalStateException e) { return false; }

        scanning = true;
        return true;
    }

    public synchronized void stopScan()
    {
        if (!scanning)
            return;

        scanning = false;

        final BluetoothLeScanner leScanner = scanner();
        if (leScanner == null)
            return;

        try
        {
            leScanner.stopScan(this);
        }
        catch (SecurityException | IllegalStateException ignored) {}
    }

    public synchronized boolean isScanning()
    {
        return scanning;
    }

    /** Connected devices are included because they may no longer advertise. */
    public static String[] connectedAddresses(Context context)
    {
        final BluetoothManager manager = context.getSystemService(BluetoothManager.class);
        if (manager == null)
            return NO_UUIDS;

        try
        {
            final List<BluetoothDevice> devices = manager.getConnectedDevices(BluetoothProfile.GATT);
            final String[] addresses = new String[devices.size()];
            for (int i = 0; i < devices.size(); ++i)
                addresses[i] = devices.get(i).getAddress();

            return addresses;
        }
        catch (SecurityException e)
        {
            return NO_UUIDS;
        }
    }

    @Override
    public void onScanResult(int callbackType, ScanResult result)
    {
        deliver(result);
    }

    @Override
    public void onBatchScanResults(List<ScanResult> results)
    {
        for (ScanResult result : results)
            deliver(result);
    }

    @Override
    public void onScanFailed(int errorCode)
    {
        synchronized (this)
        {
            scanning = false;
        }

        nativeOnScanFailed(token, errorCode);
    }

    private void deliver(ScanResult result)
    {
        if (result == null)
            return;

        final BluetoothDevice device = result.getDevice();
        if (device == null)
            return;

        final ScanRecord record = result.getScanRecord();

        // device.getName() requires BLUETOOTH_CONNECT on API 31+.
        String name = record == null ? null : record.getDeviceName();

        String[] uuids = NO_UUIDS;
        int[] manufacturerIds = NO_IDS;
        byte[][] manufacturerData = NO_DATA;

        if (record != null)
        {
            final List<ParcelUuid> serviceUuids = record.getServiceUuids();
            if (serviceUuids != null && !serviceUuids.isEmpty())
            {
                uuids = new String[serviceUuids.size()];
                for (int i = 0; i < serviceUuids.size(); ++i)
                    uuids[i] = serviceUuids.get(i).getUuid().toString();
            }

            final SparseArray<byte[]> manufacturer = record.getManufacturerSpecificData();
            if (manufacturer != null && manufacturer.size() > 0)
            {
                manufacturerIds = new int[manufacturer.size()];
                manufacturerData = new byte[manufacturer.size()][];

                for (int i = 0; i < manufacturer.size(); ++i)
                {
                    manufacturerIds[i] = manufacturer.keyAt(i);
                    manufacturerData[i] = manufacturer.valueAt(i);
                }
            }
        }

        nativeOnScanResult(token, device.getAddress(), name, result.getRssi(), uuids,
                           manufacturerIds, manufacturerData);
    }

    private static native void nativeOnScanResult(long token,
                                                  String address,
                                                  String name,
                                                  int rssi,
                                                  String[] serviceUuids,
                                                  int[] manufacturerIds,
                                                  byte[][] manufacturerData);

    private static native void nativeOnScanFailed(long token, int errorCode);
}
