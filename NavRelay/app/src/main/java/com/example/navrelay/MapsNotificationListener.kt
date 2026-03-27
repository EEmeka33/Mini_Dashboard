package com.example.navrelay

import android.annotation.SuppressLint
import android.app.Notification
import android.content.Context
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.os.Bundle
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import android.text.TextUtils
import org.json.JSONObject

class MapsNotificationListener :
    NotificationListenerService(),
    LocationListener {

    private var lastSpeedKmh: Double = 0.0
    private var lastLat: Double? = null
    private var lastLon: Double? = null

    override fun onCreate() {
        super.onCreate()
        requestLocationUpdates()
    }

    @SuppressLint("MissingPermission")
    private fun requestLocationUpdates() {
        val lm = getSystemService(Context.LOCATION_SERVICE) as LocationManager
        try {
            lm.requestLocationUpdates(
                LocationManager.GPS_PROVIDER,
                1000L,
                1f,
                this
            )
        } catch (e: SecurityException) {
            e.printStackTrace()
        }
    }

    override fun onNotificationPosted(sbn: StatusBarNotification) {
        val pkg = sbn.packageName ?: return
        if (pkg != "com.google.android.apps.maps") return

        val notif = sbn.notification ?: return
        val extras = notif.extras ?: return

        val title = extras.getString(Notification.EXTRA_TITLE) ?: ""
        val text = extras.getCharSequence(Notification.EXTRA_TEXT)?.toString() ?: ""
        val subText = extras.getCharSequence(Notification.EXTRA_SUB_TEXT)?.toString() ?: ""

        if (TextUtils.isEmpty(text) && TextUtils.isEmpty(subText)) return

        val nextAction = when {
            text.isNotEmpty() -> text
            subText.isNotEmpty() -> subText
            else -> title
        }

        val distanceToNext = parseDistanceMeters(title + " " + text + " " + subText)

        val json = JSONObject()
        json.put("timestamp", System.currentTimeMillis() / 1000)
        json.put("next_action", nextAction)
        json.put("distance_to_next_m", distanceToNext)
        json.put("speed_kmh", lastSpeedKmh)
        json.put("lat", lastLat ?: JSONObject.NULL)
        json.put("lon", lastLon ?: JSONObject.NULL)

        NavServer.send(json)
    }

    private fun parseDistanceMeters(s: String): Int {
        val lower = s.lowercase()
        val regex = Regex("""(\d+[.,]?\d*)\s*(km|m)""")
        val m = regex.find(lower) ?: return 0
        val valueStr = m.groupValues[1].replace(',', '.')
        val unit = m.groupValues[2]
        val v = valueStr.toDoubleOrNull() ?: return 0
        return if (unit == "km") (v * 1000).toInt() else v.toInt()
    }

    // LocationListener
    override fun onLocationChanged(location: Location) {
        lastSpeedKmh = location.speed * 3.6
        lastLat = location.latitude
        lastLon = location.longitude
    }

    override fun onStatusChanged(provider: String?, status: Int, extras: Bundle?) {}
    override fun onProviderEnabled(provider: String) {}
    override fun onProviderDisabled(provider: String) {}
}