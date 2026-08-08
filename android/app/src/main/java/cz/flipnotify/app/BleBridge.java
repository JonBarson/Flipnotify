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
    private final android.os.Handler handler = new android.os.Handler(android.os.Looper.getMainLooper());
    private BluetoothGatt gatt;
    private BluetoothGattCharacteristic rx;
    private BluetoothLeScanner scanner;
    private ScanCallback scanCallback;
    private String status = "Not connected";
    private Runnable onChanged;

    private BleBridge(Context c) { context = c.getApplicationContext(); }
    public static synchronized BleBridge get(Context c) { if(instance == null) instance = new BleBridge(c); return instance; }
    public String getStatus() { return status; }

    private void notifyChanged() { Runnable r = onChanged; if(r != null) r.run(); }

    @SuppressLint("MissingPermission")
    private void stopScan() {
        if(scanner != null && scanCallback != null) {
            try { scanner.stopScan(scanCallback); } catch(Exception ignored) {}
        }
        scanCallback = null;
    }

    // The Flipper advertises its name ("Flipper Xxxx") and a 16-bit service UUID (0x3080 | hw_color).
    // Match on either, so a missing device name in the scan record does not hide it.
    @SuppressLint("MissingPermission")
    private static boolean isFlipper(ScanResult result) {
        ScanRecord rec = result.getScanRecord();
        String name = rec != null ? rec.getDeviceName() : null;
        if(name == null) { try { name = result.getDevice().getName(); } catch(Exception ignored) {} }
        if(name != null && name.startsWith("Flipper")) return true;
        if(rec != null && rec.getServiceUuids() != null) {
            for(android.os.ParcelUuid pu : rec.getServiceUuids()) {
                UUID u = pu.getUuid();
                // 16-bit UUIDs use the Bluetooth base UUID: 0000xxxx-0000-1000-8000-00805F9B34FB
                if(u.getLeastSignificantBits() == 0x800000805f9b34fbL
                        && (u.getMostSignificantBits() & 0xFFFFFFFFL) == 0x1000L) {
                    int s = (int)((u.getMostSignificantBits() >>> 32) & 0xFFFF);
                    if((s & 0xFFF0) == 0x3080) return true;
                }
            }
        }
        return false;
    }

    @SuppressLint("MissingPermission")
    private static String nameOf(ScanResult result) {
        ScanRecord rec = result.getScanRecord();
        String name = rec != null ? rec.getDeviceName() : null;
        if(name == null) { try { name = result.getDevice().getName(); } catch(Exception ignored) {} }
        return name == null ? "Flipper" : name;
    }

    @SuppressLint("MissingPermission")
    public void scanAndConnect(Runnable changed) {
        onChanged = changed;
        BluetoothManager bm = context.getSystemService(BluetoothManager.class);
        BluetoothAdapter adapter = bm == null ? null : bm.getAdapter();
        if(adapter == null || !adapter.isEnabled()) { status = "Bluetooth is off"; notifyChanged(); return; }
        scanner = adapter.getBluetoothLeScanner();
        if(scanner == null) { status = "Bluetooth is off"; notifyChanged(); return; }
        stopScan();
        status = "Scanning..."; notifyChanged();
        ScanSettings settings = new ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build();
        final java.util.Set<String> seen = new java.util.HashSet<>();
        scanCallback = new ScanCallback() {
            @SuppressLint("MissingPermission") @Override public void onScanResult(int callbackType, ScanResult result) {
                seen.add(result.getDevice().getAddress());
                if(isFlipper(result)) {
                    stopScan();
                    status = "Connecting " + nameOf(result); notifyChanged();
                    gatt = result.getDevice().connectGatt(context, false, callback);
                    return;
                }
                status = "Scanning... (" + seen.size() + " devices)"; notifyChanged();
            }
            @Override public void onScanFailed(int errorCode) { stopScan(); status = "Scan failed: " + errorCode; notifyChanged(); }
        };
        try {
            scanner.startScan(null, settings, scanCallback);
        } catch(Exception e) {
            scanCallback = null; status = "Scan error"; notifyChanged(); return;
        }
        final ScanCallback started = scanCallback;
        handler.postDelayed(() -> {
            if(scanCallback == started) { stopScan(); status = "No Flipper found (" + seen.size() + " devices seen)"; notifyChanged(); }
        }, 20000);
    }

    private final BluetoothGattCallback callback = new BluetoothGattCallback() {
        @SuppressLint("MissingPermission") @Override public void onConnectionStateChange(BluetoothGatt g, int statusCode, int newState) {
            if(newState == BluetoothProfile.STATE_CONNECTED) {
                status = "Connected, negotiating MTU..."; notifyChanged();
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
        if(bytes.length > 240) bytes = Arrays.copyOf(bytes, 240);
        rx.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
        rx.setValue(bytes);
        return gatt.writeCharacteristic(rx);
    }
    private static String safe(String s) { return s == null ? "" : s.replace('\t',' ').replace('\n',' '); }
}
