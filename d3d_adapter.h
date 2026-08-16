#pragma once
// ============================================================================
// NatureEnhance 双后端适配器 —— 版本中立接口(D3D8 / D3D9 运行时自动分发)
//
// 与 GMGraphic 的双后端改造同架构(GMG-2026-08 移植):
//   - 规则: 本头只出现 DWORD / UINT / 指针 / float / RECT / std 类型, 绝不出现
//     IDirect3DDevice8/9 等 D3D 类型。这样共享代码无需认识任何一个具体版本的
//     接口, 也就没有 d3d8.h 与 d3d9.h 的头冲突。
//   - 实现分两个 TU:
//       d3d_adapter8.cpp  仅认识 d3d8.h + d3dx8.h  (D3D8 分支, D3DX8 静态库)
//       d3d_adapter9.cpp  仅认识 d3d9.h + d3dx9.h  (D3D9 分支, D3DX9 运行时解析)
//     两者永不同时进入同一编译单元。
//   - 公共 API 按运行时的后端选择(version()/ensure_version())分发。未初始化时
//     默认按 D3D8(历史行为, 原生 GM8 游戏无回归)。
//   - 纹理 / 表面 / 设备一律不透明 void*, 共享代码绝不直接调其方法, 只经 d3d::。
//   - 共享代码沿用 D3D8 枚举(D3DFMT_/D3DRS_/D3DCOLORWRITEENABLE_ 等): GmapiDefs.h
//     在 GMAPI_USE_D3D 下已引入 d3d8.h, 且这些枚举值在 DX8 与 DX9 中数值完全一致,
//     共享代码把 D3D8 枚举值当 DWORD 传入, 适配器按后端转回对应版本的类型。
// ============================================================================
#include <windows.h>
#include <string>

namespace d3d
{
    enum Version : int { UNKNOWN = 0, V8 = 8, V9 = 9 };

    // 中性"锁定矩形"。D3D8/9 的 D3DLOCKED_RECT 布局逐字相同(int Pitch + void* pBits),
    // 这里用独立结构避免接口头认识任何一个版本的 D3D 类型。
    struct LockedRect
    {
        int  Pitch;
        void* pBits;
    };

    // ---- 初始化 / 检测 ----
    int  version();                          // 惰性检测并缓存; 未初始化时默认 V8
    void ensure_version(void* device, void* iface);

    // 原始 COM 指针访问(仅适配器实现内部使用)。
    void* device();
    void* iface();

    // ---- 内部实现: 中性签名, 定义在 d3d_adapter8.cpp / d3d_adapter9.cpp ----
    namespace impl8
    {
        HRESULT set_render_state(DWORD, DWORD);
        HRESULT get_device_from_texture(void*, void**);
        HRESULT get_surface_level(void*, UINT, void**);
        HRESULT create_image_surface(UINT, UINT, DWORD, void**);
        DWORD   get_surface_format(void*);
        HRESULT lock_rect(void*, LockedRect*, const RECT*, DWORD);
        HRESULT unlock_rect(void*);
        HRESULT add_dirty_rect(void*, const RECT*);
        UINT    get_level_count(void*);
        HRESULT load_surface_from_memory(void*, const RECT*, const void*, DWORD, UINT, const RECT*);
        HRESULT load_surface_from_surface(void*, void*);
        void    release(void*);
        std::string error_text(HRESULT);
    }
    namespace impl9
    {
        HRESULT set_render_state(DWORD, DWORD);
        HRESULT get_device_from_texture(void*, void**);
        HRESULT get_surface_level(void*, UINT, void**);
        HRESULT create_image_surface(UINT, UINT, DWORD, void**);
        DWORD   get_surface_format(void*);
        HRESULT lock_rect(void*, LockedRect*, const RECT*, DWORD);
        HRESULT unlock_rect(void*);
        HRESULT add_dirty_rect(void*, const RECT*);
        UINT    get_level_count(void*);
        HRESULT load_surface_from_memory(void*, const RECT*, const void*, DWORD, UINT, const RECT*);
        HRESULT load_surface_from_surface(void*, void*);
        void    release(void*);
        std::string error_text(HRESULT);
    }

    // ---- 公共 API(运行时按后端分发, 每处一行) ----
    inline HRESULT set_render_state(DWORD s, DWORD v)
    { return version() == V9 ? impl9::set_render_state(s, v) : impl8::set_render_state(s, v); }
    inline HRESULT get_device_from_texture(void* tex, void** out)
    { return version() == V9 ? impl9::get_device_from_texture(tex, out) : impl8::get_device_from_texture(tex, out); }
    inline HRESULT get_surface_level(void* tex, UINT level, void** out)
    { return version() == V9 ? impl9::get_surface_level(tex, level, out) : impl8::get_surface_level(tex, level, out); }
    inline DWORD get_surface_format(void* surf)
    { return version() == V9 ? impl9::get_surface_format(surf) : impl8::get_surface_format(surf); }
    inline HRESULT create_image_surface(UINT w, UINT h, DWORD fmt, void** out)
    { return version() == V9 ? impl9::create_image_surface(w, h, fmt, out) : impl8::create_image_surface(w, h, fmt, out); }
    inline HRESULT lock_rect(void* surf, LockedRect* out, const RECT* rect, DWORD flags)
    { return version() == V9 ? impl9::lock_rect(surf, out, rect, flags) : impl8::lock_rect(surf, out, rect, flags); }
    inline HRESULT unlock_rect(void* surf)
    { return version() == V9 ? impl9::unlock_rect(surf) : impl8::unlock_rect(surf); }
    inline HRESULT add_dirty_rect(void* tex, const RECT* rect)
    { return version() == V9 ? impl9::add_dirty_rect(tex, rect) : impl8::add_dirty_rect(tex, rect); }
    inline UINT get_level_count(void* tex)
    { return version() == V9 ? impl9::get_level_count(tex) : impl8::get_level_count(tex); }
    inline HRESULT load_surface_from_memory(void* dst, const RECT* dstRect, const void* src, DWORD fmt, UINT pitch, const RECT* srcRect)
    { return version() == V9 ? impl9::load_surface_from_memory(dst, dstRect, src, fmt, pitch, srcRect) : impl8::load_surface_from_memory(dst, dstRect, src, fmt, pitch, srcRect); }
    inline HRESULT load_surface_from_surface(void* dst, void* src)
    { return version() == V9 ? impl9::load_surface_from_surface(dst, src) : impl8::load_surface_from_surface(dst, src); }
    inline void release(void* com)
    { if (version() == V9) impl9::release(com); else impl8::release(com); }

    // 错误码 → 可读文本(D3D8 用 DXGetErrorDescription8A, D3D9 用内置错误表)。
    inline std::string error_text(HRESULT hr)
    { return version() == V9 ? impl9::error_text(hr) : impl8::error_text(hr); }
}
