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
import android.util.Log
import org.json.JSONObject

/**
 * Lit la notification de navigation Google Maps et l'envoie au Pi en JSON.
 *
 * Exemple de notification Maps (FR) :
 *   titre : "300 m"                       <- distance avant la prochaine action
 *   texte : "Tournez a droite sur Rue X"  <- prochaine action
 *   sous-texte / texte : "12 min · 4,5 km · Arrivee a 14:32"  <- ETA (a NE PAS
 *                                           prendre pour l'action ni la distance)
 * Selon la version de Maps, la distance peut aussi etre dans la meme ligne que
 * l'action ("300 m · Tournez a droite"), d'ou le classement ci-dessous.
 *
 * Debug : adb logcat -s NavRelay
 */
class MapsNotificationListener :
    NotificationListenerService(),
    LocationListener {

    companion object {
        private const val TAG = "NavRelay"
        private const val MAPS_PKG = "com.google.android.apps.maps"

        // Espaces "normales" + insecables (Maps FR utilise U+00A0 / U+202F entre 300 et m)
        private const val SP = """[\s  ]"""

        // Distance : 300 m, 1,2 km, 0.5 mi, 500 ft... ; (?!\p{L}) evite de prendre "12 min" pour "12 m"
        private val DIST_RE = Regex(
            """(?<![\d.,])(\d+(?:[.,]\d+)?)$SP*(km|m|mi|ft|yd)(?!\p{L})""",
            RegexOption.IGNORE_CASE
        )

        // Lignes d'ETA : heure (14:32, 14 h 32), duree (12 min, 1 h 05) ou mot "arrivee"
        private val ETA_RE = Regex(
            """\b\d{1,2}$SP?[:h]$SP?\d{2}\b|\b\d+$SP*(min|h|hr|hrs)\b|arriv|\beta\b|heure d'arriv""",
            RegexOption.IGNORE_CASE
        )

        // Separateurs entre distance et instruction : "300 m · Tournez", "300 m - Tournez"
        private val LEADING_SEP_RE = Regex("""^[\s  ·•\-–—,:]+""")
    }

    private var lastSpeedKmh: Double = 0.0
    private var lastLat: Double? = null
    private var lastLon: Double? = null
    private var lastSentKey: String? = null

    override fun onCreate() {
        super.onCreate()
        requestLocationUpdates()
    }

    @SuppressLint("MissingPermission")
    private fun requestLocationUpdates() {
        val lm = getSystemService(Context.LOCATION_SERVICE) as LocationManager
        try {
            lm.requestLocationUpdates(LocationManager.GPS_PROVIDER, 1000L, 1f, this)
        } catch (e: SecurityException) {
            e.printStackTrace()
        }
    }

    /** Toujours getCharSequence : Maps met du texte formate (Spannable) -> getString() renvoie null. */
    private fun Bundle.text(key: String): String =
        getCharSequence(key)?.toString()?.trim().orEmpty()

    override fun onNotificationPosted(sbn: StatusBarNotification) {
        if (sbn.packageName != MAPS_PKG) return
        val extras = sbn.notification?.extras ?: return

        val title = extras.text(Notification.EXTRA_TITLE)
        val titleBig = extras.text(Notification.EXTRA_TITLE_BIG)
        val text = extras.text(Notification.EXTRA_TEXT)
        val bigText = extras.text(Notification.EXTRA_BIG_TEXT)
        val subText = extras.text(Notification.EXTRA_SUB_TEXT)
        val infoText = extras.text(Notification.EXTRA_INFO_TEXT)
        val lines = extras.getCharSequenceArray(Notification.EXTRA_TEXT_LINES)
            ?.map { it.toString().trim() }.orEmpty()

        Log.d(TAG, "title='$title' titleBig='$titleBig' text='$text' bigText='$bigText' " +
                "sub='$subText' info='$infoText' lines=$lines")

        // Ordre = priorite (le titre porte en general la distance)
        val candidates = (listOf(title, titleBig, text, bigText) + lines + listOf(subText, infoText))
            .filter { it.isNotEmpty() }
            .distinct()
        if (candidates.isEmpty()) return

        val (etaLines, navLines) = candidates.partition { isEta(it) }

        // 1. Distance : uniquement dans les lignes qui ne sont pas l'ETA
        //    (sinon on prendrait la distance totale restante "4,5 km")
        var distanceM = -1
        for (line in navLines) {
            val d = parseDistanceMeters(line)
            if (d >= 0) { distanceM = d; break }
        }

        // 2. Action : premiere ligne non-ETA qui contient autre chose qu'une distance
        var nextAction = ""
        for (line in navLines) {
            val rest = stripLeadingDistance(line)
            if (rest.isNotEmpty() && !isEta(rest)) { nextAction = rest; break }
        }

        val eta = etaLines.firstOrNull().orEmpty()

        // Pas d'action ni de distance (ex : notification "Navigation en cours") -> on ignore
        if (nextAction.isEmpty() && distanceM < 0) return

        val json = JSONObject()
        json.put("timestamp", System.currentTimeMillis() / 1000)
        json.put("next_action", nextAction)
        json.put("distance_to_next_m", distanceM)   // -1 = inconnue
        json.put("eta", eta)
        json.put("speed_kmh", lastSpeedKmh)
        json.put("lat", lastLat ?: JSONObject.NULL)
        json.put("lon", lastLon ?: JSONObject.NULL)

        val key = "$nextAction|$distanceM|$eta"
        if (key != lastSentKey) Log.i(TAG, "-> next='$nextAction' dist=$distanceM eta='$eta'")
        lastSentKey = key

        NavServer.send(json)
    }

    private fun isEta(s: String): Boolean = ETA_RE.containsMatchIn(s)

    /** "300 m · Tournez a droite" -> "Tournez a droite" ; "300 m" -> "" */
    private fun stripLeadingDistance(s: String): String {
        val m = DIST_RE.find(s) ?: return s.trim()
        if (s.substring(0, m.range.first).isNotBlank()) return s.trim()   // distance pas en tete
        return s.substring(m.range.last + 1).replace(LEADING_SEP_RE, "").trim()
    }

    /** Retourne la distance en metres, ou -1 si aucune distance n'est trouvee. */
    private fun parseDistanceMeters(s: String): Int {
        val m = DIST_RE.find(s) ?: return -1
        val v = m.groupValues[1].replace(',', '.').toDoubleOrNull() ?: return -1
        return when (m.groupValues[2].lowercase()) {
            "km" -> (v * 1000).toInt()
            "mi" -> (v * 1609.344).toInt()
            "ft" -> (v * 0.3048).toInt()
            "yd" -> (v * 0.9144).toInt()
            else -> v.toInt()
        }
    }

    // LocationListener
    override fun onLocationChanged(location: Location) {
        lastSpeedKmh = location.speed * 3.6
        lastLat = location.latitude
        lastLon = location.longitude
    }

    @Deprecated("Deprecated in Java")
    override fun onStatusChanged(provider: String?, status: Int, extras: Bundle?) {}
    override fun onProviderEnabled(provider: String) {}
    override fun onProviderDisabled(provider: String) {}
}
