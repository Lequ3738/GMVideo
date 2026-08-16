#pragma once
#include <windows.h>
#include "bass.h"
#include <cstdint>
#include <cstddef>

// ============================================================================
// BassOut —— bass push stream 音频输出(GMVideo 音频渲染后端)
//
// 替代 DirectSound(AudioOut): bass 内部自动管理播放缓冲/队列/stall, 不丢数据,
// 位置/暂停/恢复/循环/倍速全部现成 API。GMVideo 后台线程解 PCM 后 Put() 推送,
// 主线程 GetPositionSec() 查播放进度驱动视频同步。
//
// 与进程里已有 bass 用户(如 NatureEnhance/MaizeMusic)共存: BASS_Init 重复调用
// 返回 BASS_ERROR_ALREADY, 视为已可用直接继续, 且不 BASS_Free(避免杀掉别的流)。
// ============================================================================

class BassOut
{
public:
    BassOut();
    ~BassOut();

    // 初始化 bass 输出设备。若已被其它模块初始化(ALREADY)则接受并复用。
    bool Init(HWND hwnd = nullptr);
    // 创建 push stream。rate/chans/bits = 送入数据的真实 PCM 格式。
    // 必须与 MF 协商出的实际音频格式一致, 否则 bass 按错误速率播放(快/慢)。
    bool CreateStream(DWORD rate, DWORD chans, WORD bits);
    void Destroy();

    bool Valid() const { return m_stream != 0; }
    bool OwnedInit() const { return m_ownedInit; }   // true=本 DLL 初始化; false=复用已存在的 bass

    // ---- 播放控制 ----
    bool Play(bool restart);   // restart=FALSE 从暂停处恢复; TRUE 从头播
    void Pause();
    void Reset();              // 重建 push stream: 彻底清空队列/输出缓冲/声卡残留

    // ---- 变速 ----
    // 音频变速(bass 核心采样率变速): speed>0, 1.0=原速, 2.0=2x, 0.5=半速。
    // 实时生效, GetPositionSec 返回变速后的输出进度(视频按其同步)。
    // 注: 不用 bass_fx tempo(音调不变) —— 它需要可流式 source, push stream
    // 不支持(实测 BASS_ERROR_UNSTREAMABLE)。FREQ 变速音调随速度变化。
    bool SetTempo(double speed);
    double Tempo() const { return m_tempo; }

    // ---- 数据 ----
    // 推送 PCM 数据。bass 内部队列缓冲 + stall 自动恢复, 不丢数据。
    // 返回 true 成功(数据已入队), false 出错(队列满等, LastError() 可取)。
    bool Put(const uint8_t* data, size_t bytes);

    // ---- 查询 ----
    double GetPositionSec();   // 变速后的输出播放进度(秒); 音频变速时含 speed 效应
    UINT   ByteRate() const { return m_byteRate; }
    HSTREAM StreamHandle() const { return m_stream; }   // 只读暴露给调用方(DSP/诊断等)
    const char* LastError() const { return m_lastError; }

private:
    HSTREAM m_stream;
    DWORD   m_baseRate;       // 创建时的采样率(SetTempo 用 FREQ=baseRate*speed)
    DWORD   m_chans;          // 创建时的声道数(Reset 重建用)
    WORD    m_bits;           // 创建时的位深(Reset 重建用)
    UINT    m_byteRate;
    bool    m_ownedInit;       // 本次是否真正调用 BASS_Init 成功(需 BASS_Free)
    double  m_tempo;           // 当前变速率(1.0=原速)
    const char* m_lastError;
};
