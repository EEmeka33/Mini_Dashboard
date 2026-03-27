package com.example.navrelay

import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothServerSocket
import android.bluetooth.BluetoothSocket
import android.content.Context
import org.json.JSONObject
import java.io.OutputStream
import java.util.UUID
import java.util.concurrent.atomic.AtomicReference

object NavServer {

    private const val SERVICE_NAME = "NavJsonService"

    // UUID SPP custom, garde-le pareil dans la Pi ou dans sdptool []
    val SPP_UUID: UUID =
        UUID.fromString("00001101-0000-1000-8000-00805F9B34FB")

    private val socketRef = AtomicReference<BluetoothSocket?>()
    private val outRef = AtomicReference<OutputStream?>()

    @Volatile
    private var running = false

    fun start(context: Context) {
        if (running) return
        running = true
        Thread {
            try {
                val adapter = BluetoothAdapter.getDefaultAdapter()
                    ?: error("No BT adapter")
                if (!adapter.isEnabled) {
                    error("Bluetooth disabled")
                }

                val serverSock: BluetoothServerSocket =
                    adapter.listenUsingRfcommWithServiceRecord(SERVICE_NAME, SPP_UUID)

                while (running) {
                    val client: BluetoothSocket = serverSock.accept()
                    socketRef.getAndSet(client)?.close()
                    outRef.getAndSet(client.outputStream)?.close()
                }
            } catch (e: Exception) {
                e.printStackTrace()
                running = false
            }
        }.start()
    }

    fun stop() {
        running = false
        try { outRef.getAndSet(null)?.close() } catch (_: Exception) {}
        try { socketRef.getAndSet(null)?.close() } catch (_: Exception) {}
    }

    fun send(json: JSONObject) {
        val out = outRef.get() ?: return
        try {
            val line = json.toString() + "\n"
            out.write(line.toByteArray(Charsets.UTF_8))
            out.flush()
        } catch (e: Exception) {
            e.printStackTrace()
            stop()
        }
    }
}