#include <d3d.h>
#include <winternl.h>
#include <../km/d3dkmthk.h>

#include "Common/HResultException.h"
#include "D3dDdi/Adapter.h"
#include "D3dDdi/Device.h"
#include "D3dDdi/DeviceFuncs.h"
#include "D3dDdi/Resource.h"

namespace
{
	HANDLE g_gdiResourceHandle = nullptr;
	D3dDdi::Resource* g_gdiResource = nullptr;
	bool g_isReadOnlyGdiLockEnabled = false;
}

namespace D3dDdi
{
	Device::Device(HANDLE adapter, HANDLE device)
		: m_origVtable(*DeviceFuncs::s_origVtablePtr)
		, m_adapter(Adapter::get(adapter))
		, m_device(device)
		, m_renderTarget(nullptr)
		, m_renderTargetSubResourceIndex(0)
		, m_sharedPrimary(nullptr)
		, m_streamSourceData{}
		, m_streamSource(nullptr)
	{
	}

	HRESULT Device::blt(const D3DDDIARG_BLT& data)
	{
		auto it = m_resources.find(data.hDstResource);
		if (it != m_resources.end())
		{
			return it->second.blt(data);
		}
		prepareForRendering(data.hSrcResource, data.SrcSubResourceIndex, true);
		return m_origVtable.pfnBlt(m_device, &data);
	}

	HRESULT Device::clear(const D3DDDIARG_CLEAR& data, UINT numRect, const RECT* rect)
	{
		if (data.Flags & D3DCLEAR_TARGET)
		{
			prepareForRendering();
		}
		return m_origVtable.pfnClear(m_device, &data, numRect, rect);
	}

	HRESULT Device::colorFill(const D3DDDIARG_COLORFILL& data)
	{
		auto it = m_resources.find(data.hResource);
		if (it != m_resources.end())
		{
			return it->second.colorFill(data);
		}
		return m_origVtable.pfnColorFill(m_device, &data);
	}

	template <typename Arg>
	HRESULT Device::createResourceImpl(Arg& data)
	{
		try
		{
			Resource resource(*this, data);
			m_resources.emplace(resource, std::move(resource));
			return S_OK;
		}
		catch (const HResultException& e)
		{
			return e.getResult();
		}
	}

	HRESULT Device::createResource(D3DDDIARG_CREATERESOURCE& data)
	{
		return createResourceImpl(data);
	}

	HRESULT Device::createResource2(D3DDDIARG_CREATERESOURCE2& data)
	{
		return createResourceImpl(data);
	}

	HRESULT Device::destroyResource(HANDLE resource)
	{
		if (g_gdiResource && resource == *g_gdiResource)
		{
			D3DDDIARG_LOCK lock = {};
			lock.hResource = *g_gdiResource;
			g_gdiResource->lock(lock);

			D3DDDIARG_UNLOCK unlock = {};
			unlock.hResource = *g_gdiResource;
			g_gdiResource->unlock(unlock);
		}

		if (resource == m_sharedPrimary)
		{
			D3DKMTReleaseProcessVidPnSourceOwners(GetCurrentProcess());
		}

		HRESULT result = m_origVtable.pfnDestroyResource(m_device, resource);
		if (SUCCEEDED(result))
		{
			m_resources.erase(resource);
			if (resource == m_sharedPrimary)
			{
				m_sharedPrimary = nullptr;
			}
			if (resource == g_gdiResourceHandle)
			{
				g_gdiResourceHandle = nullptr;
				g_gdiResource = nullptr;
			}
			if (resource == m_streamSource)
			{
				m_streamSource = nullptr;
			}
		}

		return result;
	}

	HRESULT Device::drawIndexedPrimitive(const D3DDDIARG_DRAWINDEXEDPRIMITIVE& data)
	{
		prepareForRendering();
		return m_origVtable.pfnDrawIndexedPrimitive(m_device, &data);
	}

	HRESULT Device::drawIndexedPrimitive2(const D3DDDIARG_DRAWINDEXEDPRIMITIVE2& data,
		UINT indicesSize, const void* indexBuffer, const UINT* flagBuffer)
	{
		prepareForRendering();
		return m_origVtable.pfnDrawIndexedPrimitive2(m_device, &data, indicesSize, indexBuffer, flagBuffer);
	}

	HRESULT Device::drawPrimitive(const D3DDDIARG_DRAWPRIMITIVE& data, const UINT* flagBuffer)
	{
		if (m_streamSource && 0 != m_streamSourceData.Stride)
		{
			m_streamSource->fixVertexData(m_streamSourceData.Offset + data.VStart * m_streamSourceData.Stride,
				data.PrimitiveCount, m_streamSourceData.Stride);
		}
		prepareForRendering();
		return m_origVtable.pfnDrawPrimitive(m_device, &data, flagBuffer);
	}

	HRESULT Device::drawPrimitive2(const D3DDDIARG_DRAWPRIMITIVE2& data)
	{
		prepareForRendering();
		return m_origVtable.pfnDrawPrimitive2(m_device, &data);
	}

	HRESULT Device::drawRectPatch(const D3DDDIARG_DRAWRECTPATCH& data, const D3DDDIRECTPATCH_INFO* info,
		const FLOAT* patch)
	{
		prepareForRendering();
		return m_origVtable.pfnDrawRectPatch(m_device, &data, info, patch);
	}

	HRESULT Device::drawTriPatch(const D3DDDIARG_DRAWTRIPATCH& data, const D3DDDITRIPATCH_INFO* info,
		const FLOAT* patch)
	{
		prepareForRendering();
		return m_origVtable.pfnDrawTriPatch(m_device, &data, info, patch);
	}

	HRESULT Device::lock(D3DDDIARG_LOCK& data)
	{
		auto it = m_resources.find(data.hResource);
		if (it != m_resources.end())
		{
			return it->second.lock(data);
		}
		return m_origVtable.pfnLock(m_device, &data);
	}

	HRESULT Device::openResource(D3DDDIARG_OPENRESOURCE& data)
	{
		HRESULT result = m_origVtable.pfnOpenResource(m_device, &data);
		if (SUCCEEDED(result) && data.Flags.Fullscreen)
		{
			m_sharedPrimary = data.hResource;
		}
		return result;
	}

	HRESULT Device::present(const D3DDDIARG_PRESENT& data)
	{
		prepareForRendering(data.hSrcResource, data.SrcSubResourceIndex, true);
		return m_origVtable.pfnPresent(m_device, &data);
	}

	HRESULT Device::present1(D3DDDIARG_PRESENT1& data)
	{
		for (UINT i = 0; i < data.SrcResources; ++i)
		{
			prepareForRendering(data.phSrcResources[i].hResource, data.phSrcResources[i].SubResourceIndex, true);
		}
		return m_origVtable.pfnPresent1(m_device, &data);
	}

	HRESULT Device::setRenderTarget(const D3DDDIARG_SETRENDERTARGET& data)
	{
		HRESULT result = m_origVtable.pfnSetRenderTarget(m_device, &data);
		if (SUCCEEDED(result) && 0 == data.RenderTargetIndex)
		{
			m_renderTarget = getResource(data.hRenderTarget);
			m_renderTargetSubResourceIndex = data.SubResourceIndex;
		}
		return result;
	}

	HRESULT Device::setStreamSource(const D3DDDIARG_SETSTREAMSOURCE& data)
	{
		HRESULT result = m_origVtable.pfnSetStreamSource(m_device, &data);
		if (SUCCEEDED(result) && 0 == data.Stream)
		{
			m_streamSourceData = data;
			m_streamSource = getResource(data.hVertexBuffer);
		}
		return result;
	}

	HRESULT Device::setStreamSourceUm(const D3DDDIARG_SETSTREAMSOURCEUM& data, const void* umBuffer)
	{
		HRESULT result = m_origVtable.pfnSetStreamSourceUm(m_device, &data, umBuffer);
		if (SUCCEEDED(result) && 0 == data.Stream)
		{
			m_streamSourceData = {};
			m_streamSource = nullptr;
		}
		return result;
	}

	HRESULT Device::unlock(const D3DDDIARG_UNLOCK& data)
	{
		auto it = m_resources.find(data.hResource);
		if (it != m_resources.end())
		{
			return it->second.unlock(data);
		}
		return m_origVtable.pfnUnlock(m_device, &data);
	}

	HRESULT Device::updateWInfo(const D3DDDIARG_WINFO& data)
	{
		if (1.0f == data.WNear && 1.0f == data.WFar)
		{
			D3DDDIARG_WINFO wInfo = {};
			wInfo.WNear = 0.0f;
			wInfo.WFar = 1.0f;
			return m_origVtable.pfnUpdateWInfo(m_device, &wInfo);
		}
		return m_origVtable.pfnUpdateWInfo(m_device, &data);
	}

	Resource* Device::getGdiResource()
	{
		return g_gdiResource;
	}

	void Device::prepareForRendering(HANDLE resource, UINT subResourceIndex, bool isReadOnly)
	{
		auto it = m_resources.find(resource);
		if (it != m_resources.end())
		{
			it->second.prepareForRendering(subResourceIndex, isReadOnly);
		}
	}

	void Device::prepareForRendering()
	{
		if (m_renderTarget)
		{
			m_renderTarget->prepareForRendering(m_renderTargetSubResourceIndex, false);
		}
	}

	void Device::add(HANDLE adapter, HANDLE device)
	{
		s_devices.emplace(device, Device(adapter, device));
	}

	Device& Device::get(HANDLE device)
	{
		auto it = s_devices.find(device);
		if (it != s_devices.end())
		{
			return it->second;
		}

		return s_devices.emplace(device, Device(nullptr, device)).first->second;
	}

	void Device::remove(HANDLE device)
	{
		s_devices.erase(device);
	}

	Resource* Device::getResource(HANDLE resource)
	{
		auto it = m_resources.find(resource);
		return it != m_resources.end() ? &it->second : nullptr;
	}

	Resource* Device::findResource(HANDLE resource)
	{
		for (auto& device : s_devices)
		{
			auto res = device.second.getResource(resource);
			if (res)
			{
				return res;
			}
		}
		return nullptr;
	}

	void Device::setGdiResourceHandle(HANDLE resource)
	{
		if ((!resource && !g_gdiResource) ||
			(g_gdiResource && resource == *g_gdiResource))
		{
			return;
		}

		if (g_gdiResource)
		{
			g_gdiResource->setAsGdiResource(false);
		}

		g_gdiResourceHandle = resource;
		g_gdiResource = findResource(resource);

		if (g_gdiResource)
		{
			g_gdiResource->setAsGdiResource(true);
		}
	}

	void Device::setReadOnlyGdiLock(bool enable)
	{
		g_isReadOnlyGdiLockEnabled = enable;
	}

	std::map<HANDLE, Device> Device::s_devices;
}
