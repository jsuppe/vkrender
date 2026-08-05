package dev.vkrender.vkrender_sender

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder

/**
 * Minimal foreground service whose only job is to hold the `mediaProjection`
 * foreground-service type for the duration of a screen cast. Android 14+
 * throws from MediaProjection unless such a service is already running when
 * capture starts, and flutter_webrtc does not ship one. Started/stopped from
 * Dart (see lib/screen_capture_service.dart) around getDisplayMedia.
 */
class ScreenCaptureService : Service() {
    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val channelId = "vkrender_cast"
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
            nm.createNotificationChannel(
                NotificationChannel(channelId, "Screen cast", NotificationManager.IMPORTANCE_LOW)
            )
        }
        val notification: Notification = Notification.Builder(this, channelId)
            .setContentTitle("vkrender")
            .setContentText("Casting your screen")
            .setSmallIcon(android.R.drawable.ic_menu_share)
            .build()

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(
                NOTIF_ID,
                notification,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION
            )
        } else {
            startForeground(NOTIF_ID, notification)
        }
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        stopForeground(STOP_FOREGROUND_REMOVE)
        super.onDestroy()
    }

    companion object {
        private const val NOTIF_ID = 0xC457 // "CAST"
    }
}
