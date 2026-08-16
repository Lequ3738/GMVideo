#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

// ============================================================================
// VideoMF —— Media Foundation 解码核心(独立插件用)
//
// 职责: 用 IMFSourceReader 从 MP4 拉流, 输出:
//   - 视频: 请求 MFVideoFormat_NV12(解码器原生格式), 每次 DecodeNextFrame()
//           取一帧 NV12 原始数据, 再 ConvertNV12ToBGRA() 转成 BGRA 字节序
//           (即 D3D 的 A8R8G8B8, 可直接喂 d3d::load_surface_from_memory)。
//   - 音频: 请求 PCM 16bit / 44100Hz / stereo, SourceReader 自动插入
//           Audio Resampler 完成 AAC->PCM, 由后台线程 ReadAudioChunk 边解边喂
//           AudioOut(DirectSound), 不落地文件。
//
// 解码方式: 全软件解码(不启用 DXVA2 硬解)。实测硬解在 GM8 环境首帧初始化
//   阻塞 ~23 秒, 且循环 Seek 内存持续增长; 软解首帧 <50ms、内存稳定, 1080p
//   远快于实时。D3D8/D3D9 后端通用。
// ============================================================================

class VideoMF
{
public:
    VideoMF();
    ~VideoMF();

    // 打开文件并配置流。path 为 UTF-16 完整路径。返回 false 时 LastError() 有原因。
    // 全软件解码。
    bool Open(const wchar_t* path);
    void Close();

    bool HasVideo() const { return m_videoIdx != 0xFFFFFFFFu; }
    bool HasAudio() const { return m_audioIdx != 0xFFFFFFFFu; }
    bool IsOpen()   const { return m_reader != nullptr; }
    bool Eof()      const { return m_eof; }

    // ---- 元数据 ----
    UINT32 GetWidth()        const { return m_width; }
    UINT32 GetHeight()       const { return m_height; }
    double GetFPS()          const { return m_fps; }
    double GetDurationSec()  const { return m_duration100ns / 1e7; }

    // ---- 视频拉流 ----
    // 读下一视频帧。返回 false 表示 EOF 或出错(LastError())。
    bool DecodeNextFrame();
    // NV12 原始数据 / 大小 / Y plane 行距(UV plane 假定同 pitch)。
    const uint8_t* GetNV12Data()   const { return m_nv12.data(); }
    size_t         GetNV12Size()   const { return m_nv12.size(); }
    LONG           GetYPlanePitch() const { return m_yPitch; }
    LONG           GetBufferPitch() const { return m_bufferPitch; }   // 最近一帧 MF 2D 缓冲的真实行距
    LONG           GetAlignedHeight() const { return m_alignedH; }     // 宏块对齐后的缓冲高度
    // NV12 -> BGRA(width*4*height), 结果在 GetBGRA()。
    void ConvertNV12ToBGRA();
    const uint8_t* GetBGRA() const { return m_bgra.data(); }

    // ---- 音频拉流 ----
    struct AudioFormatInfo
    {
        bool valid;
        GUID subtype;          // MFAudioFormat_PCM / MFAudioFormat_Float 等
        UINT32 rate, channels, bits;
    };
    AudioFormatInfo GetAudioFormat();   // 读取协商后的实际音频媒体类型(内部加锁)
    // 读下一音频样本的 PCM 数据(16bit/44100/stereo)到 out。返回 true 有数据,
    // false = 已到 EOF 或出错(LastError)。供后台线程边解边喂 AudioOut。
    bool ReadAudioChunk(std::vector<uint8_t>& out);
    // 最近一次 ReadAudioChunk 读到的音频样本时间戳(秒, -1=未读过)。
    // 用于验证跳转后音频线程读到的是目标位置的内容(测试/诊断)。
    double GetLastAudioSampleTime() const { return m_lastAudioTs; }
    // 读出全部音频样本(已转 PCM16/44100/stereo)写入 WAV 文件。
    bool ExtractAudioToWav(const wchar_t* wavPath);

    // ---- 定位 ----
    bool Seek(double seconds);

    std::string LastError() const { return m_error; }

private:
    void SetLastErrorHr(HRESULT hr, const char* where);
    void SetError(std::string msg);
    bool UpdateVideoFormat();   // 读当前视频媒体类型 -> w/h/fps/pitch
    bool FindStreams();         // 枚举流, 找视频/音频索引, 配置 NV12 / PCM
    static std::string HResultText(HRESULT hr);

    IMFSourceReader* m_reader;
    DWORD   m_videoIdx;
    DWORD   m_audioIdx;
    UINT32  m_width;
    UINT32  m_height;
    double  m_fps;
    UINT64  m_duration100ns;
    LONG    m_yPitch;          // NV12 Y plane 行距(字节)
    LONG    m_bufferPitch;     // MF 2D 缓冲真实行距(最近一帧)
    LONG    m_alignedH;        // 宏块对齐后的缓冲高度(UV 平面起点用)
    UINT32  m_yuvMatrix;       // YUV 矩阵(MFVideoTransferMatrix 值), 颜色转换用
    double  m_lastAudioTs;     // 最近一次音频样本时间戳(秒, -1=未读过)
    std::vector<uint8_t> m_nv12;
    std::vector<uint8_t> m_bgra;
    // 保护 m_reader(主线程视频流 / 后台线程音频流并发)。必须是递归锁:
    // GM8 游戏循环跑在消息泵里, 主线程在耗时调用期间可能被重入
    // (嵌套消息分发 -> 再次进入 DecodeNextFrame), 非递归锁会抛
    // "resource deadlock would occur"(实测崩溃)。
    std::recursive_mutex m_mutex;
    bool m_eof;
    std::string m_error;
};
