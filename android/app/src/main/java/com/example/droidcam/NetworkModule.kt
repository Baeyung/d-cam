package com.example.droidcam

import android.util.Log
import java.io.OutputStream
import java.net.Socket
import java.nio.ByteBuffer

class NetworkModule {
    private val TAG = "NetworkModule"
    private var socket: Socket? = null
    private var outputStream: OutputStream? = null
    @Volatile private var isRunning = false

    fun start() {
        if (isRunning) return
        isRunning = true
        Thread {
            while (isRunning) {
                try {
                    Log.d(TAG, "Connecting to localhost:${Config.PHONE_PORT}")
                    socket = Socket("127.0.0.1", Config.PHONE_PORT)
                    outputStream = socket?.getOutputStream()
                    Log.d(TAG, "Connected.")

                    // Connection loop
                    while (isRunning && socket?.isConnected == true) {
                        Thread.sleep(1000)
                    }
                } catch (e: Exception) {
                    Log.e(TAG, "Connection error: ${e.message}")
                    try { Thread.sleep(2000) } catch (ie: InterruptedException) { }
                } finally {
                    closeSocket()
                }
            }
        }.start()
    }

    fun stop() {
        isRunning = false
        closeSocket()
    }

    private fun closeSocket() {
        try { outputStream?.close() } catch (e: Exception) {}
        try { socket?.close() } catch (e: Exception) {}
        outputStream = null
        socket = null
    }

    fun sendConfig(spsPpsData: ByteArray) {
        sendMessage(Config.Protocol.TYPE_CONFIG, spsPpsData)
    }

    fun sendVideoFrame(timestamp: Long, frameData: ByteArray) {
        // Payload prefixed with 8-byte timestamp
        val payload = ByteBuffer.allocate(8 + frameData.size)
            .putLong(timestamp)
            .put(frameData)
            .array()
        sendMessage(Config.Protocol.TYPE_VIDEO_FRAME, payload)
    }

    fun sendHeartbeat() {
        sendMessage(Config.Protocol.TYPE_HEARTBEAT, ByteArray(0))
    }

    @Synchronized
    private fun sendMessage(type: Byte, payload: ByteArray) {
        val out = outputStream ?: return
        try {
            val header = ByteBuffer.allocate(9)
                .put(Config.Protocol.MAGIC)
                .put(type)
                .putInt(payload.size)
                .array()
            out.write(header)
            if (payload.isNotEmpty()) {
                out.write(payload)
            }
            out.flush()
        } catch (e: Exception) {
            Log.e(TAG, "Send error: ${e.message}")
            closeSocket() // Force reconnect on next loop
        }
    }
}
