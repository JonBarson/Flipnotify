package cz.flipnotify.app;

import android.app.Notification;
import android.service.notification.NotificationListenerService;
import android.service.notification.StatusBarNotification;

public class NotificationBridgeService extends NotificationListenerService {
    @Override public void onNotificationPosted(StatusBarNotification sbn) {
        if(!getSharedPreferences("p", MODE_PRIVATE).getBoolean("enabled", true)) return;
        Notification n = sbn.getNotification();
        String title = String.valueOf(n.extras.getCharSequence(Notification.EXTRA_TITLE, ""));
        String text = String.valueOf(n.extras.getCharSequence(Notification.EXTRA_TEXT, ""));
        String app;
        try { app = getPackageManager().getApplicationLabel(getPackageManager().getApplicationInfo(sbn.getPackageName(), 0)).toString(); }
        catch(Exception e) { app = sbn.getPackageName(); }
        if(text.equals("null")) text = "";
        if(title.equals("null")) title = "";
        BleBridge.get(this).send(app, title, text);
    }
}
