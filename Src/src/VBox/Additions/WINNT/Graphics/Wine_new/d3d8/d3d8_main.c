/*
 * Direct3D 8
 *
 * Copyright 2005 Oliver Stieber
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 */

/*
 * Oracle LGPL Disclaimer: For the avoidance of doubt, except that if any license choice
 * other than GPL or LGPL is available it will apply instead, Oracle elects to use only
 * the Lesser General Public License version 2.1 (LGPLv2) at this time for any software where
 * a choice of LGPL license versions is made available with the language indicating
 * that LGPLv2 or any later version may be used, or where a choice of which version
 * of the LGPL is applied is otherwise unspecified.
 */

#include "config.h"
#include "initguid.h"
#include "d3d8_private.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3d8);

HRESULT WINAPI D3D8GetSWInfo(void) {
    FIXME("(void): stub\n");
    return 0;
}

void WINAPI DebugSetMute(void) {
    /* nothing to do */
}

IDirect3D8 * WINAPI DECLSPEC_HOTPATCH Direct3DCreate8(UINT sdk_version)
{
    struct d3d8 *object;

    TRACE("sdk_version %#x.\n", sdk_version);

    if (!(object = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*object))))
        return NULL;

    if (!d3d8_init(object))
    {
        WARN("Failed to initialize d3d8.\n");
        HeapFree(GetProcessHeap(), 0, object);
        return NULL;
    }

    TRACE("Created d3d8 object %p.\n", object);

    return &object->IDirect3D8_iface;
}

/* At process attach */
BOOL WINAPI DllMain(HINSTANCE hInstDLL, DWORD fdwReason, LPVOID lpv)
{
    TRACE("fdwReason=%d\n", fdwReason);
    if (fdwReason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(hInstDLL);

    return TRUE;
}

/***********************************************************************
 *              ValidateVertexShader (D3D8.@)
 *
 * I've seen reserved1 and reserved2 always passed as 0's
 * bool seems always passed as 0 or 1, but other values work as well...
 * toto       result?
 */
HRESULT WINAPI ValidateVertexShader(DWORD* vertexshader, DWORD* reserved1, DWORD* reserved2, BOOL bool, DWORD* toto)
{
  HRESULT ret;
  static BOOL warned;

  if (TRACE_ON(d3d8) || !warned) {
      FIXME("(%p %p %p %d %p): stub\n", vertexshader, reserved1, reserved2, bool, toto);
      warned = TRUE;
  }

  if (!vertexshader)
      return E_FAIL;

  if (reserved1 || reserved2)
      return E_FAIL;

  switch(*vertexshader) {
        case 0xFFFE0101:
        case 0xFFFE0100:
            ret=S_OK;
            break;
        default:
            WARN("Invalid shader version token %#x.\n", *vertexshader);
            ret=E_FAIL;
        }

  return ret;
}

/***********************************************************************
 *              ValidatePixelShader (D3D8.@)
 *
 * PARAMS
 * toto       result?
 */
HRESULT WINAPI ValidatePixelShader(DWORD* pixelshader, DWORD* reserved1, BOOL bool, DWORD* toto)
{
  HRESULT ret;
  static BOOL warned;

  if (TRACE_ON(d3d8) || !warned) {
      FIXME("(%p %p %d %p): stub\n", pixelshader, reserved1, bool, toto);
      warned = TRUE;
  }

  if (!pixelshader)
      return E_FAIL;

  if (reserved1)
      return E_FAIL;

  switch(*pixelshader) {
        case 0xFFFF0100:
        case 0xFFFF0101:
        case 0xFFFF0102:
        case 0xFFFF0103:
        case 0xFFFF0104:
            ret=S_OK;
            break;
        default:
            WARN("Invalid shader version token %#x.\n", *pixelshader);
            ret=E_FAIL;
        }
  return ret;
}
