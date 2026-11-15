#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <DirectXPackedVector.h>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>
using Microsoft::WRL::ComPtr;
static void Check(bool value, const char* message) {
	if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
static void Hr(HRESULT hr) { Check(SUCCEEDED(hr), "D3D operation failed"); }
struct Harness {
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> dc;
	ComPtr<ID3D11InfoQueue> debug;
	ComPtr<ID3D11ComputeShader> shader;
	ComPtr<ID3D11Buffer> result, readback, constants;
	ComPtr<ID3D11UnorderedAccessView> uav;
	ComPtr<ID3D11SamplerState> sampler;
	Harness(const wchar_t* path) {
		HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG,
			nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &dc);
		if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING) hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP,
			nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &dc);
		Hr(hr); device.As(&debug);
		ComPtr<ID3DBlob> code, errors;
		hr = D3DCompileFromFile(path, nullptr, nullptr, "main", "cs_5_0",
			D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
		if (errors) std::cerr << static_cast<const char*>(errors->GetBufferPointer());
		Hr(hr); Hr(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader));
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = bd.StructureByteStride = 4; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		Hr(device->CreateBuffer(&bd, nullptr, &result));
		D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
		ud.Format = DXGI_FORMAT_R32_UINT; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; ud.Buffer.NumElements = 1;
		Hr(device->CreateUnorderedAccessView(result.Get(), &ud, &uav));
		bd.Usage = D3D11_USAGE_STAGING; bd.BindFlags = 0; bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		Hr(device->CreateBuffer(&bd, nullptr, &readback));
		bd = {}; bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		Hr(device->CreateBuffer(&bd, nullptr, &constants));
		D3D11_SAMPLER_DESC sd{};
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.ComparisonFunc = D3D11_COMPARISON_NEVER; sd.MaxLOD = D3D11_FLOAT32_MAX;
		Hr(device->CreateSamplerState(&sd, &sampler));
	}
	bool Duplicate(ID3D11ShaderResourceView* a, ID3D11ShaderResourceView* b, UINT w, UINT h, bool alpha) {
		const UINT zero[4]{}; const UINT options[4]{alpha ? 1u : 0u, 0, 0, 0};
		dc->UpdateSubresource(constants.Get(), 0, nullptr, options, 0, 0);
		dc->ClearUnorderedAccessViewUint(uav.Get(), zero);
		ID3D11ShaderResourceView* srvs[2]{a,b};
		ID3D11Buffer* cb = constants.Get(); ID3D11UnorderedAccessView* target = uav.Get(); ID3D11SamplerState* sam = sampler.Get();
		dc->CSSetShader(shader.Get(), nullptr, 0); dc->CSSetShaderResources(0,2,srvs);
		dc->CSSetSamplers(0,1,&sam); dc->CSSetConstantBuffers(0,1,&cb); dc->CSSetUnorderedAccessViews(0,1,&target,nullptr);
		dc->Dispatch((w+15)/16,(h+15)/16,1);
		ID3D11ShaderResourceView* empty[2]{}; target=nullptr; cb=nullptr;
		dc->CSSetShaderResources(0,2,empty); dc->CSSetUnorderedAccessViews(0,1,&target,nullptr); dc->CSSetConstantBuffers(0,1,&cb);
		dc->CopyResource(readback.Get(), result.Get());
		D3D11_MAPPED_SUBRESOURCE mapped{}; Hr(dc->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped));
		UINT value=1; std::memcpy(&value,mapped.pData,sizeof(value)); dc->Unmap(readback.Get(),0);
		return value==0;
	}
	void CheckDebug() {
		if (!debug) { std::cout << "D3D debug layer unavailable; message check skipped.\n"; return; }
		for (UINT64 i=0;i<debug->GetNumStoredMessages();++i) {
			SIZE_T size=0; Hr(debug->GetMessage(i,nullptr,&size));
			std::vector<char> bytes(size); auto* message=reinterpret_cast<D3D11_MESSAGE*>(bytes.data());
			Hr(debug->GetMessage(i,message,&size));
			if (message->Severity<=D3D11_MESSAGE_SEVERITY_WARNING) {
				std::cerr << message->pDescription << '\n'; Check(false,"D3D debug layer reported a resource/synchronization issue");
			}
		}
	}
};
int wmain(int argc, wchar_t** argv) {
	Check(argc==2,"Pass the production DuplicateFrameCS.hlsl path");
	Harness test(argv[1]); unsigned cases=0;
	for (DXGI_FORMAT format : {DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM,
		DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R32G32B32A32_FLOAT}) {
		const unsigned component = format==DXGI_FORMAT_R16G16B16A16_FLOAT ? 2u : format==DXGI_FORMAT_R32G32B32A32_FLOAT ? 4u : 1u;
		for (const auto& extent : {std::array<UINT,2>{1,1},{2,3},{15,17},{17,19},{33,31},{65,3}}) {
			const auto w=extent[0], h=extent[1]; const unsigned stride=component*4;
			std::vector<unsigned char> image(static_cast<size_t>(w)*h*stride,64);
			if (component==2) {
				const uint16_t values[4]{0xB800,0x4800,0x3000,0x3C00}; // -.5, 8, .125, 1.
				for (size_t i=0;i<image.size();i+=stride) std::memcpy(image.data()+i,values,stride);
			} else if (component==4) {
				const float values[4]{-.5f,8.f,.125f,1.f};
				for (size_t i=0;i<image.size();i+=stride) std::memcpy(image.data()+i,values,stride);
			}
			D3D11_TEXTURE2D_DESC td{}; td.Width=w; td.Height=h; td.ArraySize=td.MipLevels=td.SampleDesc.Count=1;
			td.Format=format; td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
			D3D11_SUBRESOURCE_DATA initial{}; initial.pSysMem=image.data(); initial.SysMemPitch=w*stride;
			ComPtr<ID3D11Texture2D> a,b; ComPtr<ID3D11ShaderResourceView> as,bs;
			Hr(test.device->CreateTexture2D(&td,&initial,&a)); Hr(test.device->CreateTexture2D(&td,&initial,&b));
			Hr(test.device->CreateShaderResourceView(a.Get(),nullptr,&as)); Hr(test.device->CreateShaderResourceView(b.Get(),nullptr,&bs));
			Check(test.Duplicate(as.Get(),bs.Get(),w,h,false),"equal RGB images differed"); ++cases;
			for (unsigned pixel=0;pixel<w*h;++pixel) {
				// Exhaust every pixel for small odd images; cover the complete border
				// and the central pixel across multi-group images as well.
				if (w*h>400 && pixel/w>0 && pixel/w<h-1 && pixel%w>0 && pixel%w<w-1 && pixel!=(w*h)/2) continue;
				for (unsigned channel=0;channel<4;++channel) {
					auto changed=image; changed[static_cast<size_t>(pixel)*stride+channel*component] ^= 1;
					test.dc->UpdateSubresource(b.Get(),0,nullptr,changed.data(),w*stride,0);
					Check(!test.Duplicate(as.Get(),bs.Get(),w,h,channel==3),"single-pixel/channel/edge change was filtered"); ++cases;
					if (channel==3) { Check(test.Duplicate(as.Get(),bs.Get(),w,h,false),"SDR RGB-only compatibility changed"); ++cases; }
				}
			}
			if (component==4) {
				const float nan=std::numeric_limits<float>::quiet_NaN(); std::memcpy(image.data(),&nan,4);
				test.dc->UpdateSubresource(a.Get(),0,nullptr,image.data(),w*stride,0);
				test.dc->UpdateSubresource(b.Get(),0,nullptr,image.data(),w*stride,0);
				Check(!test.Duplicate(as.Get(),bs.Get(),w,h,true),"NaN was hidden as a duplicate"); ++cases;
			}
		}
	}
	test.CheckDebug();
	std::cout << "Production duplicate shader: " << cases << " WARP cases passed (formats, exact channels, edges, alpha, HDR and NaN).\n";
}
