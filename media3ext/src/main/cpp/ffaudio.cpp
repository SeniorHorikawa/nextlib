
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

        // Resample output.
        AVSampleFormat sampleFormat = context->sample_fmt;
        int channelCount = context->ch_layout.nb_channels;
        int sampleRate = context->sample_rate;
        int sampleCount = frame->nb_samples;
        int dataSize = av_samples_get_buffer_size(nullptr, channelCount, sampleCount,
                                                  sampleFormat, 1);
        // ★★★ FIX(2026-09-30)：这里原来是
        //     `int channelLayout = (int) context->ch_layout.u.mask;`
        //     + `swr_alloc()` + `av_opt_set_int(…, "in_channel_layout", channelLayout, …)`。
        //   `u.mask` 只在 `order == AV_CHANNEL_ORDER_NATIVE` 时才有意义；
        //   **≥9 声道走 `av_channel_layout_default()` 的 UNSPEC 分支，`u.mask` 恒为 0**
        //   （16 声道那条流被本机 ffmpeg 显示成 `hexadecagonal`，正是 UNSPEC-16 的名字）
        //   ⇒ 老代码会给 swr 传一个"未设置"的声道布局。
        //   官方 Media3 1.5.1 早已把这段换成 `swr_alloc_set_opts2(&…, &context->ch_layout, …)`，
        //   直接传 `AVChannelLayout` 结构（能表达 UNSPEC/CUSTOM），这里照抄官方实现。
        SwrContext *resampleContext = static_cast<SwrContext *>(context->opaque);
        if (!resampleContext) {
            // ★★★ FIX(2026-09-30)：**给 swr 一个显式的声道布局**。
            //   FFmpeg 对 >8 声道走 `av_channel_layout_default()` 的 **UNSPEC** 分支
            //   （没有具体声道定义，`u.mask` 也只是 0/无效），swresample 拿它算地址会崩 ——
            //   真机栈顶 `swr_convert+1892`、寄存器里还留着布局名 "hexadecagonal" 的 ASCII。
            //   这里 in/out 都用**同一个显式 NATIVE 布局（前 N 个声道全掩码）**：
            //   in==out ⇒ 声道映射恒等 ⇒ 只做"格式 + planar/interleaved"转换，语义与原来完全一致，
            //   但 swr 内部有了明确的声道定义，不再踩 UNSPEC。
            AVChannelLayout explicitLayout = {0};
            explicitLayout.order = AV_CHANNEL_ORDER_NATIVE;
            explicitLayout.nb_channels = channelCount;
            explicitLayout.u.mask = (channelCount >= 64) ? ~0ULL : ((1ULL << channelCount) - 1);
            result = swr_alloc_set_opts2(&resampleContext,             // ps
                                         &explicitLayout,               // out_ch_layout
                                         context->request_sample_fmt,  // out_sample_fmt
                                         sampleRate,                   // out_sample_rate
                                         &explicitLayout,               // in_ch_layout
                                         sampleFormat,                 // in_sample_fmt
                                         sampleRate,                   // in_sample_rate
                                         0,                            // log_offset
                                         nullptr                       // log_ctx
            );
            if (result < 0) {
                logError("swr_alloc_set_opts2", result);
                av_frame_free(&frame);
                return transformError(result);
            }
            result = swr_init(resampleContext);
            if (result < 0) {
                logError("swr_init", result);
                av_frame_free(&frame);
                return transformError(result);
            }
            context->opaque = resampleContext;
        }
        int inSampleSize = av_get_bytes_per_sample(sampleFormat);
        int outSampleSize = av_get_bytes_per_sample(context->request_sample_fmt);
        int outSamples = swr_get_out_samples(resampleContext, sampleCount);
        int bufferOutSize = outSampleSize * channelCount * outSamples;
        if (outSize + bufferOutSize > outputSize) {
            LOGD(
                    "Output buffer size (%d) too small for output data (%d), "
                    "reallocating buffer.",
                    outputSize, outSize + bufferOutSize);
            outputSize = outSize + bufferOutSize;
            outputBuffer = growBuffer(outputSize);
            if (!outputBuffer) {
                LOGE("Failed to reallocate output buffer.");
                av_frame_free(&frame);
                return AUDIO_DECODER_ERROR_OTHER;
            }
        }
        // ★★★ FIX(2026-09-30)：`swr_convert` 的第 3 个参数是 **每声道样本数**，
        //   原代码传的是 `bufferOutSize`（= outSampleSize * channelCount * outSamples，**字节数**）——
        //   多声道时被放大的倍数正是"每帧字节数"（16 声道 16-bit = 32 倍），
        //   swr 按这个错误容量做地址运算 → 真机 SIGSEGV（栈顶 swr_convert+1892）。
        //   低声道数时因为"输出样本数受输入样本数限制"侥幸不炸，所以上游长期未暴露。
        result = swr_convert(resampleContext, &outputBuffer, outSamples,
                             (const uint8_t **) frame->data, frame->nb_samples);
        av_frame_free(&frame);
        if (result < 0) {
            logError("swr_convert", result);
            return AUDIO_DECODER_ERROR_INVALID_DATA;
        }
        int available = swr_get_out_samples(resampleContext, 0);
        if (available != 0) {
            LOGE("Expected no samples remaining after resampling, but found %d.",
                 available);
            return AUDIO_DECODER_ERROR_INVALID_DATA;
        }
        outputBuffer += bufferOutSize;
        outSize += bufferOutSize;
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