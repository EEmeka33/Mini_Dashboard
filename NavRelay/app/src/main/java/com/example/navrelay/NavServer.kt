package com.example.navrelay

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothServerSocket
import android.bluetooth.BluetoothSocket
import android.content.Context
import android.util.Log
import org.json.JSONObject
import java.io.OutputStream
import java.util.UUID
import java.util.concurrent.atomic.AtomicReference

object NavServer {

    private const val TAG = "NavRelay"
    private const val SERVICE_NAME = "NavJsonService"

    // UUID SPP, doit rester coherent avec ce que cherche nav-bind-daemon.sh
    val SPP_UUID: UUID =
        UUID.fromString("00001101-0000-1000-8000-00805F9B34FB")

    private val serverRef = AtomicReference<BluetoothServerSocket?>()
    private val socketRef = AtomicReference<BluetoothSocket?>()
    private val outRef = AtomicReference<OutputStream?>()

    @Volatile private var running = false

    /** Etat lisible affiche dans MainActivity */
    @Volatile var state: String = "Stopped (press Start)"
        private set

    /** Nombre de messages envoyes au Pi (pour verifier que ca circule) */
    @Volatile var sentCount: Int = 0
        private set

    @Volatile var lastMessage: String = ""
        private set

    val isRunning: Boolean get() = running
    val isConnected: Boolean get() = outRef.get() != null

    @SuppressLint("MissingPermission")
    fun start(context: Context) {
        if (running) return
        running = true
        Thread {
            // Boucle : si le serveur tombe (Bluetooth coupe/rallume...), on le relance
            while (running) {
                try {
                    val adapter = BluetoothAdapter.getDefaultAdapter()
                        ?: error("No Bluetooth adapter")
                    if (!adapter.isEnabled) {
                        state = "Bluetooth is OFF - turn it on"
                        Thread.sleep(2000)
                        continue
                    }

                    val server = adapter.listenUsingRfcommWithServiceRecord(SERVICE_NAME, SPP_UUID)
                    serverRef.set(server)
                    state = "Waiting for the Pi..."
                    Log.i(TAG, "RFCOMM server listening ($SERVICE_NAME)")

                    while (running) {
                        val client: BluetoothSocket = server.accept()
                        socketRef.getAndSet(client)?.let { runCatching { it.close() } }
                        outRef.getAndSet(client.outputStream)?.let { runCatching { it.close() } }
                        state = "Connected to ${client.remoteDevice?.name ?: "Pi"}"
                        Log.i(TAG, "Pi connected: ${client.remoteDevice?.address}")
                    }
                } catch (e: Exception) {
                    if (running) {
                        Log.w(TAG, "Server error, restarting in 2 s: ${e.message}")
                        state = "Error: ${e.message} (retrying)"
                        runCatching { Thread.sleep(2000) }
                    }
                } finally {
                    serverRef.getAndSet(null)?.let { runCatching { it.close() } }
                }
            }
        }.start()
    }

    fun stop() {
        running = false
        state = "Stopped (press Start)"
        dropClient()
        serverRef.getAndSet(null)?.let { runCatching { it.close() } }
    }

    /** Ferme seulement la connexion avec le Pi (le serveur continue d'ecouter) */
    private fun dropClient() {
        outRef.getAndSet(null)?.let { runCatching { it.close() } }
        socketRef.getAndSet(null)?.let { runCatching { it.close() } }
    }

    fun send(json: JSONObject) {
        val out = outRef.get()
        if (out == null) {
            Log.d(TAG, "Not sent (Pi not connected): $json")
            return
        }
        try {
            val line = json.toString() + "\n"
            out.write(line.toByteArray(Charsets.UTF_8))
            out.flush()
            sentCount++
            lastMessage = json.optString("next_action")
        } catch (e: Exception) {
            // Le Pi s'est deconnecte : on attend qu'il se reconnecte, SANS arreter
            // le serveur (avant, stop() tuait tout et il fallait rappuyer sur Start)
            Log.w(TAG, "Pi disconnected: ${e.message}")
            dropClient()
            if (running) state = "Waiting for the Pi..."
        }
    }
}
