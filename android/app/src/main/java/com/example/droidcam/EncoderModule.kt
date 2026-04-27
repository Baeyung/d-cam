package com.example.droidcam

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.util.Log
import android.view.Surface

class EncoderModule(private val networkModule: NetworkModule) {
    private val TAG = "EncoderModule"
    private var mediaCodec: MediaCodec? = null
    var inputSurface: Surface? = null
        private set
    @Volatile private var isRunning = false

    fun start(width: Int, height: Int, fps: Int, bitrate: Int) {
        if (isRunning) return
        try {
            val format = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, width, height)
            format.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            format.setInteger(MediaFormat.KEY_BIT_RATE, bitrate)
            format.setInteger(MediaFormat.KEY_FRAME_RATE, fps)
            format.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 2) // IDR every 2 seconds

            mediaCodec = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
            mediaCodec?.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            inputSurface = mediaCodec?.createInputSurface()

            mediaCodec?.setCallback(object : MediaCodec.Callback() {
                override fun onInputBufferAvailable(codec: MediaCodec, index: Int) {
                    // Not used with Surface input
                }

                override fun onOutputBufferAvailable(codec: MediaCodec, index: Int, info: MediaCodec.BufferInfo) {
                    val buffer = codec.getOutputBuffer(index) ?: return
                    val data = ByteArray(info.size)
                    buffer.get(data)

                    // If it's a config frame (SPS/PPS)
                    if ((info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0) {
                        Log.d(TAG, "Sending CONFIG frame: ${data.size} bytes")
                        networkModule.sendConfig(data)
                    } else {
                        // Send regular video frame
                        networkModule.sendVideoFrame(info.presentationTimeUs, data)
                    }
                    codec.releaseOutputBuffer(index, false)
                }

                override fun onError(codec: MediaCodec, e: MediaCodec.CodecException) {
                    Log.e(TAG, "Encoder error: ${e.message}")
                }

                override fun onOutputFormatChanged(codec: MediaCodec, format: MediaFormat) {
                    Log.d(TAG, "Output format changed: $format")
                }
            })

            mediaCodec?.start()
            isRunning = true
            Log.d(TAG, "Encoder started.")
        } catch (e: Exception) {
            Log.e(TAG, "Failed to start encoder: ${e.message}")
        }
    }

    fun stop() {
        if (!isRunning) return
        isRunning = false
        try {
            mediaCodec?.stop()
            mediaCodec?.release()
        } catch (e: Exception) {
            Log.e(TAG, "Error stopping encoder: ${e.message}")
        }
        mediaCodec = null
        inputSurface = null
        Log.d(TAG, "Encoder stopped.")
    }
}
