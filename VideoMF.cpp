#include "VideoMF.h"
#include <wrl/client.h>
#include <emmintrin.h>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

// ============================================================================
// VideoMF 实现 —— IMFSourceReader 拉流(全软件解码) + NV12->BGRA 转换 + 音频 PCM 提取
// ============================================================================

// 特殊流常量(MF_SOURCE_READER_FIRST_VIDEO_STREAM 等)在配置/读取时通用,
// 避免 SourceReader 在选择流后重编号索引的问题。
static const DWORD kVideo = MF_SOURCE_READER_FIRST_VIDEO_STREAM;
static const DWORD kAudio = MF_SOURCE_READER_FIRST_AUDIO_STREAM;

VideoMF::VideoMF()
    : m_reader(nullptr)
    , m_videoIdx(0xFFFFFFFFu)
    , m_audioIdx(0xFFFFFFFFu)
    , m_width(0)
    , m_height(0)
    , m_fps(0.0)
    , m_duration100ns(0)
    , m_yPitch(0)
    , m_bufferPitch(0)
    , m_alignedH(0)
    , m_yuvMatrix(0)
    , m_lastAudioTs(-1.0)
    , m_eof(false)
{
}

VideoMF::~VideoMF()
{
    Close();
}

// ---- 错误处理 ----

std::string VideoMF::HResultText(HRESULT hr)
{
    switch (hr)
    {
    case S_OK:                                return "S_OK";
    case MF_E_INVALIDSTREAMNUMBER:            return "MF_E_INVALIDSTREAMNUMBER (无效流编号, 可能没有该流)";
    case MF_E_UNSUPPORTED_BYTESTREAM_TYPE:    return "MF_E_UNSUPPORTED_BYTESTREAM_TYPE (不支持的媒体容器, 仅支持 MP4 系)";
    case MF_E_UNSUPPORTED_FORMAT:             return "MF_E_UNSUPPORTED_FORMAT (不支持的媒体格式, 需 H.264/AAC)";
    case MF_E_TOPO_CODEC_NOT_FOUND:           return "MF_E_TOPO_CODEC_NOT_FOUND (系统缺少对应解码器)";
    case MF_E_INVALIDMEDIATYPE:               return "MF_E_INVALIDMEDIATYPE (请求的媒体类型无效)";
    case MF_E_ATTRIBUTENOTFOUND:              return "MF_E_ATTRIBUTENOTFOUND (属性缺失)";
    case MF_E_MEDIA_SOURCE_WRONGSTATE:        return "MF_E_MEDIA_SOURCE_WRONGSTATE (媒体源状态错误)";
    case MF_E_NOT_FOUND:                      return "MF_E_NOT_FOUND";
    case MF_E_END_OF_STREAM:                  return "MF_E_END_OF_STREAM";
    case MF_E_UNEXPECTED:                     return "MF_E_UNEXPECTED";
    case E_FAIL:                              return "E_FAIL";
    case E_INVALIDARG:                        return "E_INVALIDARG";
    case E_OUTOFMEMORY:                       return "E_OUTOFMEMORY";
    case E_NOTIMPL:                           return "E_NOTIMPL";
    default:
    {
        char buf[48];
        snprintf(buf, sizeof(buf), "0x%08X", (unsigned)hr);
        return buf;
    }
    }
}

void VideoMF::SetLastErrorHr(HRESULT hr, const char* where)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "0x%08X", (unsigned)hr);
    m_error = std::string(where) + " 失败: " + buf + " (" + HResultText(hr) + ")";
}

void VideoMF::SetError(std::string msg)
{
    m_error = std::move(msg);
}

// ---- 打开 / 配置 ----

bool VideoMF::Open(const wchar_t* path)
{
    Close();

    // 全软件解码: 不设置 DXVA2/硬件属性, SourceReader 直接用系统软解解码器。
    HRESULT hr = MFCreateSourceReaderFromURL(path, nullptr, &m_reader);
    if (FAILED(hr)) { SetLastErrorHr(hr, "MFCreateSourceReaderFromURL"); return false; }

    if (!FindStreams())
        return false;

    if (m_videoIdx == 0xFFFFFFFFu)
    {
        SetError("该文件没有视频流(可能只有音频)。");
        Close();
        return false;
    }

    // 视频要求偶数尺寸(H.264 强制 mod-2; NV12 转换按 2x2 色度采样)。
    if ((m_width & 1) || (m_height & 1))
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "视频尺寸 %ux%u 不是偶数, NV12 解码不支持。", m_width, m_height);
        SetError(buf);
        Close();
        return false;
    }

    // 预分配 NV12 / BGRA 缓冲。
    m_nv12.assign((size_t)m_width * m_height * 3 / 2, 0);
    m_bgra.assign((size_t)m_width * m_height * 4, 0);
    m_yPitch = (LONG)m_width;

    // 时长(100ns 单位)。
    PROPVARIANT var;
    PropVariantInit(&var);
    hr = m_reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &var);
    if (SUCCEEDED(hr) && var.vt == VT_UI8)
        m_duration100ns = var.uhVal.QuadPart;
    else if (SUCCEEDED(hr) && var.vt == VT_I8)
        m_duration100ns = (UINT64)var.hVal.QuadPart;
    PropVariantClear(&var);

    return true;
}

void VideoMF::Close()
{
    if (m_reader)
    {
        m_reader->Release();
        m_reader = nullptr;
    }
    m_videoIdx = 0xFFFFFFFFu;
    m_audioIdx = 0xFFFFFFFFu;
    m_width = m_height = 0;
    m_fps = 0.0;
    m_duration100ns = 0;
    m_yPitch = 0;
    m_bufferPitch = 0;
    m_alignedH = 0;
    m_yuvMatrix = 0;
    m_lastAudioTs = -1.0;
    m_eof = false;
    m_nv12.clear();
    m_bgra.clear();
}

// 检测流并配置视频流为 NV12、音频流为 PCM16/44100/立体声。
// 注意: IMFSourceReader 不暴露流枚举(GetStreamCount/GetStreamDescriptorByIndex
// 在 IMFMediaSource 上), 因此用"尝试配置特殊流常量"来检测音频是否存在:
//   - SetCurrentMediaType(FIRST_VIDEO_STREAM, ...) 成功 -> 有视频(必需, 失败即打开失败)
//   - SetCurrentMediaType(FIRST_AUDIO_STREAM, ...) 成功 -> 有音频; 返回
//     MF_E_INVALIDSTREAMNUMBER -> 无音频流(不致命, 用墙钟同步)
bool VideoMF::FindStreams()
{
    HRESULT hr = S_OK;

    // ---- 配置视频流: 请求 NV12(解码器原生输出, 二期 DXVA 同路径) ----
    ComPtr<IMFMediaType> videoType;
    hr = MFCreateMediaType(&videoType);
    if (FAILED(hr)) { SetLastErrorHr(hr, "MFCreateMediaType(video)"); return false; }
    videoType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    videoType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);

    hr = m_reader->SetCurrentMediaType(kVideo, nullptr, videoType.Get());
    if (FAILED(hr)) { SetLastErrorHr(hr, "SetCurrentMediaType(NV12)"); return false; }

    m_videoIdx = 0;   // 标记: 视频流已选中

    // 读取实际协商出的视频媒体类型 -> 宽/高/FPS。
    if (!UpdateVideoFormat())
        return false;

    // ---- 配置音频流(若有): 请求 PCM16/44100/立体声, SourceReader 自动插入
    //      Audio Resampler 完成 AAC->PCM ----
    ComPtr<IMFMediaType> audioType;
    hr = MFCreateMediaType(&audioType);
    if (FAILED(hr)) { SetLastErrorHr(hr, "MFCreateMediaType(audio)"); return false; }

    audioType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    audioType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    audioType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    audioType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 44100);
    audioType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    audioType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
    audioType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 44100 * 4);

    hr = m_reader->SetCurrentMediaType(kAudio, nullptr, audioType.Get());
    if (FAILED(hr))
    {
        // 无音频流或音频配置失败: 视频仍可播放, 降级墙钟同步。
        m_audioIdx = 0xFFFFFFFFu;
    }
    else
    {
        m_audioIdx = 0;
    }

    return true;
}

bool VideoMF::UpdateVideoFormat()
{
    ComPtr<IMFMediaType> type;
    HRESULT hr = m_reader->GetCurrentMediaType(kVideo, &type);
    if (FAILED(hr)) { SetLastErrorHr(hr, "GetCurrentMediaType(video)"); return false; }

    UINT32 w = 0, h = 0;
    hr = MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h);
    if (FAILED(hr) || w == 0 || h == 0)
    {
        SetError("解析视频尺寸失败(媒体类型无 MF_MT_FRAME_SIZE)。");
        return false;
    }
    m_width = w;
    m_height = h;

    UINT32 num = 0, den = 0;
    m_fps = 30.0;   // 缺省
    if (SUCCEEDED(MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &num, &den)) &&
        num != 0 && den != 0)
    {
        m_fps = (double)num / (double)den;
    }

    // YUV 矩阵(颜色转换用)。H.264 流的 VUI 会带矩阵信息(MF_MT_YUV_MATRIX);
    // 源未带时用 BT.601 作明确默认(不按尺寸猜测)。
    UINT32 matrix = 0;
    if (FAILED(type->GetUINT32(MF_MT_YUV_MATRIX, &matrix)) || matrix == 0)
        matrix = MFVideoTransferMatrix_BT601;
    m_yuvMatrix = matrix;

    return true;
}

// ---- 视频拉流 ----

// 把 NV12 数据(pitch 可能 > width)拷成紧密排布到 dst(width*height*3/2)。
// alignedH 是宏块对齐高度(UV 平面起点 = src + pitch*alignedH)。
static void CopyNV12ToPacked(uint8_t* dst, const uint8_t* src, LONG pitch,
                             LONG alignedH, UINT w, UINT h)
{
    size_t ySize = (size_t)w * h;
    const uint8_t* yp = src;
    const uint8_t* uvp = src + (size_t)pitch * alignedH;
    for (UINT row = 0; row < h; ++row)
        memcpy(dst + (size_t)row * w, yp + (size_t)row * pitch, w);
    for (UINT row = 0; row < h / 2; ++row)
        memcpy(dst + ySize + (size_t)row * w, uvp + (size_t)row * pitch, w);
}

bool VideoMF::DecodeNextFrame()
{
    // 阶段 1: 持锁拉帧(与音频线程串行化 m_reader 访问)。
    ComPtr<IMFSample> sample;
    {
        std::lock_guard<std::recursive_mutex> lk(m_mutex);
        if (!m_reader) return false;

        // 循环读取直到拿到真正的视频帧(tick 或空样本跳过)。
        for (;;)
        {
            DWORD streamIndex = 0, flags = 0;
            LONGLONG timestamp = 0;
            HRESULT hr = m_reader->ReadSample(kVideo, 0, &streamIndex, &flags, &timestamp, &sample);
            if (FAILED(hr))
            {
                if (hr == MF_E_END_OF_STREAM) { m_eof = true; return false; }
                // Seek 刚执行过 Flush 时, ReadSample 可能撞上 flush 未完成
                // (MF_E_NOTACCEPTING), 短暂重试(DXVA2 硬解尤其常见)。
                if (hr == MF_E_NOTACCEPTING)
                {
                    Sleep(5);
                    continue;
                }
                SetLastErrorHr(hr, "ReadSample(video)");
                return false;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { m_eof = true; return false; }
            if (flags & MF_SOURCE_READERF_STREAMTICK) continue;   // 无数据的时间点
            if (!sample) continue;
            break;   // 拿到一帧, 出循环
        }
    }
    // 锁在此释放。sample 已从 SourceReader 取出, 后续只操作 sample 与 buffer,
    // 不再碰 m_reader —— 耗时操作(Lock2D/CopyNV12ToPacked)因此不再持锁阻塞
    // 音频线程, 也消除了主线程消息泵重入时"持锁阻塞 -> 重入再取锁"的窗口
    // (m_mutex 同时是递归锁, 即使重入发生也不抛异常)。

    // 阶段 2: 无锁处理 sample。
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = sample->GetBufferByIndex(0, &buffer);
    if (FAILED(hr)) { SetLastErrorHr(hr, "GetBufferByIndex"); return false; }

    // 软件解码: 解码器输出的视频 buffer 实现 IMF2DBuffer, Lock2D 取数据。
    ComPtr<IMF2DBuffer> buf2d;
    hr = buffer.As(&buf2d);
    if (FAILED(hr) || !buf2d)
    {
        SetError("视频帧不实现 IMF2DBuffer(软件解码路径)。");
        return false;
    }
    BYTE* p = nullptr;
    LONG lp = 0;
    hr = buf2d->Lock2D(&p, &lp);
    if (FAILED(hr) || !p || lp <= 0)
    {
        SetError("Lock2D 失败(软件解码路径)。");
        return false;
    }

    // H.264 按 16 行宏块对齐解码: 显示高度 360 会被补到 368 行, UV 平面
    // 从 pitch*alignedH 开始, 不是 pitch*height。Y 平面高度必然是宏块对齐的
    // (16 的倍数), 直接取整即可。
    LONG alignedH = (LONG)(((m_height + 15) / 16) * 16);
    m_bufferPitch = lp;
    m_alignedH = alignedH;

    // 拷贝 NV12 到紧密排布(m_nv12 已在 Open 预分配)。
    CopyNV12ToPacked(m_nv12.data(), p, lp, alignedH, m_width, m_height);

    buf2d->Unlock2D();

    return true;
}

// ---- NV12 -> BGRA(A8R8G8B8 内存序), SSE2 加速 ----
// 有限范围(16-235)整数定点转换, 每像素 1.5 字节 -> 4 字节。
// YUV 矩阵按 m_yuvMatrix 选系数: BT.601(SD) 或 BT.709(HD)。
// SSE2: 每 4 像素一组, pmaddwd 一次完成 298*C+409*V 类定点乘加,
// packus 饱和链即 clamp(0-255)。与标量语义逐位一致(先整体求和
// +128 再 >>8, 算术右移, 饱和 clamp)。运行环境限定 x86/x64
// (SSE2 无条件可用), 不设回退分支。
static void ConvertRowNV12ToBGRA_SSE2(uint8_t* dst, const uint8_t* yrow,
    const uint8_t* uvrow, UINT32 w, bool use601)
{
    const int kR  = use601 ? 409 : 459;
    const int kGU = use601 ? -100 : -54;
    const int kGV = use601 ? -208 : -136;
    const int kBU = use601 ? 516 : 541;

    const __m128i kRYV = _mm_setr_epi16(298, kR, 298, kR, 298, kR, 298, kR);
    const __m128i kRYU = _mm_setr_epi16(298, kGU, 298, kGU, 298, kGU, 298, kGU);
    const __m128i kRYB = _mm_setr_epi16(298, kBU, 298, kBU, 298, kBU, 298, kBU);
    const __m128i kGVV = _mm_setr_epi16(0, kGV, 0, kGV, 0, kGV, 0, kGV);

    const __m128i zero  = _mm_setzero_si128();
    const __m128i alpha = _mm_set1_epi8((char)0xFF);
    const __m128i c128  = _mm_set1_epi32(128);

    UINT32 x = 0;
    for (; x + 4 <= w; x += 4)
    {
        // 低 8 字节: Y0..Y7 / U0V0U1V1U2V2U3V3(每 2 像素一对 UV)
        __m128i y8 = _mm_loadl_epi64((const __m128i*)(yrow + x));
        __m128i uv = _mm_loadl_epi64((const __m128i*)(uvrow + (x >> 1)));

        // 16 位小端元素 = {V0U0,V1U1,V2U2,V3U3} -> 拆 U/V
        __m128i v8    = _mm_packus_epi16(_mm_srli_epi16(uv, 8), zero);       // {V0..V3}
        __m128i u8    = _mm_packus_epi16(_mm_and_si128(uv, _mm_set1_epi16(0xFF)), zero); // {U0..U3}
        __m128i vrep8 = _mm_unpacklo_epi8(v8, v8);     // 字节 {V0,V0,V1,V1,V2,V2,V3,V3}
        __m128i urep8 = _mm_unpacklo_epi8(u8, u8);     // 字节 {U0,U0,U1,U1,U2,U2,U3,U3}
        __m128i c16   = _mm_unpacklo_epi8(y8, zero);   // int16 {C0..C7}
        __m128i v16   = _mm_unpacklo_epi8(vrep8, zero); // int16 {V0,V0,V1,V1,V2,V2,V3,V3}
        __m128i u16   = _mm_unpacklo_epi8(urep8, zero); // int16 {U0,U0,U1,U1,U2,U2,U3,U3}
        __m128i xv    = _mm_unpacklo_epi16(c16, v16);  // int16 {C0,V0,C1,V0,C2,V1,C3,V1}
        __m128i xu    = _mm_unpacklo_epi16(c16, u16);  // int16 {C0,U0,C1,U0,C2,U1,C3,U1}

        __m128i r32 = _mm_madd_epi16(xv, kRYV);              // {R0..R3}
        __m128i g32 = _mm_madd_epi16(xu, kRYU);              // {298C+kGU*U}
        g32 = _mm_add_epi32(g32, _mm_madd_epi16(xv, kGVV));   // + kGV*V
        __m128i b32 = _mm_madd_epi16(xu, kRYB);              // {298C+kBU*U}

        r32 = _mm_srai_epi32(_mm_add_epi32(r32, c128), 8);   // (+128)>>8 舍入
        g32 = _mm_srai_epi32(_mm_add_epi32(g32, c128), 8);
        b32 = _mm_srai_epi32(_mm_add_epi32(b32, c128), 8);

        // 饱和压缩两段 = clamp(0..255)
        __m128i r8 = _mm_packus_epi16(_mm_packs_epi32(r32, r32), zero);
        __m128i g8 = _mm_packus_epi16(_mm_packs_epi32(g32, g32), zero);
        __m128i b8 = _mm_packus_epi16(_mm_packs_epi32(b32, b32), zero);

        // 三次交错 -> BGRA 16 字节 = 4 像素
        __m128i bg  = _mm_unpacklo_epi8(b8, g8);      // {B0,G0,B1,G1,B2,G2,B3,G3}
        __m128i ra  = _mm_unpacklo_epi8(r8, alpha);   // {R0,FF,R1,FF,R2,FF,R3,FF}
        __m128i out = _mm_unpacklo_epi8(bg, ra);      // {B,G,R,FF} x4
        _mm_storeu_si128((__m128i*)(dst + x * 4), out);
    }

    // 尾部 w%4 像素(NV12 只保证偶数宽): 标量收尾
    for (; x < w; ++x)
    {
        int C = (int)yrow[x] - 16;
        int U = (int)uvrow[(x >> 1) << 1] - 128;
        int V = (int)uvrow[((x >> 1) << 1) + 1] - 128;

        int R = (298 * C + kR * V + 128) >> 8;
        int G = (298 * C + kGU * U + kGV * V + 128) >> 8;
        int B = (298 * C + kBU * U + 128) >> 8;

        dst[0] = (uint8_t)(B < 0 ? 0 : B > 255 ? 255 : B);   // B
        dst[1] = (uint8_t)(G < 0 ? 0 : G > 255 ? 255 : G);   // G
        dst[2] = (uint8_t)(R < 0 ? 0 : R > 255 ? 255 : R);   // R
        dst[3] = 255;                                        // A
        dst += 4;
    }
}

void VideoMF::ConvertNV12ToBGRA()
{
    if (m_nv12.empty() || m_width == 0 || m_height == 0)
        return;

    const UINT32 w = m_width, h = m_height;
    m_bgra.resize((size_t)w * h * 4);

    // 定点系数(<<8)。BT.601: R=1.596, G=-0.392/-0.813, B=2.017;
    // BT.709: R=1.793, G=-0.213/-0.533, B=2.112。非 601(709/2020 等)一律按 709。
    const bool use601 = (m_yuvMatrix == MFVideoTransferMatrix_BT601);
    const uint8_t* yp = m_nv12.data();
    const uint8_t* uvp = yp + (size_t)w * h;   // 紧密排布, UV 平面 pitch = w

    for (UINT32 y = 0; y < h; ++y)
    {
        ConvertRowNV12ToBGRA_SSE2(m_bgra.data() + (size_t)y * w * 4,
            yp + (size_t)y * w, uvp + (size_t)(y >> 1) * w, w, use601);
    }
}

// ---- 音频拉流 ----

// 读取协商后的实际音频媒体类型(诊断用; 内部加锁保护 m_reader)。
VideoMF::AudioFormatInfo VideoMF::GetAudioFormat()
{
    std::lock_guard<std::recursive_mutex> lk(m_mutex);
    AudioFormatInfo info = { false, GUID_NULL, 0, 0, 0 };
    if (!m_reader || m_audioIdx == 0xFFFFFFFFu)
        return info;
    ComPtr<IMFMediaType> type;
    if (FAILED(m_reader->GetCurrentMediaType(kAudio, &type)))
        return info;
    type->GetGUID(MF_MT_SUBTYPE, &info.subtype);
    type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &info.rate);
    type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &info.channels);
    type->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &info.bits);
    info.valid = true;
    return info;
}

// 逐样本读 PCM(后台线程边解边喂 AudioOut)。锁保护与视频流并发访问。
bool VideoMF::ReadAudioChunk(std::vector<uint8_t>& out)
{
    std::lock_guard<std::recursive_mutex> lk(m_mutex);
    if (!m_reader || m_audioIdx == 0xFFFFFFFFu)
        return false;

    out.clear();
    for (;;)
    {
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        HRESULT hr = m_reader->ReadSample(kAudio, 0, nullptr, &flags, &timestamp, &sample);
        if (hr == MF_E_END_OF_STREAM)
            return false;
        // Seek 后 Flush 未完成时 ReadSample 返回 MF_E_NOTACCEPTING, 短暂重试
        // (与视频路径一致; 不处理会把瞬时错误当 EOF, 音频从此静音)。
        if (hr == MF_E_NOTACCEPTING)
        {
            Sleep(5);
            continue;
        }
        if (FAILED(hr))
        {
            SetLastErrorHr(hr, "ReadSample(audio)");
            return false;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
            return false;
        if (flags & MF_SOURCE_READERF_STREAMTICK)
            continue;
        if (!sample)
            continue;

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer)))
            continue;

        BYTE* p = nullptr;
        DWORD maxLen = 0, curLen = 0;
        if (FAILED(buffer->Lock(&p, &maxLen, &curLen)) || !p || curLen == 0)
            continue;

        out.assign(p, p + curLen);
        buffer->Unlock();
        m_lastAudioTs = (double)timestamp / 1e7;   // 记录样本时间戳(验证跳转内容用)
        return true;
    }
}

bool VideoMF::ExtractAudioToWav(const wchar_t* wavPath)
{
    std::lock_guard<std::recursive_mutex> lk(m_mutex);
    if (!m_reader || m_audioIdx == 0xFFFFFFFFu)
    {
        SetError("没有可提取的音频流。");
        return false;
    }

    std::vector<uint8_t> pcm;
    pcm.reserve(1 << 20);   // 1MB 起步

    for (;;)
    {
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        HRESULT hr = m_reader->ReadSample(kAudio, 0, nullptr, &flags, &timestamp, &sample);
        if (FAILED(hr))
        {
            if (hr == MF_E_END_OF_STREAM) break;
            // Seek/Flush 后的瞬时状态, 短暂重试(否则误判为终止)。
            if (hr == MF_E_NOTACCEPTING)
            {
                Sleep(5);
                continue;
            }
            SetLastErrorHr(hr, "ReadSample(audio)");
            return false;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (flags & MF_SOURCE_READERF_STREAMTICK) continue;
        if (!sample) continue;

        DWORD total = 0;
        hr = sample->GetTotalLength(&total);
        if (FAILED(hr)) continue;

        ComPtr<IMFMediaBuffer> buffer;
        hr = sample->ConvertToContiguousBuffer(&buffer);
        if (FAILED(hr)) continue;

        BYTE* p = nullptr;
        DWORD maxLen = 0, curLen = 0;
        hr = buffer->Lock(&p, &maxLen, &curLen);
        if (FAILED(hr) || !p) continue;

        size_t oldSize = pcm.size();
        pcm.resize(oldSize + curLen);
        memcpy(pcm.data() + oldSize, p, curLen);
        buffer->Unlock();
    }

    if (pcm.empty())
    {
        SetError("音频流没有解码出数据。");
        return false;
    }

    // ---- 写 WAV(用 MF 协商出的实际格式; 硬编码 44100 会把 48000 源快播) ----
    AudioFormatInfo af = GetAudioFormat();
    if (!af.valid || af.rate == 0 || af.channels == 0 || af.bits == 0)
    {
        SetError("音频协商格式无效, 无法写 WAV。");
        return false;
    }
    const DWORD sampleRate = af.rate;
    const WORD  channels = (WORD)af.channels;
    const WORD  bitsPerSample = (WORD)af.bits;
    const DWORD byteRate = sampleRate * channels * (bitsPerSample / 8);
    const WORD  blockAlign = (WORD)(channels * (bitsPerSample / 8));
    // 标准 WAV 的 RIFF/data 长度字段是 32 位, 超 4GB 无法表示。
    if (pcm.size() > 0xFFFFFFF0u)
    {
        SetError("音频数据超过标准 WAV 上限(4GB), 无法写入。");
        return false;
    }
    const DWORD dataSize = (DWORD)pcm.size();
    const DWORD riffSize = 4 + (8 + 16) + (8 + dataSize);

    FILE* f = nullptr;
    if (_wfopen_s(&f, wavPath, L"wb") != 0 || !f)
    {
        SetError("创建临时 WAV 文件失败。");
        return false;
    }

    auto wr = [&](const void* p, size_t n) { fwrite(p, 1, n, f); };
    const char* riff = "RIFF"; wr(riff, 4);
    wr(&riffSize, 4);
    const char* wave = "WAVE"; wr(wave, 4);
    const char* fmt = "fmt "; wr(fmt, 4);
    DWORD fmtSize = 16;      wr(&fmtSize, 4);
    WORD  audioFormat = 1;   // PCM
    if (af.subtype == MFAudioFormat_Float)
        audioFormat = 3;     // IEEE float
    wr(&audioFormat, 2);
    wr(&channels, 2);
    wr(&sampleRate, 4);
    wr(&byteRate, 4);
    wr(&blockAlign, 2);
    wr(&bitsPerSample, 2);
    const char* data = "data"; wr(data, 4);
    wr(&dataSize, 4);
    wr(pcm.data(), pcm.size());

    fclose(f);
    return true;
}

// ---- 定位 ----

bool VideoMF::Seek(double seconds)
{
    std::lock_guard<std::recursive_mutex> lk(m_mutex);
    if (!m_reader) return false;

    PROPVARIANT var;
    PropVariantInit(&var);
    var.vt = VT_I8;
    var.hVal.QuadPart = (LONGLONG)(seconds * 1e7);
    HRESULT hr = m_reader->SetCurrentPosition(GUID_NULL, var);
    PropVariantClear(&var);
    if (FAILED(hr))
    {
        SetLastErrorHr(hr, "SetCurrentPosition");
        return false;
    }

    // 清掉流内残留样本, 让下一次 ReadSample 从目标位置开始。
    m_reader->Flush(kVideo);
    if (m_audioIdx != 0xFFFFFFFFu)
        m_reader->Flush(kAudio);

    m_eof = false;
    return true;
}
