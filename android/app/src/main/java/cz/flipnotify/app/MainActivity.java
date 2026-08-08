package cz.flipnotify.app;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.provider.Settings;
import android.widget.*;

public class MainActivity extends Activity {
    private TextView status;
    @Override public void onCreate(Bundle b) {
        super.onCreate(b); setContentView(R.layout.activity_main);
        status = findViewById(R.id.status);
        findViewById(R.id.notificationAccess).setOnClickListener(v -> startActivity(new Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS)));
        findViewById(R.id.connect).setOnClickListener(v -> {
            if(android.os.Build.VERSION.SDK_INT >= 31 && (checkSelfPermission(Manifest.permission.BLUETOOTH_SCAN) != PackageManager.PERMISSION_GRANTED || checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED)) {
                requestPermissions(new String[]{Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT}, 10); return;
            }
            BleBridge.get(this).scanAndConnect(() -> runOnUiThread(this::refresh));
        });
        ((Switch)findViewById(R.id.enabled)).setOnCheckedChangeListener((b1, checked) -> getSharedPreferences("p", MODE_PRIVATE).edit().putBoolean("enabled", checked).apply());
        refresh();
    }
    private void refresh() { status.setText(BleBridge.get(this).getStatus()); }
}
