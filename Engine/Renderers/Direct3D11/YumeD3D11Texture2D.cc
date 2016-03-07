//----------------------------------------------------------------------------
//Yume Engine
//Copyright (C) 2015  arkenthera
//This program is free software; you can redistribute it and/or modify
//it under the terms of the GNU General Public License as published by
//the Free Software Foundation; either version 2 of the License, or
//(at your option) any later version.
//This program is distributed in the hope that it will be useful,
//but WITHOUT ANY WARRANTY; without even the implied warranty of
//MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//GNU General Public License for more details.
//You should have received a copy of the GNU General Public License along
//with this program; if not, write to the Free Software Foundation, Inc.,
//51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.*/
//----------------------------------------------------------------------------
//
// File : <Filename>
// Date : <Date>
// Comments :
//
//----------------------------------------------------------------------------
#include "YumeHeaders.h"
#include "YumeD3D11Texture2D.h"

#include "YumeD3D11Renderer.h"
#include "Renderer/YumeRHI.h"

#include "YumeD3D11Renderer.h"
#include "YumeD3D11RendererImpl.h"
#include "YumeD3D11Renderable.h"

#include "Renderer/YumeImage.h"

#include "Renderer/YumeRenderer.h"

#include "Renderer/YumeRenderable.h"

#include "Logging/logging.h"


namespace YumeEngine
{

	static const char* addressModeNames[] =
	{
		"wrap",
		"mirror",
		"clamp",
		"border",
		0
	};

	static const char* filterModeNames[] =
	{
		"nearest",
		"bilinear",
		"trilinear",
		"anisotropic",
		"default",
		0
	};

	static const D3D11_FILTER d3dFilterMode[] =
	{
		D3D11_FILTER_MIN_MAG_MIP_POINT,
		D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT,
		D3D11_FILTER_MIN_MAG_MIP_LINEAR,
		D3D11_FILTER_ANISOTROPIC,
		D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT,
		D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT,
		D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR,
		D3D11_FILTER_COMPARISON_ANISOTROPIC
	};

	static const D3D11_TEXTURE_ADDRESS_MODE d3dAddressMode[] =
	{
		D3D11_TEXTURE_ADDRESS_WRAP,
		D3D11_TEXTURE_ADDRESS_MIRROR,
		D3D11_TEXTURE_ADDRESS_CLAMP,
		D3D11_TEXTURE_ADDRESS_BORDER
	};

	YumeD3D11Texture2D::YumeD3D11Texture2D(YumeRHI* rhi)
		: YumeD3D11Resource(static_cast<YumeD3D11Renderer*>(rhi))
	{
		format_ = (DXGI_FORMAT_UNKNOWN);
	}

	YumeD3D11Texture2D::~YumeD3D11Texture2D()
	{
		Release();
	}

	void YumeD3D11Texture2D::Release()
	{
		if(rhi_ && object_)
		{
			for(unsigned i = 0; i < MAX_TEXTURE_UNITS; ++i)
			{
				if(rhi_->GetTexture(i) == this)
					rhi_->SetTexture(i,0);
			}
		}

		if(renderSurface_)
			renderSurface_->Release();

		D3D_SAFE_RELEASE(object_);
		D3D_SAFE_RELEASE(shaderResourceView_);
		D3D_SAFE_RELEASE(sampler_);
	}


	bool YumeD3D11Texture2D::SetSize(int width,int height,unsigned format,TextureUsage usage)
	{
		if(width <= 0 || height <= 0)
		{
			YUMELOG_ERROR("Zero or negative texture dimensions");
			return false;
		}

		// Delete the old rendersurface if any
		renderSurface_.reset();
		usage_ = usage;

		if(usage_ == TEXTURE_RENDERTARGET || usage_ == TEXTURE_DEPTHSTENCIL)
		{
			renderSurface_ = SharedPtr<YumeRenderable>(new YumeD3D11Renderable(this));

			// Clamp mode addressing by default, nearest filtering, and mipmaps disabled
			addressMode_[COORD_U] = ADDRESS_CLAMP;
			addressMode_[COORD_V] = ADDRESS_CLAMP;
			filterMode_ = FILTER_NEAREST;
			requestedLevels_ = 1;
		}
		else if(usage_ == TEXTURE_DYNAMIC)
			requestedLevels_ = 1;

		//if(usage_ == TEXTURE_RENDERTARGET)
		//	SubscribeToEvent(E_RENDERSURFACEUPDATE,URHO3D_HANDLER(Texture2D,HandleRenderSurfaceUpdate));
		//else
		//	UnsubscribeFromEvent(E_RENDERSURFACEUPDATE);

		width_ = width;
		height_ = height;
		format_ = format;

		return Create();
	}

	bool YumeD3D11Texture2D::SetData(unsigned level,int x,int y,int width,int height,const void* data)
	{
		if(!object_)
		{
			YUMELOG_ERROR("No texture created, can not set data");
			return false;
		}

		if(!data)
		{
			YUMELOG_ERROR("Null source for setting data");
			return false;
		}

		if(level >= levels_)
		{
			YUMELOG_ERROR("Illegal mip level for setting data");
			return false;
		}

		int levelWidth = GetLevelWidth(level);
		int levelHeight = GetLevelHeight(level);
		if(x < 0 || x + width > levelWidth || y < 0 || y + height > levelHeight || width <= 0 || height <= 0)
		{
			YUMELOG_ERROR("Illegal dimensions for setting data");
			return false;
		}

		// If compressed, align the update region on a block
		if(IsCompressed())
		{
			x &= ~3;
			y &= ~3;
			width += 3;
			width &= 0xfffffffc;
			height += 3;
			height &= 0xfffffffc;
		}

		unsigned char* src = (unsigned char*)data;
		unsigned rowSize = GetRowDataSize(width);
		unsigned rowStart = GetRowDataSize(x);
		unsigned subResource = D3D11CalcSubresource(level,0,levels_);

		if(usage_ == TEXTURE_DYNAMIC)
		{
			if(IsCompressed())
			{
				height = (height + 3) >> 2;
				y >>= 2;
			}

			D3D11_MAPPED_SUBRESOURCE mappedData;
			mappedData.pData = 0;

			HRESULT hr = static_cast<YumeD3D11Renderer*>(rhi_)->GetImpl()->GetDeviceContext()->Map((ID3D11Resource*)object_,subResource,D3D11_MAP_WRITE_DISCARD,0,
				&mappedData);
			if(FAILED(hr) || !mappedData.pData)
			{
				YUMELOG_ERROR("Failed to map texture for update",hr);
				return false;
			}
			else
			{
				for(int row = 0; row < height; ++row)
					memcpy((unsigned char*)mappedData.pData + (row + y) * mappedData.RowPitch + rowStart,src + row * rowSize,rowSize);
				static_cast<YumeD3D11Renderer*>(rhi_)->GetImpl()->GetDeviceContext()->Unmap((ID3D11Resource*)object_,subResource);
			}
		}
		else
		{
			D3D11_BOX destBox;
			destBox.left = (UINT)x;
			destBox.right = (UINT)(x + width);
			destBox.top = (UINT)y;
			destBox.bottom = (UINT)(y + height);
			destBox.front = 0;
			destBox.back = 1;

			static_cast<YumeD3D11Renderer*>(rhi_)->GetImpl()->GetDeviceContext()->UpdateSubresource((ID3D11Resource*)object_,subResource,&destBox,data,
				rowSize,0);
		}

		return true;
	}

	bool YumeD3D11Texture2D::SetData(SharedPtr<YumeImage> image,bool useAlpha)
	{
		if(!image)
		{
			YUMELOG_ERROR("Null image, can not load texture");
			return false;
		}

		unsigned memoryUse = sizeof(YumeD3D11Texture2D);

		int quality = QUALITY_HIGH;
		YumeRenderer* renderer = 0;
		if(renderer)
			quality = renderer->GetTextureQuality();

		if(!image->IsCompressed())
		{
			// Convert unsuitable formats to RGBA
			unsigned components = image->GetComponents();
			if((components == 1 && !useAlpha) || components == 2 || components == 3)
			{
				image = image->ConvertToRGBA();
				if(!image)
					return false;
				components = image->GetComponents();
			}

			unsigned char* levelData = image->GetData();
			int levelWidth = image->GetWidth();
			int levelHeight = image->GetHeight();
			unsigned format = 0;

			// Discard unnecessary mip levels
			for(unsigned i = 0; i < mipsToSkip_[quality]; ++i)
			{
				image = image->GetNextLevel();
				levelData = image->GetData();
				levelWidth = image->GetWidth();
				levelHeight = image->GetHeight();
			}

			switch(components)
			{
			case 1:
				format = YumeD3D11Renderer::GetAlphaFormat();
				break;

			case 4:
				format = YumeD3D11Renderer::GetRGBAFormat();
				break;

			default: break;
			}

			// If image was previously compressed, reset number of requested levels to avoid error if level count is too high for new size
			if(IsCompressed() && requestedLevels_ > 1)
				requestedLevels_ = 0;
			SetSize(levelWidth,levelHeight,format);

			for(unsigned i = 0; i < levels_; ++i)
			{
				SetData(i,0,0,levelWidth,levelHeight,levelData);
				memoryUse += levelWidth * levelHeight * components;

				if(i < levels_ - 1)
				{
					image = image->GetNextLevel();
					levelData = image->GetData();
					levelWidth = image->GetWidth();
					levelHeight = image->GetHeight();
				}
			}
		}
		else
		{
			int width = image->GetWidth();
			int height = image->GetHeight();
			unsigned levels = image->GetNumCompressedLevels();
			unsigned format = rhi_->GetFormat(image->GetCompressedFormat());
			bool needDecompress = false;

			if(!format)
			{
				format = YumeD3D11Renderer::GetRGBAFormat();
				needDecompress = true;
			}

			unsigned mipsToSkip = mipsToSkip_[quality];
			if(mipsToSkip >= levels)
				mipsToSkip = levels - 1;
			while(mipsToSkip && (width / (1 << mipsToSkip) < 4 || height / (1 << mipsToSkip) < 4))
				--mipsToSkip;
			width /= (1 << mipsToSkip);
			height /= (1 << mipsToSkip);

			SetNumLevels((unsigned)std::max((int)(levels - mipsToSkip),1));
			SetSize(width,height,format);

			for(unsigned i = 0; i < levels_ && i < levels - mipsToSkip; ++i)
			{
				CompressedLevel level = image->GetCompressedLevel(i + mipsToSkip);
				if(!needDecompress)
				{
					SetData(i,0,0,level.width_,level.height_,level.data_);
					memoryUse += level.rows_ * level.rowSize_;
				}
				else
				{
					unsigned char* rgbaData = new unsigned char[level.width_ * level.height_ * 4];
					level.Decompress(rgbaData);
					SetData(i,0,0,level.width_,level.height_,rgbaData);
					memoryUse += level.width_ * level.height_ * 4;
					delete[] rgbaData;
				}
			}
		}

		SetMemoryUsage(memoryUse);
		return true;
	}

	bool YumeD3D11Texture2D::GetData(unsigned level,void* dest) const
	{
		if(!object_)
		{
			YUMELOG_ERROR("No texture created, can not get data");
			return false;
		}

		if(!dest)
		{
			YUMELOG_ERROR("Null destination for getting data");
			return false;
		}

		if(level >= levels_)
		{
			YUMELOG_ERROR("Illegal mip level for getting data");
			return false;
		}

		int levelWidth = GetLevelWidth(level);
		int levelHeight = GetLevelHeight(level);

		D3D11_TEXTURE2D_DESC textureDesc;
		memset(&textureDesc,0,sizeof textureDesc);
		textureDesc.Width = (UINT)levelWidth;
		textureDesc.Height = (UINT)levelHeight;
		textureDesc.MipLevels = 1;
		textureDesc.ArraySize = 1;
		textureDesc.Format = (DXGI_FORMAT)format_;
		textureDesc.SampleDesc.Count = 1;
		textureDesc.SampleDesc.Quality = 0;
		textureDesc.Usage = D3D11_USAGE_STAGING;
		textureDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

		ID3D11Texture2D* stagingTexture = 0;
		HRESULT hr = static_cast<YumeD3D11Renderer*>(rhi_)->GetImpl()->GetDevice()->CreateTexture2D(&textureDesc,0,&stagingTexture);
		if(FAILED(hr))
		{
			D3D_SAFE_RELEASE(stagingTexture);
			YUMELOG_ERROR("Failed to create staging texture for GetData",hr);
			return false;
		}

		unsigned srcSubResource = D3D11CalcSubresource(level,0,levels_);
		D3D11_BOX srcBox;
		srcBox.left = 0;
		srcBox.right = (UINT)levelWidth;
		srcBox.top = 0;
		srcBox.bottom = (UINT)levelHeight;
		srcBox.front = 0;
		srcBox.back = 1;
		static_cast<YumeD3D11Renderer*>(rhi_)->GetImpl()->GetDeviceContext()->CopySubresourceRegion(stagingTexture,0,0,0,0,(ID3D11Resource*)object_,
			srcSubResource,&srcBox);

		D3D11_MAPPED_SUBRESOURCE mappedData;
		mappedData.pData = 0;
		unsigned rowSize = GetRowDataSize(levelWidth);
		unsigned numRows = (unsigned)(IsCompressed() ? (levelHeight + 3) >> 2 : levelHeight);

		hr = static_cast<YumeD3D11Renderer*>(rhi_)->GetImpl()->GetDeviceContext()->Map((ID3D11Resource*)stagingTexture,0,D3D11_MAP_READ,0,&mappedData);
		if(FAILED(hr) || !mappedData.pData)
		{
			YUMELOG_ERROR("Failed to map staging texture for GetData",hr);
			stagingTexture->Release();
			return false;
		}
		else
		{
			for(unsigned row = 0; row < numRows; ++row)
				memcpy((unsigned char*)dest + row * rowSize,(unsigned char*)mappedData.pData + row * mappedData.RowPitch,rowSize);
			static_cast<YumeD3D11Renderer*>(rhi_)->GetImpl()->GetDeviceContext()->Unmap((ID3D11Resource*)stagingTexture,0);
			stagingTexture->Release();
			return true;
		}
	}

	bool YumeD3D11Texture2D::Create()
	{
		Release();

		if(!rhi_ || !width_ || !height_)
			return false;

		levels_ = CheckMaxLevels(width_,height_,requestedLevels_);

		D3D11_TEXTURE2D_DESC textureDesc;
		memset(&textureDesc,0,sizeof textureDesc);
		textureDesc.Width = (UINT)width_;
		textureDesc.Height = (UINT)height_;
		textureDesc.MipLevels = levels_;
		textureDesc.ArraySize = 1;
		textureDesc.Format = (DXGI_FORMAT)(sRGB_ ? GetSRGBFormat(format_) : format_);
		textureDesc.SampleDesc.Count = 1;
		textureDesc.SampleDesc.Quality = 0;
		textureDesc.Usage = usage_ == TEXTURE_DYNAMIC ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
		textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		if(usage_ == TEXTURE_RENDERTARGET)
			textureDesc.BindFlags |= D3D11_BIND_RENDER_TARGET;
		else if(usage_ == TEXTURE_DEPTHSTENCIL)
			textureDesc.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
		textureDesc.CPUAccessFlags = usage_ == TEXTURE_DYNAMIC ? D3D11_CPU_ACCESS_WRITE : 0;

		HRESULT hr = static_cast<YumeD3D11Renderer*>(rhi_)->GetImpl()->GetDevice()->CreateTexture2D(&textureDesc,0,(ID3D11Texture2D**)&object_);
		if(FAILED(hr))
		{
			D3D_SAFE_RELEASE(object_);
			YUMELOG_ERROR("Failed to create texture",hr);
			return false;
		}

		D3D11_SHADER_RESOURCE_VIEW_DESC resourceViewDesc;
		memset(&resourceViewDesc,0,sizeof resourceViewDesc);
		resourceViewDesc.Format = (DXGI_FORMAT)GetSRVFormat(textureDesc.Format);
		resourceViewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		resourceViewDesc.Texture2D.MipLevels = (UINT)levels_;

		hr = static_cast<YumeD3D11Renderer*>(rhi_)->GetImpl()->GetDevice()->CreateShaderResourceView((ID3D11Resource*)object_,&resourceViewDesc,
			(ID3D11ShaderResourceView**)&shaderResourceView_);
		if(FAILED(hr))
		{
			D3D_SAFE_RELEASE(shaderResourceView_);
			YUMELOG_ERROR("Failed to create shader resource view for texture",hr);
			return false;
		}

		if(usage_ == TEXTURE_RENDERTARGET)
		{
			D3D11_RENDER_TARGET_VIEW_DESC renderTargetViewDesc;
			memset(&renderTargetViewDesc,0,sizeof renderTargetViewDesc);
			renderTargetViewDesc.Format = textureDesc.Format;
			renderTargetViewDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;

			hr = static_cast<YumeD3D11Renderer*>(rhi_)->GetImpl()->GetDevice()->CreateRenderTargetView((ID3D11Resource*)object_,&renderTargetViewDesc,
				(ID3D11RenderTargetView**)&renderSurface_->renderTargetView_);
			if(FAILED(hr))
			{
				D3D_SAFE_RELEASE(renderSurface_->renderTargetView_);
				YUMELOG_ERROR("Failed to create rendertarget view for texture",hr);
				return false;
			}
		}
		else if(usage_ == TEXTURE_DEPTHSTENCIL)
		{
			D3D11_DEPTH_STENCIL_VIEW_DESC depthStencilViewDesc;
			memset(&depthStencilViewDesc,0,sizeof depthStencilViewDesc);
			depthStencilViewDesc.Format = (DXGI_FORMAT)GetDSVFormat(textureDesc.Format);
			depthStencilViewDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;

			hr = static_cast<YumeD3D11Renderer*>(rhi_)->GetImpl()->GetDevice()->CreateDepthStencilView((ID3D11Resource*)object_,&depthStencilViewDesc,
				(ID3D11DepthStencilView**)&renderSurface_->renderTargetView_);
			if(FAILED(hr))
			{
				D3D_SAFE_RELEASE(renderSurface_->renderTargetView_);
				YUMELOG_ERROR("Failed to create depth-stencil view for texture",hr);
				return false;
			}

			// Create also a read-only version of the view for simultaneous depth testing and sampling in shader
			depthStencilViewDesc.Flags = D3D11_DSV_READ_ONLY_DEPTH;
			hr = static_cast<YumeD3D11Renderer*>(rhi_)->GetImpl()->GetDevice()->CreateDepthStencilView((ID3D11Resource*)object_,&depthStencilViewDesc,
				(ID3D11DepthStencilView**)&renderSurface_->readOnlyView_);
			if(FAILED(hr))
			{
				D3D_SAFE_RELEASE(renderSurface_->readOnlyView_);
				YUMELOG_ERROR("Failed to create read-only depth-stencil view for texture" << hr);
			}
		}

		return true;
	}

	void YumeD3D11Texture2D::HandleRenderSurfaceUpdate(YumeHash eventType,VariantMap& eventData)
	{
		if(renderSurface_ && (renderSurface_->GetUpdateMode() == SURFACE_UPDATEALWAYS || renderSurface_->IsUpdateQueued()))
		{
			YumeRenderer* renderer = 0;
			if(renderer)
				renderer->QueueRenderable(renderSurface_.get());
			renderSurface_->ResetUpdateQueued();
		}
	}

	unsigned YumeD3D11Texture2D::GetRowDataSize(int width) const
	{
		switch(format_)
		{
		case DXGI_FORMAT_R8_UNORM:
		case DXGI_FORMAT_A8_UNORM:
			return (unsigned)width;

		case DXGI_FORMAT_R8G8_UNORM:
		case DXGI_FORMAT_R16_UNORM:
		case DXGI_FORMAT_R16_FLOAT:
		case DXGI_FORMAT_R16_TYPELESS:
			return (unsigned)(width * 2);

		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R16G16_UNORM:
		case DXGI_FORMAT_R16G16_FLOAT:
		case DXGI_FORMAT_R32_FLOAT:
		case DXGI_FORMAT_R24G8_TYPELESS:
		case DXGI_FORMAT_R32_TYPELESS:
			return (unsigned)(width * 4);

		case DXGI_FORMAT_R16G16B16A16_UNORM:
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
			return (unsigned)(width * 8);

		case DXGI_FORMAT_R32G32B32A32_FLOAT:
			return (unsigned)(width * 16);

		case DXGI_FORMAT_BC1_UNORM:
			return (unsigned)(((width + 3) >> 2) * 8);

		case DXGI_FORMAT_BC2_UNORM:
		case DXGI_FORMAT_BC3_UNORM:
			return (unsigned)(((width + 3) >> 2) * 16);

		default:
			return 0;
		}
	}
	void YumeD3D11Texture2D::UpdateParameters()
	{
		if((!parametersDirty_ && sampler_) || !object_)
			return;

		// Release old sampler
		D3D_SAFE_RELEASE(sampler_);

		D3D11_SAMPLER_DESC samplerDesc;
		memset(&samplerDesc,0,sizeof samplerDesc);
		unsigned filterModeIndex = filterMode_ != FILTER_DEFAULT ? filterMode_ : rhi_->GetDefaultTextureFilterMode();
		if(shadowCompare_)
			filterModeIndex += 4;
		samplerDesc.Filter = d3dFilterMode[filterModeIndex];
		samplerDesc.AddressU = d3dAddressMode[addressMode_[0]];
		samplerDesc.AddressV = d3dAddressMode[addressMode_[1]];
		samplerDesc.AddressW = d3dAddressMode[addressMode_[2]];
		samplerDesc.MaxAnisotropy = rhi_->GetTextureAnisotropy();
		samplerDesc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
		samplerDesc.MinLOD = -M_INFINITY;
		samplerDesc.MaxLOD = M_INFINITY;
		memcpy(&samplerDesc.BorderColor,borderColor_.Data(),4 * sizeof(float));

		HRESULT hr = static_cast<YumeD3D11Renderer*>(rhi_)->GetImpl()->GetDevice()->CreateSamplerState(&samplerDesc,(ID3D11SamplerState**)&sampler_);
		if(FAILED(hr))
		{
			D3D_SAFE_RELEASE(sampler_);
			YUMELOG_ERROR("Failed to create sampler state" << hr);
		}

		parametersDirty_ = false;
	}

	void YumeD3D11Texture2D::CheckTextureBudget(YumeHash type)
	{

	}


	unsigned YumeD3D11Texture2D::GetSRVFormat(unsigned format)
	{
		if(format == DXGI_FORMAT_R24G8_TYPELESS)
			return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
		else if(format == DXGI_FORMAT_R16_TYPELESS)
			return DXGI_FORMAT_R16_UNORM;
		else if(format == DXGI_FORMAT_R32_TYPELESS)
			return DXGI_FORMAT_R32_FLOAT;
		else
			return format;
	}

	unsigned YumeD3D11Texture2D::GetDSVFormat(unsigned format)
	{
		if(format == DXGI_FORMAT_R24G8_TYPELESS)
			return DXGI_FORMAT_D24_UNORM_S8_UINT;
		else if(format == DXGI_FORMAT_R16_TYPELESS)
			return DXGI_FORMAT_D16_UNORM;
		else if(format == DXGI_FORMAT_R32_TYPELESS)
			return DXGI_FORMAT_D32_FLOAT;
		else
			return format;
	}

	unsigned YumeD3D11Texture2D::GetSRGBFormat(unsigned format)
	{
		if(format == DXGI_FORMAT_R8G8B8A8_UNORM)
			return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		else if(format == DXGI_FORMAT_BC1_UNORM)
			return DXGI_FORMAT_BC1_UNORM_SRGB;
		else if(format == DXGI_FORMAT_BC2_UNORM)
			return DXGI_FORMAT_BC2_UNORM_SRGB;
		else if(format == DXGI_FORMAT_BC3_UNORM)
			return DXGI_FORMAT_BC3_UNORM_SRGB;
		else
			return format;
	}

	bool YumeD3D11Texture2D::IsCompressed() const
	{
		return format_ == DXGI_FORMAT_BC1_UNORM || format_ == DXGI_FORMAT_BC2_UNORM || format_ == DXGI_FORMAT_BC3_UNORM;
	}

}