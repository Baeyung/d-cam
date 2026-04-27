package com.example.droidcam

object Config {
    const val PHONE_PORT = 4747 // Typical port for such apps, but we make it configurable
    const val DESKTOP_PORT = 4747

    object Protocol {
        val MAGIC = byteArrayOf('D'.code.toByte(), 'R'.code.toByte(), 'O'.code.toByte(), 'I'.code.toByte())
        const val TYPE_CONFIG: Byte = 0x01
        const val TYPE_VIDEO_FRAME: Byte = 0x02
        const val TYPE_HEARTBEAT: Byte = 0x03
    }
}
