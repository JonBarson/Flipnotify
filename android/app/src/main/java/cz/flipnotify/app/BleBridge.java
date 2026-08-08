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
    private Runnable onChanged;

    private BleBridge(Context c) { context = c.getApplicationContext(); }
    public static synchronized BleBridge get(Context c) { if(instance == null) instance = new BleBridge(c); return instance; }
    public String getStatus() { return status; }

    private void notifyChanged() { Runnable r = onChanged; if(r != null) r.run(); }

    @SuppressLint("MissingPermission")
    public void scanAndConnect(Runnable changed) {
        onChanged = changed;
        BluetoothManager bm = context.getSystemService(BluetoothManager.class);
        BluetoothAdapter adapter = bm.getAdapter();
        if(adapter == null || !adapter.isEnabled()) { status = "Bluetooth is off"; notifyChanged(); return; }
        status = "Scanning..."; notifyChanged();
        // The Flipper advertises only a 16-bit service UUID (0x3080 | hw_color) plus its name.
        // The 128-bit Serial service UUID (SERVICE) is exposed only after connecting, so filtering
        // the scan by it filters the Flipper out. Scan unfiltered and match it by advertised name.
        ScanSettings settings = new ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build();
        BluetoothLeScanner scanner = adapter.getBluetoothLeScanner();
        if(scanner == null) { status = "Bluetooth is off"; notifyChanged(); return; }
        scanner.startScan(null, settings, new ScanCallback() {
            @SuppressLint("MissingPermission") @Override public void onScanResult(int callbackType, ScanResult result) {
                String name = null;
                if(result.getScanRecord() != null) name = result.getScanRecord().getDeviceName();
                if(name == null) name = result.getDevice().getName();
                if(name == null || !name.startsWith("Flipper")) return;
                scanner.stopScan(this);
                status = "Connecting " + name; notifyChanged();
                gatt = result.getDevice().connectGatt(context, false, callback);
            }
            @Override public void onScanFailed(int errorCode) { status = "Scan failed: " + errorCode; notifyChanged(); }
        });
    }

    private final BluetoothGattCallback callback = new BluetoothGattCallback() {
        @SuppressLint("MissingPermission") @Override public void onConnectionStateChange(BluetoothGatt g, int statusCode, int newState) {
            if(newState == BluetoothProfile.STATE_CONNECTED) {
                status = "Connected, negotiating MTU..."; notifyChanged();
                // Default ATT MTU is only 23 bytes; request a larger one so a full notification
                // fits in a single write. Fall back to discovery if the request can't be started.
                if(!g.requestMtu(247)) g.discoverServices();
            } else {
                status = "Disconnected"; rx = null; notifyChanged();
            }
        }
        @SuppressLint("MissingPermission") @Override public void onMtuChanged(BluetoothGatt g, int mtu, int statusCode) {
            status = "Discovering services..."; notifyChanged();
            g.discoverServices();
        }
        @Override public void onServicesDiscovered(BluetoothGatt g, int statusCode) {
            BluetoothGattService s = g.getService(SERVICE);
            rx = s == null ? null : s.getCharacteristic(RX);
            status = rx != null ? "Ready" : "Serial service not found";
            notifyChanged();
        }
    };

    @SuppressLint("MissingPermission")
    public synchronized boolean send(String app, String title, String text) {
        if(gatt == null || rx == null) return false;
        String clean = (safe(app) + "\t" + safe(title) + "\t" + safe(text) + "\n");
        byte[] bytes = clean.getBytes(StandardCharsets.UTF_8);
        // Send the real length (up to the Flipper serial buffer cap). Do NOT zero-pad to a fixed
        // size -- padding forces oversized writes and can break parsing on the Flipper side.
        if(bytes.length > 240) bytes = Arrays.copyOf(bytes, 240);
        rx.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
        rx.setValue(bytes);
        return gatt.writeCharacteristic(rx);
    }
    private static String safe(String s) { return s == null ? "" : s.replace('\t',' ').replace('\n',' '); }
}
