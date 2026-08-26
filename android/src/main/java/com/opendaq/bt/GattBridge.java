package com.opendaq.bt;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.content.Context;
import android.os.Build;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import java.util.UUID;

/** Owns a GATT client and forwards its callbacks to native code. */
public final class GattBridge extends BluetoothGattCallback
{
    /** Client Characteristic Configuration descriptor. */
    private static final UUID CCCD = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");

    private static final String[] NO_FIELDS = new String[0];

    /** Clients whose close was blocked by a temporarily missing runtime permission. */
    private static final List<BluetoothGatt> PENDING_CLOSE = new ArrayList<>();

    private final long token;

    private volatile BluetoothGatt gatt;

    private final Map<String, BluetoothGattCharacteristic> characteristics = new HashMap<>();

    private GattBridge(long token)
    {
        this.token = token;
    }

    /** Opens a direct GATT connection, or returns null. */
    public static GattBridge connect(Context context, String address, long token)
    {
        // Release clients left behind by an earlier permission loss.
        retryPendingClose();

        final BluetoothManager manager = context.getSystemService(BluetoothManager.class);
        if (manager == null)
            return null;

        final BluetoothAdapter adapter = manager.getAdapter();
        if (adapter == null)
            return null;

        final BluetoothDevice device;
        try
        {
            device = adapter.getRemoteDevice(address);
        }
        catch (IllegalArgumentException e)
        {
            return null;
        }

        final GattBridge bridge = new GattBridge(token);

        try
        {
            bridge.gatt = device.connectGatt(context, false, bridge, BluetoothDevice.TRANSPORT_LE);
        }
        catch (SecurityException e)
        {
            return null;
        }

        return bridge.gatt == null ? null : bridge;
    }

    /** Disconnects and releases the GATT client. */
    public synchronized void close()
    {
        final BluetoothGatt current = gatt;
        if (current == null)
            return;

        try
        {
            current.disconnect();
        }
        catch (SecurityException ignored)
        {
            // close() may still succeed and release the GATT slot.
        }

        try
        {
            current.close();
        }
        catch (SecurityException ignored)
        {
            synchronized (PENDING_CLOSE)
            {
                PENDING_CLOSE.add(current);
            }
        }

        gatt = null;

        synchronized (characteristics)
        {
            characteristics.clear();
        }
    }

    private static void retryPendingClose()
    {
        synchronized (PENDING_CLOSE)
        {
            final Iterator<BluetoothGatt> pending = PENDING_CLOSE.iterator();
            while (pending.hasNext())
            {
                try
                {
                    pending.next().close();
                    pending.remove();
                }
                catch (SecurityException ignored)
                {
                    // Retry when a later connection proves the permission is available.
                    return;
                }
            }
        }
    }

    public boolean discoverServices()
    {
        final BluetoothGatt current = gatt;
        if (current == null)
            return false;

        try
        {
            return current.discoverServices();
        }
        catch (SecurityException e)
        {
            return false;
        }
    }

    public boolean requestMtu(int mtu)
    {
        final BluetoothGatt current = gatt;
        if (current == null)
            return false;

        try
        {
            return current.requestMtu(mtu);
        }
        catch (SecurityException e)
        {
            return false;
        }
    }

    /** Returns six fields per characteristic: handle, service UUID/instance/primary, UUID, properties. */
    public String[] describeServices()
    {
        final BluetoothGatt current = gatt;
        if (current == null)
            return NO_FIELDS;

        final List<BluetoothGattService> services;
        try
        {
            services = current.getServices();
        }
        catch (SecurityException e)
        {
            return NO_FIELDS;
        }

        if (services == null || services.isEmpty())
            return NO_FIELDS;

        final List<String> fields = new ArrayList<>();

        synchronized (characteristics)
        {
            characteristics.clear();

            for (BluetoothGattService service : services)
            {
                final String serviceUuid = service.getUuid().toString();
                final String serviceInstance = Integer.toString(service.getInstanceId());
                final boolean primary = service.getType() == BluetoothGattService.SERVICE_TYPE_PRIMARY;

                for (BluetoothGattCharacteristic characteristic : service.getCharacteristics())
                {
                    final String handle = handleOf(service, characteristic);
                    characteristics.put(handle, characteristic);

                    fields.add(handle);
                    fields.add(serviceUuid);
                    fields.add(serviceInstance);
                    fields.add(primary ? "1" : "0");
                    fields.add(characteristic.getUuid().toString());
                    fields.add(Integer.toString(characteristic.getProperties()));
                }
            }
        }

        return fields.toArray(new String[0]);
    }

    private static String handleOf(BluetoothGattService service,
                                   BluetoothGattCharacteristic characteristic)
    {
        return service.getUuid() + "/" + service.getInstanceId() + "/"
             + characteristic.getUuid() + "/" + characteristic.getInstanceId();
    }

    private BluetoothGattCharacteristic lookup(String handle)
    {
        synchronized (characteristics)
        {
            return characteristics.get(handle);
        }
    }

    public boolean read(String handle)
    {
        final BluetoothGatt current = gatt;
        final BluetoothGattCharacteristic characteristic = lookup(handle);
        if (current == null || characteristic == null)
            return false;

        try
        {
            return current.readCharacteristic(characteristic);
        }
        catch (SecurityException e)
        {
            return false;
        }
    }

    public boolean write(String handle, byte[] value, boolean withResponse)
    {
        final BluetoothGatt current = gatt;
        final BluetoothGattCharacteristic characteristic = lookup(handle);
        if (current == null || characteristic == null)
            return false;

        final int writeType = withResponse ? BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                                           : BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE;

        try
        {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU)
                return current.writeCharacteristic(characteristic, value, writeType) == BluetoothGatt.GATT_SUCCESS;

            // C++ serializes operations, as required by this mutable pre-33 API.
            characteristic.setWriteType(writeType);
            characteristic.setValue(value);
            return current.writeCharacteristic(characteristic);
        }
        catch (SecurityException e)
        {
            return false;
        }
    }

    /** Updates local notification routing and the peripheral's CCCD. */
    public boolean setNotify(String handle, boolean enable, boolean indicate)
    {
        final BluetoothGatt current = gatt;
        final BluetoothGattCharacteristic characteristic = lookup(handle);
        if (current == null || characteristic == null)
            return false;

        try
        {
            if (!current.setCharacteristicNotification(characteristic, enable))
                return false;

            final BluetoothGattDescriptor cccd = characteristic.getDescriptor(CCCD);
            if (cccd == null)
                return false;

            final byte[] value = !enable
                ? BluetoothGattDescriptor.DISABLE_NOTIFICATION_VALUE
                : (indicate ? BluetoothGattDescriptor.ENABLE_INDICATION_VALUE
                            : BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);

            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU)
                return current.writeDescriptor(cccd, value) == BluetoothGatt.GATT_SUCCESS;

            cccd.setValue(value);
            return current.writeDescriptor(cccd);
        }
        catch (SecurityException e)
        {
            return false;
        }
    }

    // Callbacks run on binder threads.

    @Override
    public void onConnectionStateChange(BluetoothGatt gatt, int status, int newState)
    {
        nativeOnConnectionStateChange(token, status, newState);
    }

    @Override
    public void onServicesDiscovered(BluetoothGatt gatt, int status)
    {
        nativeOnServicesDiscovered(token, status);
    }

    @Override
    public void onMtuChanged(BluetoothGatt gatt, int mtu, int status)
    {
        nativeOnMtuChanged(token, mtu, status);
    }

    @Override
    public void onCharacteristicRead(BluetoothGatt gatt,
                                     BluetoothGattCharacteristic characteristic,
                                     byte[] value,
                                     int status)
    {
        nativeOnCharacteristicRead(token, handleOf(characteristic), value, status);
    }

    /** Pre-33 callback overload. */
    @Override
    @SuppressWarnings("deprecation")
    public void onCharacteristicRead(BluetoothGatt gatt,
                                     BluetoothGattCharacteristic characteristic,
                                     int status)
    {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU)
            return;

        nativeOnCharacteristicRead(token, handleOf(characteristic), characteristic.getValue(), status);
    }

    @Override
    public void onCharacteristicChanged(BluetoothGatt gatt,
                                        BluetoothGattCharacteristic characteristic,
                                        byte[] value)
    {
        nativeOnCharacteristicChanged(token, handleOf(characteristic), value);
    }

    @Override
    @SuppressWarnings("deprecation")
    public void onCharacteristicChanged(BluetoothGatt gatt,
                                        BluetoothGattCharacteristic characteristic)
    {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU)
            return;

        nativeOnCharacteristicChanged(token, handleOf(characteristic), characteristic.getValue());
    }

    @Override
    public void onCharacteristicWrite(BluetoothGatt gatt,
                                      BluetoothGattCharacteristic characteristic,
                                      int status)
    {
        nativeOnCharacteristicWrite(token, handleOf(characteristic), status);
    }

    @Override
    public void onDescriptorWrite(BluetoothGatt gatt, BluetoothGattDescriptor descriptor, int status)
    {
        nativeOnDescriptorWrite(token, handleOf(descriptor.getCharacteristic()), status);
    }

    private static String handleOf(BluetoothGattCharacteristic characteristic)
    {
        if (characteristic == null)
            return "";

        final BluetoothGattService service = characteristic.getService();
        return service == null ? "" : handleOf(service, characteristic);
    }

    private static native void nativeOnConnectionStateChange(long token, int status, int newState);

    private static native void nativeOnServicesDiscovered(long token, int status);

    private static native void nativeOnMtuChanged(long token, int mtu, int status);

    private static native void nativeOnCharacteristicRead(long token, String handle, byte[] value, int status);

    private static native void nativeOnCharacteristicWrite(long token, String handle, int status);

    private static native void nativeOnCharacteristicChanged(long token, String handle, byte[] value);

    private static native void nativeOnDescriptorWrite(long token, String handle, int status);
}
