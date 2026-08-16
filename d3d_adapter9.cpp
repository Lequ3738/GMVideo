// D3D9 后端实现。本 TU 认识 d3d9.h / d3dx9.h(现代 Windows SDK 的 D3D9Ex 合并头)。
// 在 GMDirectX9 插件下, 设备指针是 IDirect3DDevice9 对象。
//
// 与 D3D8 的差异集中体现在本文件:
//   1. CreateImageSurface(D3D8 专有) -> CreateOffscreenPlainSurface(D3DPOOL_SYSTEMMEM)
//   2. D3DX9 必须运行时 LoadLibrary + GetProcAddress:
//      D3DX8 是静态库(无 d3dx8.dll), 且 D3DX8/D3DX9 有大量同名 stdcall 函数
//      (如 D3DXLoadSurfaceFromMemory, 都是 @40), 两个都静态链必然撞名。
//      因此 D3DX9 走运行时解析(与 GMDirectX9 自身一致); D3D8 分支保持静态链
//      d3dx8.lib(原样)。d3d9.h / d3dx9.h 仍完整引入供类型使用。
//   3. D3D9 创建方法带共享句柄参数: CreateOffscreenPlainSurface 是 7 参
//      (多 HANDLE* pSharedHandle), 传 nullptr 即"不共享"的经典行为。
#include <cstdio>
#include "d3d_adapter.h"
#include "Direct3D_9/d3dx9.h"

namespace d3d
{
    namespace impl9
    {
        static IDirect3DDevice9* dev() { return (IDirect3DDevice9*)device(); }

        // ---- D3DX9 运行时解析(不静态链 d3dx9.lib, 避免与 D3DX8 撞名) ----
        // 签名直接取自 d3dx9tex.h。
        typedef HRESULT(WINAPI* D3DX9_LOAD_SURFACE_FROM_MEMORY)(LPDIRECT3DSURFACE9, const PALETTEENTRY*, const RECT*, LPCVOID, D3DFORMAT, UINT, const PALETTEENTRY*, const RECT*, DWORD, D3DCOLOR);
        typedef HRESULT(WINAPI* D3DX9_LOAD_SURFACE_FROM_SURFACE)(LPDIRECT3DSURFACE9, const PALETTEENTRY*, const RECT*, LPDIRECT3DSURFACE9, const PALETTEENTRY*, const RECT*, DWORD, D3DCOLOR);
        static HMODULE s_d3dx9 = nullptr;
        static D3DX9_LOAD_SURFACE_FROM_MEMORY  s_load_mem  = nullptr;
        static D3DX9_LOAD_SURFACE_FROM_SURFACE s_load_surf = nullptr;

        static bool load_d3dx9()
        {
            if (s_d3dx9) return s_load_mem && s_load_surf;
            s_d3dx9 = LoadLibraryW(L"D3DX9_43.dll");   // GMDirectX9 的 gex 已带此 DLL
            if (!s_d3dx9) return false;
            s_load_mem  = (D3DX9_LOAD_SURFACE_FROM_MEMORY)GetProcAddress(s_d3dx9, "D3DXLoadSurfaceFromMemory");
            s_load_surf = (D3DX9_LOAD_SURFACE_FROM_SURFACE)GetProcAddress(s_d3dx9, "D3DXLoadSurfaceFromSurface");
            return s_load_mem && s_load_surf;
        }

        HRESULT set_render_state(DWORD s, DWORD v)
        { return dev()->SetRenderState((D3DRENDERSTATETYPE)s, v); }

        // ---- 纹理 / 表面(一律不透明 void*, 内部转回 D3D9 类型) ----
        HRESULT get_device_from_texture(void* tex, void** out)
        { return ((IDirect3DTexture9*)tex)->GetDevice((IDirect3DDevice9**)out); }

        HRESULT get_surface_level(void* tex, UINT level, void** out)
        { return ((IDirect3DTexture9*)tex)->GetSurfaceLevel(level, (IDirect3DSurface9**)out); }

        // D3D8 的 CreateImageSurface 在 D3D9 的等价物是
        // CreateOffscreenPlainSurface(D3DPOOL_SYSTEMMEM, 可锁定)。
        HRESULT create_image_surface(UINT w, UINT h, DWORD fmt, void** out)
        { return dev()->CreateOffscreenPlainSurface(w, h, (D3DFORMAT)fmt, D3DPOOL_SYSTEMMEM, (IDirect3DSurface9**)out, nullptr); }

        DWORD get_surface_format(void*)
        {
            // 现代 Windows SDK 的精简 d3d9.h 不含 D3DSURFACE_DESC9/GetDesc,
            // D3D9 后端暂不查询格式(二期 DXVA2 硬解时补完整 D3D9 头)。
            return 0;
        }

        HRESULT lock_rect(void* surf, LockedRect* out, const RECT* rect, DWORD flags)
        {
            D3DLOCKED_RECT lr;
            HRESULT hr = ((IDirect3DSurface9*)surf)->LockRect(&lr, rect, flags);
            if (SUCCEEDED(hr)) { out->Pitch = lr.Pitch; out->pBits = lr.pBits; }
            return hr;
        }

        HRESULT unlock_rect(void* surf)
        { return ((IDirect3DSurface9*)surf)->UnlockRect(); }

        HRESULT add_dirty_rect(void* tex, const RECT* rect)
        { return ((IDirect3DTexture9*)tex)->AddDirtyRect(rect); }

        UINT get_level_count(void* tex)
        { return ((IDirect3DTexture9*)tex)->GetLevelCount(); }

        // ---- 表面上传(D3DX9) ----
        // GM8 的 surface 纹理是 D3DPOOL_DEFAULT, LockRect 返回 D3DERR_INVALIDCALL;
        // 纯 D3D9 API 无法把内存像素写入不可锁表面(UpdateSurface 要求源/目标同格式
        // 且目标非 RT; StretchRect 源必须是 RT), D3DXLoadSurfaceFromMemory 是唯一
        // 可行路径。D3DX9_43.dll 为必需依赖(GMDirectX9 的 gex 已自带)。
        HRESULT load_surface_from_memory(void* dst, const RECT* dstRect, const void* src,
                                         DWORD fmt, UINT pitch, const RECT* srcRect)
        {
            if (!dst || !src || !dstRect)
                return E_INVALIDARG;
            if (!load_d3dx9())
                return D3DERR_NOTAVAILABLE;
            return s_load_mem((IDirect3DSurface9*)dst, nullptr, dstRect,
                src, (D3DFORMAT)fmt, pitch, nullptr, srcRect, D3DX_FILTER_NONE, 0);
        }

        HRESULT load_surface_from_surface(void* dst, void* src)
        {
            if (!dst || !src) return E_INVALIDARG;
            if (!load_d3dx9()) return D3DERR_NOTAVAILABLE;
            return s_load_surf((IDirect3DSurface9*)dst, nullptr, nullptr,
                (IDirect3DSurface9*)src, nullptr, nullptr, D3DX_FILTER_NONE, 0);
        }

        void release(void* com)
        { if (com) ((IUnknown*)com)->Release(); }

        // ---- 错误文本(D3D9 精简表: 覆盖 NatureEnhance 实际会遇到的错误码) ----
        // 不引入 DX SDK 的 dxerr.cpp(3967 行, 依赖 d3d10/11 等头), 用 switch 表即可。
        std::string error_text(HRESULT hr)
        {
            switch (hr)
            {
            case D3D_OK:                                return "D3D_OK";
            case D3DERR_DEVICELOST:                     return "D3DERR_DEVICELOST";
            case D3DERR_DEVICENOTRESET:                 return "D3DERR_DEVICENOTRESET";
            case D3DERR_DEVICEREMOVED:                  return "D3DERR_DEVICEREMOVED";
            case D3DERR_DRIVERINTERNALERROR:            return "D3DERR_DRIVERINTERNALERROR";
            case D3DERR_INVALIDCALL:                    return "D3DERR_INVALIDCALL";
            case D3DERR_NOTAVAILABLE:                   return "D3DERR_NOTAVAILABLE";
            case D3DERR_NOTFOUND:                       return "D3DERR_NOTFOUND";
            case D3DERR_OUTOFVIDEOMEMORY:               return "D3DERR_OUTOFVIDEOMEMORY";
            case D3DERR_WASSTILLDRAWING:                return "D3DERR_WASSTILLDRAWING";
            case E_FAIL:                                return "E_FAIL";
            case E_INVALIDARG:                          return "E_INVALIDARG";
            case E_OUTOFMEMORY:                         return "E_OUTOFMEMORY";
            case E_NOTIMPL:                             return "E_NOTIMPL";
            default:
                char buf[32];
                sprintf(buf, "0x%08X", (unsigned)hr);
                return buf;
            }
        }
    }
}
