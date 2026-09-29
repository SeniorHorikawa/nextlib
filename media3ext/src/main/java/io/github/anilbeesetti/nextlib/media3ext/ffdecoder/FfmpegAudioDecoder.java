package io.github.anilbeesetti.nextlib.media3ext.ffdecoder;

import static androidx.media3.common.util.Assertions.checkNotNull;

import android.annotation.SuppressLint;

import androidx.annotation.Keep;
import androidx.annotation.Nullable;
import androidx.media3.common.C;
import androidx.media3.common.Format;
import androidx.media3.common.MimeTypes;
import androidx.media3.common.util.ParsableByteArray;
import androidx.media3.common.util.Util;
import androidx.media3.decoder.DecoderInputBuffer;
import androidx.media3.decoder.SimpleDecoder;
import androidx.media3.decoder.SimpleDecoderOutputBuffer;

import java.nio.ByteBuffer;
import java.util.List;

/** FFmpeg audio decoder. */
/* package */
@SuppressLint("UnsafeOptInUsageError")
final class FfmpegAudioDecoder
    extends SimpleDecoder<DecoderInputBuffer, SimpleDecoderOutputBuffer, FfmpegDecoderException> {

  // Output buffer sizes when decoding PCM mu-law streams, which is the maximum FFmpeg outputs.
  // ★ FIX(2026-09-30): 原值 65535/131070 是按 <=8 声道取的，16 声道 x 2048 块长需要
  //   65536 / 131072 —— 恰好差 1~2 字节；装不下时 native 返回 0，被 decode() 当作
  //   "无需输出"静默跳过（且 growOutputBuffer 的调用路径形同虚设）。抬到能覆盖 64 声道。
  private static final int INITIAL_OUTPUT_BUFFER_SIZE_16BIT = 1 << 18;
  private static final int INITIAL_OUTPUT_BUFFER_SIZE_32BIT = INITIAL_OUTPUT_BUFFER_SIZE_16BIT * 2;

  private static final int AUDIO_DECODER_ERROR_INVALID_DATA = -1;
  private static final int AUDIO_DECODER_ERROR_OTHER = -2;

  private final String codecName;
  @Nullable private final byte[] extraData;
  private final @C.PcmEncoding int encoding;
  private int outputBufferSize;

  private long nativeContext; // May be reassigned on resetting the codec.
  private boolean hasOutputFormat;
  private volatile int channelCount;
  private volatile int sampleRate;

  public FfmpegAudioDecoder(
      Format format,
      int numInputBuffers,
      int numOutputBuffers,
      int initialInputBufferSize,
      boolean outputFloat)
      throws FfmpegDecoderException {
    super(new DecoderInputBuffer[numInputBuffers], new SimpleDecoderOutputBuffer[numOutputBuffers]);
    if (!FfmpegLibrary.isAvailable()) {
      throw new FfmpegDecoderException("Failed to load decoder native libraries.");
    }
    checkNotNull(format.sampleMimeType);
    codecName = checkNotNull(FfmpegLibrary.getCodecName(format.sampleMimeType));
    extraData = getExtraData(format.sampleMimeType, format.initializationData);
    encoding = outputFloat ? C.ENCODING_PCM_FLOAT : C.ENCODING_PCM_16BIT;
    outputBufferSize = outputFloat ? INITIAL_OUTPUT_BUFFER_SIZE_32BIT : INITIAL_OUTPUT_BUFFER_SIZE_16BIT;
    nativeContext =
        ffmpegInitialize(codecName, extraData, outputFloat, format.sampleRate, format.channelCount);
    if (nativeContext == 0) {
      throw new FfmpegDecoderException("Initialization failed.");
    }
    setInitialInputBufferSize(initialInputBufferSize);
  }

  @Override
  public String getName() {
    return "ffmpeg" + FfmpegLibrary.getVersion() + "-" + codecName;
  }

  @Override
  protected DecoderInputBuffer createInputBuffer() {
    return new DecoderInputBuffer(
        DecoderInputBuffer.BUFFER_REPLACEMENT_MODE_DIRECT,
        FfmpegLibrary.getInputBufferPaddingSize());
  }

  @Override
  protected SimpleDecoderOutputBuffer createOutputBuffer() {
    return new SimpleDecoderOutputBuffer(this::releaseOutputBuffer);
  }

  @Override
  protected FfmpegDecoderException createUnexpectedDecodeException(Throwable error) {
    return new FfmpegDecoderException("Unexpected decode error", error);
  }

  @Override
  @Nullable
  protected FfmpegDecoderException decode(
      DecoderInputBuffer inputBuffer, SimpleDecoderOutputBuffer outputBuffer, boolean reset) {
    if (reset) {
      nativeContext = ffmpegReset(nativeContext, extraData);
      if (nativeContext == 0) {
        return new FfmpegDecoderException("Error resetting (see logcat).");
      }
    }
    ByteBuffer inputData = Util.castNonNull(inputBuffer.data);
    int inputSize = inputData.limit();
    ByteBuffer outputData = outputBuffer.init(inputBuffer.timeUs, outputBufferSize);
    int result = ffmpegDecode(nativeContext, inputData, inputSize, outputBuffer, outputData, outputBufferSize);
    if (result == AUDIO_DECODER_ERROR_OTHER) {
      return new FfmpegDecoderException("Error decoding (see logcat).");
    } else if (result == AUDIO_DECODER_ERROR_INVALID_DATA) {
      // Treat invalid data errors as non-fatal to match the behavior of MediaCodec. No output will
      // be produced for this buffer, so mark it as decode-only to ensure that the audio sink's
      // position is reset when more audio is produced.
      outputBuffer.shouldBeSkipped = true;
      return null;
    } else if (result == 0) {
      // There's no need to output empty buffers.
      outputBuffer.shouldBeSkipped = true;
      return null;
    }
    if (!hasOutputFormat) {
      channelCount = ffmpegGetChannelCount(nativeContext);
      sampleRate = ffmpegGetSampleRate(nativeContext);
      if (sampleRate == 0 && "alac".equals(codecName)) {
        checkNotNull(extraData);
        // ALAC decoder did not set the sample rate in earlier versions of FFmpeg. See
        // https://trac.ffmpeg.org/ticket/6096.
        ParsableByteArray parsableExtraData = new ParsableByteArray(extraData);
        parsableExtraData.setPosition(extraData.length - 4);
        sampleRate = parsableExtraData.readUnsignedIntToInt();
      }
      hasOutputFormat = true;
    }
    // Get a new reference to the output ByteBuffer in case the native decode method reallocated the
    // buffer to grow its size.
    outputData = checkNotNull(outputBuffer.data);
    outputData.position(0);
    outputData.limit(result);
    return null;
  }

  // Called from native code
  /** @noinspection unused*/
  @Keep
  private ByteBuffer growOutputBuffer(SimpleDecoderOutputBuffer outputBuffer, int requiredSize) {
    // Use it for new buffer so that hopefully we won't need to reallocate again
    outputBufferSize = requiredSize;
    return outputBuffer.grow(requiredSize);
  }

  @Override
  public void release() {
    super.release();
    ffmpegRelease(nativeContext);
    nativeContext = 0;
  }

  /** Returns the channel count of output audio. */
  public int getChannelCount() {
    return channelCount;
  }

  /** Returns the sample rate of output audio. */
  public int getSampleRate() {
    return sampleRate;
  }

  /** Returns the encoding of output audio. */
  public @C.PcmEncoding int getEncoding() {
    return encoding;
  }

  /**
   * Returns FFmpeg-compatible codec-specific initialization data ("extra data"), or {@code null} if
   * not required.
   */
  @Nullable
  private static byte[] getExtraData(String mimeType, List<byte[]> initializationData) {
    if (initializationData.isEmpty()) return null;
    return switch (mimeType) {
      case MimeTypes.AUDIO_AAC, MimeTypes.AUDIO_OPUS -> initializationData.get(0);
      case MimeTypes.AUDIO_ALAC -> getAlacExtraData(initializationData);
      case MimeTypes.AUDIO_VORBIS -> getVorbisExtraData(initializationData);
      // Other codecs do not require extra data.
      default -> null;
    };
  }

  private static byte[] getAlacExtraData(List<byte[]> initializationData) {
    // FFmpeg's ALAC decoder expects an ALAC atom, which contains the ALAC "magic cookie", as extra
    // data. initializationData[0] contains only the magic cookie, and so we need to package it into
    // an ALAC atom. See:
    // https://ffmpeg.org/doxygen/0.6/alac_8c.html
    // https://github.com/macosforge/alac/blob/master/ALACMagicCookieDescription.txt
    byte[] magicCookie = initializationData.get(0);
    int alacAtomLength = 12 + magicCookie.length;
    ByteBuffer alacAtom = ByteBuffer.allocate(alacAtomLength);
    alacAtom.putInt(alacAtomLength);
    alacAtom.putInt(0x616c6163); // type=alac
    alacAtom.putInt(0); // version=0, flags=0
    alacAtom.put(magicCookie, /* offset= */ 0, magicCookie.length);
    return alacAtom.array();
  }

  /**
   * Vorbis extradata —— ★ FIX(2026-09-30)。
   *
   * FFmpeg 期望的布局是 {@code [头数-1][len1][len2][头1][头2][头3]}（与 ffmpeg 自己
   * ogg→mkv 时写出的 CodecPrivate 一致）。原实现写成
   * {@code [len(id)][id][00 00][len(setup)][setup]}，而 FFmpeg 是用
   * {@code overhead = extraData[2..3]} 去定位**第三段头**的 —— 它把 id 头放在 offset 2，
   * 于是 overhead 恒等于 id 头前两字节 {@code 0x01 0x76} = 374，第三段头被指到 offset 406，
   * 落在 setup 头**内部**，FFmpeg 直接报
   * {@code Third header is not the setup header} / {@code Setup header is too short}。
   * 结果是解码器 avcodec_open2 成功、却一帧都不出（也没有异常），
   * 上层表现为"永远无声"。≥16 声道的 Vorbis 才会踩到（<=8 声道由平台解码器处理，不走这条路）。
   *
   * Media3 只提供 id + setup 两段头，所以这里补一个最小合法的 comment 头占位。
   */
  private static byte[] getVorbisExtraData(List<byte[]> initializationData) {
    byte[] idHeader = initializationData.get(0);
    byte[] setupHeader = initializationData.get(1);
    byte[] commentHeader = minimalVorbisCommentHeader();
    byte[] extraData =
        new byte[3 + idHeader.length + commentHeader.length + setupHeader.length];
    extraData[0] = 2; // 头数 - 1
    extraData[1] = (byte) idHeader.length; // 1 字节长度字段 => 必须 <= 255
    extraData[2] = (byte) commentHeader.length;
    System.arraycopy(idHeader, 0, extraData, 3, idHeader.length);
    System.arraycopy(commentHeader, 0, extraData, 3 + idHeader.length, commentHeader.length);
    System.arraycopy(
        setupHeader, 0, extraData, 3 + idHeader.length + commentHeader.length, setupHeader.length);
    return extraData;
  }

  /**
   * 最小合法的 Vorbis comment 头：
   * {@code \x03vorbis + vendor 长度(LE) + vendor + 注释条数(LE, 0) + framing bit}。
   * FFmpeg 只严格要求第三段是 setup 头，这段只是把布局填补完整。
   */
  private static byte[] minimalVorbisCommentHeader() {
    byte[] vendor = "NextLib".getBytes(java.nio.charset.StandardCharsets.US_ASCII);
    byte[] header = new byte[7 + 4 + vendor.length + 4 + 1];
    header[0] = 3;
    System.arraycopy(
        "vorbis".getBytes(java.nio.charset.StandardCharsets.US_ASCII), 0, header, 1, 6);
    int p = 7;
    header[p++] = (byte) (vendor.length & 0xFF);
    header[p++] = (byte) ((vendor.length >> 8) & 0xFF);
    header[p++] = (byte) ((vendor.length >> 16) & 0xFF);
    header[p++] = (byte) ((vendor.length >> 24) & 0xFF);
    System.arraycopy(vendor, 0, header, p, vendor.length);
    p += vendor.length;
    header[p++] = 0; // 注释条数 = 0
    header[p++] = 0;
    header[p++] = 0;
    header[p++] = 0;
    header[p] = 1; // framing bit
    return header;
  }

  private native long ffmpegInitialize(
      String codecName,
      @Nullable byte[] extraData,
      boolean outputFloat,
      int rawSampleRate,
      int rawChannelCount);

  private native int ffmpegDecode(
      long context, ByteBuffer inputData, int inputSize, SimpleDecoderOutputBuffer decoderOutputBuffer, ByteBuffer outputData, int outputSize);

  private native int ffmpegGetChannelCount(long context);

  private native int ffmpegGetSampleRate(long context);

  private native long ffmpegReset(long context, @Nullable byte[] extraData);

  private native void ffmpegRelease(long context);
}
