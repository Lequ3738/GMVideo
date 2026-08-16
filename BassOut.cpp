#include "BassOut.h"
#include <cstdio>

BassOut::BassOut()
    : m_stream(0), m_baseRate(0), m_chans(0), m_bits(0), m_byteRate(0),
      m_ownedInit(false), m_tempo(1.0), m_lastError("")
{
}

BassOut::~BassOut()
{
    Destroy();
}

bool BassOut::Init(HWND hwnd)
{
    // bass 是进程级单例: 若已被其它模块(如 NatureEnhance/MaizeMusic)初始化,
    // BASS_Init 返回 FALSE + BASS_ERROR_ALREADY, 视为可用, 不重复初始化。
    if (BASS_Init(-1, 44100, 0, hwnd, nullptr))
    {
        m_ownedInit = true;
        return true;
    }
    if (BASS_ErrorGetCode() == BASS_ERROR_ALREADY)
    {
        m_ownedInit = false;   // 别人已初始化, 不 BASS_Free
        return true;
    }
    m_lastError = "BASS_Init 失败";
    return false;
}

bool BassOut::CreateStream(DWORD rate, DWORD chans, WORD bits)
{
    if (rate == 0 || chans == 0 || bits == 0)
    {
        m_lastError = "CreateStream: 非法格式";
        return false;
    }
    // push stream: proc = STREAMPROC_PUSH, flags=0(16bit 或 32bit PCM)。
    // flags: BASS_SAMPLE_FLOAT 仅用于浮点, PCM 整型用 0。
    DWORD flags = 0;
    if (bits == 32)
        flags |= BASS_SAMPLE_FLOAT;
    else if (bits != 16)
    {
        m_lastError = "CreateStream: 仅支持 16/32bit PCM";
        return false;
    }
    m_stream = BASS_StreamCreate(rate, chans, flags, STREAMPROC_PUSH, nullptr);
    if (!m_stream)
    {
        m_lastError = "BASS_StreamCreate 失败";
        return false;
    }
    m_baseRate = rate;
    m_chans = chans;
    m_bits = bits;
    m_byteRate = rate * chans * (bits / 8);
    m_tempo = 1.0;
    // 限制 push 队列, 防止音频线程一直快于播放导致内存无限增长。
    // 2 秒 = byteRate * 2 字节。
    BASS_ChannelSetAttribute(m_stream, BASS_ATTRIB_PUSH_LIMIT, (float)(m_byteRate * 2));
    return true;
}

void BassOut::Destroy()
{
    if (m_stream)
    {
        BASS_StreamFree(m_stream);
        m_stream = 0;
    }
    if (m_ownedInit)
    {
        BASS_Free();
        m_ownedInit = false;
    }
    m_baseRate = 0;
    m_chans = 0;
    m_bits = 0;
    m_byteRate = 0;
    m_tempo = 1.0;
}

bool BassOut::Play(bool restart)
{
    if (!m_stream) return false;
    return BASS_ChannelPlay(m_stream, restart ? TRUE : FALSE) != FALSE;
}

void BassOut::Pause()
{
    if (m_stream) BASS_ChannelPause(m_stream);
}

void BassOut::Reset()
{
    if (!m_stream) return;
    // 重建 push stream, 不用 SetPosition(0): 它只清队列, 输出缓冲/声卡残留旧
    // 音频(跳转/循环后旧声+新声双轨), 且与音频线程并发时会把跳转前读出的旧
    // chunk 推回新队列。重建流彻底清空 bass 内部一切, 无竞态窗口。
    DWORD rate = m_baseRate, chans = m_chans;
    WORD bits = m_bits;
    double tempo = m_tempo;
    BASS_StreamFree(m_stream);
    m_stream = 0;

    DWORD flags = 0;
    if (bits == 32) flags |= BASS_SAMPLE_FLOAT;
    m_stream = BASS_StreamCreate(rate, chans, flags, STREAMPROC_PUSH, nullptr);
    if (!m_stream)
        return;   // 重建失败: 调用方会因 Play(false) 失败而处理
    m_byteRate = rate * chans * (bits / 8);
    BASS_ChannelSetAttribute(m_stream, BASS_ATTRIB_PUSH_LIMIT, (float)(m_byteRate * 2));
    if (tempo != 1.0)
        BASS_ChannelSetAttribute(m_stream, BASS_ATTRIB_FREQ, (float)(rate * tempo));
    // 重建后流处于停止态, 调用方负责 Play(false) 重启。
}

bool BassOut::SetTempo(double speed)
{
    if (!m_stream || speed <= 0)
        return false;
    // bass 核心采样率变速: FREQ = 基础采样率 × 速度。实时生效, 位置连续,
    // GetPositionSec 返回变速后的输出进度。音调随速度变化(磁带效果)。
    if (BASS_ChannelSetAttribute(m_stream, BASS_ATTRIB_FREQ, (float)(m_baseRate * speed)) == FALSE)
    {
        m_lastError = "BASS_ATTRIB_FREQ 设置失败";
        return false;
    }
    m_tempo = speed;
    return true;
}

bool BassOut::Put(const uint8_t* data, size_t bytes)
{
    if (!m_stream || bytes == 0) return true;   // 空数据不处理
    // 返回当前排队字节数; -1 = 出错。length 必须整数样本帧(4 字节对齐)。
    if (BASS_StreamPutData(m_stream, (void*)data, (DWORD)bytes) == -1)
    {
        m_lastError = "BASS_StreamPutData 失败";
        return false;
    }
    return true;
}

double BassOut::GetPositionSec()
{
    if (!m_stream || m_byteRate == 0) return 0.0;
    QWORD pos = BASS_ChannelGetPosition(m_stream, BASS_POS_BYTE);
    if (pos == -1) return 0.0;
    return (double)pos / m_byteRate;
}
