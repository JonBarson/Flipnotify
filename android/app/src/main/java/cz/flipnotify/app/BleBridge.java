package cz.flipnotify.app;

import android.annotation.SuppressLint;
import android.bluetooth.*;
import android.bluetooth.le.*;
import android.content.Context;
import java.nio.charset.StandardCharsets;
import java.util.*;

public final class BleBridge {
    public static final UUID SERVICE = UUID.fromString("8fe5b3d5-2e7f-4a98-2a48-7acc60fe0000");
    public static final UUID RX = UUID.fromString("19ed82ae-ed21-4c9d-4145-228e62fe0000");
    private static BleBridge instance;
    private final Context context;
    private BluetoothGatt gatt;
    private BluetoothGattCharacteristic rx;
    private String status = "Not connected";

    private BleBridge(Context c) { context = c.getApplicationContext(); }
    public static synchronized BleBridge get(Context c) { if(instance == null) instance = new BleBridge(c); return instance; }
    public String getStatus() { return status; }

    @SuppressLint("MissingPermission")
    public void scanAndConnect(Runnable changed) {
        BluetoothManager bm = context.getSystemService(BluetoothManager.class);
        BluetoothAdapter adapter = bm.getAdapter();
        if(adapter == null || !adapter.isEnabled()) { status = "Bluetooth is off"; changed.run(); return; }
        status = "Scanning..."; changed.run();
        ScanFilter filter = new ScanFilter.Builder().setServiceUuid(new android.os.ParcelUuid(SERVICE)).build();
        ScanSettings settings = new ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build();
        BluetoothLeScanner scanner = adapter.getBluetoothLeScanner();
        scanner.startScan(Collections.singletonList(filter), settings, new ScanCallback() {
            @Override public void onScanResult(int callbackType, ScanResult result) {
                scanner.stopScan(this);
                status = "Connecting " + result.getDevice().getName(); changed.run();
                gatt = result.getDevice().connectGatt(context, false, callback);
            }
            @Override public void onScanFailed(int errorCode) { status = "Scan failed: " + errorCode; changed.run(); }
        });
    }

    private final BluetoothGattCallback callback = new BluetoothGattCallback() {
        @SuppressLint("MissingPermission") @Override public void onConnectionStateChange(BluetoothGatt g, int statusCode, int newState) {
            if(newState == BluetoothProfile.STATE_CONNECTED) { status = "Connected, discovering..."; g.discoverServices(); }
            else { status = "Disconnected"; rx = null; }
        }
        @Override public void onServicesDiscovered(BluetoothGatt g, int statusCode) {
            BluetoothGattService s = g.getService(SERVICE);
            rx = s == null ? null : s.getCharacteristic(RX);
            status = rx != null ? "Ready" : "Serial service not found";
        }
    };

    @SuppressLint("MissingPermission")
    public synchronized boolean send(String app, String title, String text) {
        if(gatt == null || rx == null) return false;
        String clean = (safe(app) + "\t" + safe(title) + "\t" + safe(text) + "\n");
        byte[] bytes = clean.getBytes(StandardCharsets.UTF_8);
        bytes = Arrays.copyOf(bytes, 240);
        rx.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
        rx.setValue(bytes);
        return gatt.writeCharacteristic(rx);
    }
    private static String safe(String s) { return s == null ? "" : s.replace('\t',' ').replace('\n',' '); }
}
