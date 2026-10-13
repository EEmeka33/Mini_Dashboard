package com.example.navrelay

import android.Manifest
import android.content.ComponentName
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.service.notification.NotificationListenerService
import android.widget.Button
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.content.ContextCompat

class MainActivity : AppCompatActivity() {

    private lateinit var statusText: TextView
    private val handler = Handler(Looper.getMainLooper())

    private val notifPermLauncher =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { }

    /** Rafraichit l'etat affiche toutes les secondes */
    private val refresh = object : Runnable {
        override fun run() {
            statusText.text = buildStatus()
            handler.postDelayed(this, 1000)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        statusText = findViewById(R.id.statusText)
        val startButton: Button = findViewById(R.id.startButton)

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            val granted = ContextCompat.checkSelfPermission(
                this, Manifest.permission.POST_NOTIFICATIONS
            ) == PackageManager.PERMISSION_GRANTED
            if (!granted) notifPermLauncher.launch(Manifest.permission.POST_NOTIFICATIONS)
        }

        val perms = mutableListOf(
            Manifest.permission.ACCESS_FINE_LOCATION,
            Manifest.permission.ACCESS_COARSE_LOCATION
        )
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            perms += listOf(
                Manifest.permission.BLUETOOTH_CONNECT,
                Manifest.permission.BLUETOOTH_ADVERTISE,
                Manifest.permission.BLUETOOTH_SCAN
            )
        }
        val missing = perms.filter {
            ActivityCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isNotEmpty()) {
            ActivityCompat.requestPermissions(this, missing.toTypedArray(), 1234)
        }

        startButton.setOnClickListener {
            NavServer.start(this)
            if (hasNotificationAccess()) {
                // Apres une reinstallation (Run depuis Android Studio), Android ne
                // relie souvent pas le service de notifications : on le force.
                NotificationListenerService.requestRebind(
                    ComponentName(this, MapsNotificationListener::class.java)
                )
            } else {
                startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS))
            }
        }
    }

    override fun onResume() {
        super.onResume()
        handler.post(refresh)
    }

    override fun onPause() {
        super.onPause()
        handler.removeCallbacks(refresh)
    }

    private fun hasNotificationAccess(): Boolean =
        NotificationManagerCompat.getEnabledListenerPackages(this).contains(packageName)

    private fun buildStatus(): String {
        val access = hasNotificationAccess()
        val sb = StringBuilder()
        sb.append("1. Notification access: ")
            .append(if (access) "OK" else "MISSING - press Start and enable NavRelay")
            .append('\n')
        sb.append("2. Maps listener: ")
            .append(
                when {
                    !access -> "-"
                    MapsNotificationListener.connected -> "bound"
                    else -> "NOT bound - disable then re-enable NavRelay in notification access"
                }
            ).append('\n')
        sb.append("3. Bluetooth server: ").append(NavServer.state).append('\n')
        sb.append("4. Messages sent to Pi: ").append(NavServer.sentCount)
        val lastSeen = MapsNotificationListener.lastParsed
        if (lastSeen.isNotEmpty()) sb.append("\n\nLast Maps info:\n").append(lastSeen)
        return sb.toString()
    }
}
