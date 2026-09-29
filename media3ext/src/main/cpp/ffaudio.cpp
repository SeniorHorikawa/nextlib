
#include <android/log.h>
#include <jni.h>
#include <cstdlib>
#include <cstdio>
#include <android/native_window_jni.h>
#include <algorithm>
#include "ffcommon.h"

extern "C" {
#ifdef __cplusplus
#define __STDC_CONSTANT_MACROS
#ifdef _STDINT_H
#undef _STDINT_H
#endif
#include <cstdint>
#endif
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/log.h>
#include <cstdarg>
#include <cstring>
#include <libswresample/swresample.h>
}

# define LOGD(...)  __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

// Output format corresponding to AudioFormat.ENCODING_PCM_16BIT.
static const AVSampleFormat OUTPUT_FORMAT_PCM_16BIT = AV_SAMPLE_FMT_S16;
// Output format corresponding to AudioFormat.ENCODING_PCM_FLOAT.
static const AVSampleFormat OUTPUT_FORMAT_PCM_FLOAT = AV_SAMPLE_FMT_FLT;

static const int AUDIO_DECODER_ERROR_INVALID_DATA = -1;
static const int AUDIO_DECODER_ERROR_OTHER = -2;

static jmethodID growOutputBufferMethod;


/**
 * Allocates and opens a new AVCodecContext for the specified codec, passing the
 * provided extraData as initialization data for the decoder if it is non-NULL.
 * Returns the created context.
 */
// ★ 诊断(2026-09-30)：把 FFmpeg 自己的 av_log 转发到 logcat（之前完全看不到 FFmpeg 说了什么）
static void mbAvLogCallback(void *avcl, int level, const char *fmt, va_list vl) {
    if (level > AV_LOG_INFO) return;
    char line[512];
    vsnprintf(line, sizeof(line), fmt, vl);
    __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "avlog: %s", line);
}

AVCodecContext *createContext(JNIEnv *env, AVCodec *codec, jbyteArray extraData,
                              jboolean outputFloat, jint rawSampleRate,
                              jint rawChannelCount);

/**
 * Decodes the packet into the output buffer, returning the number of bytes
 * written, or a negative AUDIO_DECODER_ERROR constant value in the case of an
 * error.
 */
// ★★★ FIX(2026-09-30)：**自己把 AVFrame 转成 interleaved PCM，不依赖 swresample。**
//   为什么：swresample 在 >8 声道（FFmpeg 给的是 AV_CHANNEL_ORDER_UNSPEC 布局）下必崩 ——
//   真机栈顶恒为 `swr_convert+1892`；换显式 NATIVE 布局也无效。
//   而这里只需要"采样率不变、声道数不变"的 **planar <-> interleaved + 位深转换**，
//   自己写就够，且彻底避开 swr 的声道布局处理。in/out 声道顺序恒等。
//   @return 写入字节数；负值 = 不支持/空间不足
static int copyFrameToInterleaved(const AVFrame *frame, uint8_t *dst, int dstSize,
                                  enum AVSampleFormat outFmt) {
    const int ch = frame->ch_layout.nb_channels;
    const int n = frame->nb_samples;
    if (ch <= 0 || n <= 0) return 0;
    if (outFmt != AV_SAMPLE_FMT_S16 && outFmt != AV_SAMPLE_FMT_FLT) return -1;
    const enum AVSampleFormat inFmt = (enum AVSampleFormat) frame->format;
    const int inB = av_get_bytes_per_sample(inFmt);
    const int outB = av_get_bytes_per_sample(outFmt);
    if (inB <= 0 || !frame->data[0]) return -2;
    const int need = n * ch * outB;
    if (need > dstSize) return -3;
    const int planar = av_sample_fmt_is_planar(inFmt);
    for (int i = 0; i < n; i++) {
        for (int c = 0; c < ch; c++) {
            const uint8_t *src = planar
                ? frame->data[c] + (size_t) i * inB
                : frame->data[0] + ((size_t) i * ch + c) * inB;
            if (!src) return -5;
            float v;
            switch (inFmt) {
                case AV_SAMPLE_FMT_FLTP: case AV_SAMPLE_FMT_FLT: { float t; memcpy(&t, src, 4); v = t; break; }
                case AV_SAMPLE_FMT_S16P: case AV_SAMPLE_FMT_S16: { int16_t t; memcpy(&t, src, 2); v = t / 32768.0f; break; }
                case AV_SAMPLE_FMT_S32P: case AV_SAMPLE_FMT_S32: { int32_t t; memcpy(&t, src, 4); v = (float) (t / 2147483648.0); break; }
                case AV_SAMPLE_FMT_DBLP: case AV_SAMPLE_FMT_DBL: { double t; memcpy(&t, src, 8); v = (float) t; break; }
                case AV_SAMPLE_FMT_U8P:  case AV_SAMPLE_FMT_U8:  v = (*src - 128.0f) / 128.0f; break;
                default: return -4;
            }
            uint8_t *op = dst + ((size_t) i * ch + c) * outB;
            if (outFmt == AV_SAMPLE_FMT_S16) {
                float scaled = v * 32767.0f;
                int si = (int) (scaled + (scaled >= 0 ? 0.5f : -0.5f));
                if (si > 32767) si = 32767; else if (si < -32768) si = -32768;
                int16_t o = (int16_t) si;
                memcpy(op, &o, 2);
            } else {
                memcpy(op, &v, 4);
            }
        }
    }
    return need;
}

int decodePacket(AVCodecContext *context, AVPacket *packet,
                 uint8_t *outputBuffer, int outputSize);

/**
 * Transforms ffmpeg AVERROR into a negative AUDIO_DECODER_ERROR constant value.
 */
int transformError(int errorNumber);

struct GrowOutputBufferCallback {
    uint8_t *operator()(int requiredSize) const;

    JNIEnv *env;
    jobject thiz;
    jobject decoderOutputBuffer;
};

uint8_t *GrowOutputBufferCallback::operator()(int requiredSize) const {
    jobject newOutputData = env->CallObjectMethod(thiz, growOutputBufferMethod, decoderOutputBuffer, requiredSize);
    if (env->ExceptionCheck()) {
        LOGE("growOutputBuffer() failed");
        env->ExceptionDescribe();
        return nullptr;
    }
    return static_cast<uint8_t *>(env->GetDirectBufferAddress(newOutputData));
}

AVCodecContext *createContext(JNIEnv *env, AVCodec *codec, jbyteArray extraData,
                              jboolean outputFloat, jint rawSampleRate,
                              jint rawChannelCount) {
    av_log_set_callback(mbAvLogCallback);
    AVCodecContext *context = avcodec_alloc_context3(codec);
    if (!context) {
        LOGE("Failed to allocate context.");
        return nullptr;
    }
    context->request_sample_fmt =
            outputFloat ? OUTPUT_FORMAT_PCM_FLOAT : OUTPUT_FORMAT_PCM_16BIT;
    if (extraData) {
        jsize size = env->GetArrayLength(extraData);
        context->extradata_size = size;
        context->extradata =
                (uint8_t *) av_malloc(size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!context->extradata) {
            LOGE("Failed to allocate extra data.");
            releaseContext(context);
            return nullptr;
        }
        env->GetByteArrayRegion(extraData, 0, size, (jbyte *) context->extradata);
    }
    if (context->codec_id == AV_CODEC_ID_PCM_MULAW ||
        context->codec_id == AV_CODEC_ID_PCM_ALAW) {
        context->sample_rate = rawSampleRate;
        context->ch_layout.nb_channels = rawChannelCount;
        av_channel_layout_default(&context->ch_layout, rawChannelCount);
    }
    // ★ 诊断(2026-09-30)：**不再吞掉解码器错误**。原值 AV_EF_IGNORE_ERR 会把解码器报的错
    //   全部静默掉 —— 如果 Vorbis 每帧都失败，就会表现成"无帧、无错"（正是我们查到的现象）。
    //   改成 0（默认）让 FFmpeg 把真正的原因打进 logcat。确认原因后再决定是否恢复。
    context->err_recognition = 0;
    int result = avcodec_open2(context, codec, nullptr);
    if (result < 0) {
        logError("avcodec_open2", result);
        releaseContext(context);
        return nullptr;
    }
    // ★ 诊断(2026-09-30)：确认 open 真的成功，并留下解码器的关键状态
    LOGD("diag: avcodec_open2 OK | codec=%s ch=%d rate=%d extradata=%d sample_fmt=%d",
         codec->name, context->ch_layout.nb_channels, context->sample_rate,
         context->extradata_size, context->sample_fmt);
    return context;
}

int decodePacket(AVCodecContext *context, AVPacket *packet,
                 uint8_t *outputBuffer, int outputSize, GrowOutputBufferCallback growBuffer) {
    int result = 0;
    // ★ 诊断(2026-09-30)：头 8 个输入包打 size + 首 12 字节。
    //   判读：首字节 01/03/05 + "vorbis" ⇒ 喂进去的其实是三段头（上游解封装的问题）；
    //         size=0 或极小 ⇒ 空包；其余 ⇒ 包本身正常，问题在解码器侧。
    {
        static int diagIn = 0;
        if (diagIn < 8) {
            const uint8_t *d = (const uint8_t *) packet->data;
            int n = packet->size;
            char first[64] = {0};
            int lim = n < 12 ? n : 12;
            if (lim < 0) lim = 0;
            for (int k = 0; k < lim; k++) {
                snprintf(first + k * 3, 4, "%02x ", d[k]);
            }
            LOGD("diag: 输入包 #%d size=%d 首%d字节=%s", diagIn + 1, n, lim, first);
            diagIn++;
        }
    }
    // Queue input data.
    result = avcodec_send_packet(context, packet);
    if (result) {
        logError("avcodec_send_packet", result);
        return transformError(result);
    }
    {
        static int diagSend = 0;
        if (diagSend++ < 3) {
            LOGD("diag: send_packet OK #%d", diagSend);
        }
    }

    // Dequeue output data until it runs out.
    int outSize = 0;
    // ★ 诊断（2026-09-30）：定位"解码器吃了输入却不出帧"。
    //   Java 侧只能看到 `入队>0 / 出帧=0`，看不到 native 为什么不出 —— 这几行就是补那个洞。
    //   每条计数只打有限条，避免刷屏（与工程里"高频日志必须去抖"同一条原则）。
    static int diagEagain = 0;
    static int diagFrames = 0;
    while (true) {
        AVFrame *frame = av_frame_alloc();
        if (!frame) {
            LOGE("Failed to allocate output frame.");
            return AUDIO_DECODER_ERROR_INVALID_DATA;
        }
        result = avcodec_receive_frame(context, frame);
        if (result) {
            av_frame_free(&frame);
            if (result == AVERROR(EAGAIN)) {
                if (diagEagain++ < 5) {
                    LOGD("diag: receive_frame=EAGAIN #%d | codec_id=%d sample_fmt=%d "
                         "ch=%d rate=%d extradata=%d req_fmt=%d",
                         diagEagain, context->codec_id, context->sample_fmt,
                         context->ch_layout.nb_channels, context->sample_rate,
                         context->extradata_size, context->request_sample_fmt);
                }
                break;
            }
            logError("avcodec_receive_frame", result);
            return transformError(result);
        }
        if (diagFrames++ < 3) {
            LOGD("diag: 出帧 #%d nb_samples=%d fmt=%d ch=%d", diagFrames,
                 frame->nb_samples, frame->format, frame->ch_layout.nb_channels);
        }

        // ★★★ FIX(2026-09-30)：**绕开 swresample**（它对 >8 声道必崩）。
        //   解码器输出即目标采样率与声道数，只需 planar/interleaved + 位深转换。
        {
            const int outB = av_get_bytes_per_sample(context->request_sample_fmt);
            const int need = frame->nb_samples * channelCount * outB;
            if (need > 0 && outSize + need > outputSize) {
                // growBuffer 返回**缓冲区基址**，所以要把已写部分 outSize 的偏移加回去。
                outputBuffer = growBuffer(outSize + need);
                if (!outputBuffer) {
                    LOGE("Failed to reallocate output buffer.");
                    av_frame_free(&frame);
                    return AUDIO_DECODER_ERROR_OTHER;
                }
            }
            int written = copyFrameToInterleaved(frame, outputBuffer + outSize, outputSize - outSize,
                                                 context->request_sample_fmt);
            if (written < 0) {
                LOGE("convert to interleaved failed: %d", written);
                av_frame_free(&frame);
                return AUDIO_DECODER_ERROR_INVALID_DATA;
            }
            av_frame_free(&frame);
            outSize += written;
        }
    }
    return outSize;
}

int transformError(int errorNumber) {
    return errorNumber == AVERROR_INVALIDDATA ? AUDIO_DECODER_ERROR_INVALID_DATA
                                              : AUDIO_DECODER_ERROR_OTHER;
}

extern "C"
JNIEXPORT jlong JNICALL
Java_io_github_anilbeesetti_nextlib_media3ext_ffdecoder_FfmpegAudioDecoder_ffmpegInitialize(JNIEnv *env,
                                                                        jobject thiz,
                                                                        jstring codec_name,
                                                                        jbyteArray extra_data,
                                                                        jboolean output_float,
                                                                        jint raw_sample_rate,
                                                                        jint raw_channel_count) {
    AVCodec *codec = getCodecByName(env, codec_name);
    if (!codec) {
        LOGE("Codec not found.");
        return 0L;
    }
    jclass clazz = env->FindClass("io/github/anilbeesetti/nextlib/media3ext/ffdecoder/FfmpegAudioDecoder");
    growOutputBufferMethod = env->GetMethodID(clazz, "growOutputBuffer","(Landroidx/media3/decoder/SimpleDecoderOutputBuffer;I)Ljava/nio/ByteBuffer;");
    return (jlong) createContext(env, codec, extra_data, output_float, raw_sample_rate,
                                 raw_channel_count);
}

extern "C"
JNIEXPORT jint JNICALL
Java_io_github_anilbeesetti_nextlib_media3ext_ffdecoder_FfmpegAudioDecoder_ffmpegDecode(JNIEnv *env,
                                                                    jobject thiz,
                                                                    jlong context,
                                                                    jobject input_data,
                                                                    jint input_size,
                                                                    jobject decoderOutputBuffer,
                                                                    jobject output_data,
                                                                    jint output_size) {
    if (!context) {
        LOGE("Context must be non-NULL.");
        return -1;
    }
    if (!input_data || !decoderOutputBuffer || !output_data) {
        LOGE("Input and output buffers must be non-NULL.");
        return -1;
    }
    if (input_size < 0) {
        LOGE("Invalid input buffer size: %d.", input_size);
        return -1;
    }
    if (output_size < 0) {
        LOGE("Invalid output buffer length: %d", output_size);
        return -1;
    }
    auto *inputBuffer = (uint8_t *) env->GetDirectBufferAddress(input_data);
    auto *outputBuffer = (uint8_t *) env->GetDirectBufferAddress(output_data);
    AVPacket *packet;
    packet = av_packet_alloc();

    if (packet == nullptr) {
        LOGE("audio_decoder_decode_frame: av_packet_alloc failed");
        return -1;
    }

    packet->data = inputBuffer;
    packet->size = input_size;
    int decodedPacket = decodePacket((AVCodecContext *) context, packet, outputBuffer,
                                     output_size, GrowOutputBufferCallback{env, thiz, decoderOutputBuffer});
    av_packet_free(&packet);
    return decodedPacket;
}

extern "C"
JNIEXPORT jint JNICALL
Java_io_github_anilbeesetti_nextlib_media3ext_ffdecoder_FfmpegAudioDecoder_ffmpegGetChannelCount(
        JNIEnv *env, jobject thiz, jlong context) {
    if (!context) {
        LOGE("Context must be non-NULL.");
        return -1;
    }
    return ((AVCodecContext *) context)->ch_layout.nb_channels;
}

extern "C"
JNIEXPORT jint JNICALL
Java_io_github_anilbeesetti_nextlib_media3ext_ffdecoder_FfmpegAudioDecoder_ffmpegGetSampleRate(JNIEnv *env,
                                                                           jobject thiz,
                                                                           jlong context) {
    if (!context) {
        LOGE("Context must be non-NULL.");
        return -1;
    }
    return ((AVCodecContext *) context)->sample_rate;
}

extern "C"
JNIEXPORT jlong JNICALL
Java_io_github_anilbeesetti_nextlib_media3ext_ffdecoder_FfmpegAudioDecoder_ffmpegReset(JNIEnv *env,
                                                                   jobject thiz,
                                                                   jlong jContext,
                                                                   jbyteArray extra_data) {
    auto *context = (AVCodecContext *) jContext;
    if (!context) {
        LOGE("Tried to reset without a context.");
        return 0L;
    }

    AVCodecID codecId = context->codec_id;
    if (codecId == AV_CODEC_ID_TRUEHD) {
        // Release and recreate the context if the codec is TrueHD.
        // TODO: Figure out why flushing doesn't work for this codec.
        releaseContext(context);
        auto *codec = const_cast<AVCodec *>(avcodec_find_decoder(codecId));
        if (!codec) {
            LOGE("Unexpected error finding codec %d.", codecId);
            return 0L;
        }
        auto outputFloat =
                (jboolean) (context->request_sample_fmt == OUTPUT_FORMAT_PCM_FLOAT);
        return (jlong) createContext(env, codec, extra_data, outputFloat,
                /* rawSampleRate= */ -1,
                /* rawChannelCount= */ -1);
    }

    avcodec_flush_buffers(context);
    return (jlong) context;
}

extern "C"
JNIEXPORT void JNICALL
Java_io_github_anilbeesetti_nextlib_media3ext_ffdecoder_FfmpegAudioDecoder_ffmpegRelease(JNIEnv *env,
                                                                     jobject thiz,
                                                                     jlong context) {
    if (context) {
        releaseContext((AVCodecContext *) context);
    }
}