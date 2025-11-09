// Copyright Epic Games, Inc. All Rights Reserved.

#include "NVENC/NVENCCaps.h"

#if WITH_OMNI_NVENC

#include "NVENC/NVENCPlatform.h"
#include "NVENC/NVEncodeAPILoader.h"
#include "NVENC/NVENCDefs.h"
#include "NVENC/NVENCSession.h"
#include "Logging/LogMacros.h"
#include "Misc/ScopeExit.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <d3d11.h>
#if OMNI_WITH_D3D12_RHI
#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi.h>
#endif
#include "Windows/HideWindowsPlatformTypes.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogNVENCCaps, Log, All);

namespace OmniNVENC
{
#if PLATFORM_WINDOWS && OMNI_WITH_D3D12_RHI
    namespace
    {
        bool CreateProbeD3D12Device(TRefCountPtr<ID3D12Device>& OutDevice)
        {
            TRefCountPtr<IDXGIFactory1> DxgiFactory;
            HRESULT Hr = CreateDXGIFactory1(IID_PPV_ARGS(DxgiFactory.GetInitReference()));
            if (FAILED(Hr))
            {
                UE_LOG(LogNVENCCaps, Warning, TEXT("Failed to create DXGI factory for D3D12 NVENC probe (0x%08x)."), Hr);
                return false;
            }

            for (UINT AdapterIndex = 0;; ++AdapterIndex)
            {
                TRefCountPtr<IDXGIAdapter1> Adapter;
                Hr = DxgiFactory->EnumAdapters1(AdapterIndex, Adapter.GetInitReference());
                if (Hr == DXGI_ERROR_NOT_FOUND)
                {
                    break;
                }

                if (FAILED(Hr) || !Adapter.IsValid())
                {
                    continue;
                }

                DXGI_ADAPTER_DESC1 AdapterDesc;
                if (FAILED(Adapter->GetDesc1(&AdapterDesc)))
                {
                    continue;
                }

                if ((AdapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
                {
                    continue;
                }

                Hr = D3D12CreateDevice(Adapter.GetReference(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(OutDevice.GetInitReference()));
                if (SUCCEEDED(Hr))
                {
                    return true;
                }
            }

            Hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(OutDevice.GetInitReference()));
            if (FAILED(Hr))
            {
                UE_LOG(LogNVENCCaps, Warning, TEXT("Failed to create fallback D3D12 device for NVENC probe (0x%08x)."), Hr);
                return false;
            }

            return true;
        }
    }
#endif // PLATFORM_WINDOWS && OMNI_WITH_D3D12_RHI

    bool FNVENCCaps::Query(ENVENCCodec Codec, FNVENCCapabilities& OutCapabilities)
    {
        OutCapabilities = FNVENCCapabilities();

        FNVEncodeAPILoader& Loader = FNVEncodeAPILoader::Get();
        if (!Loader.Load())
        {
            UE_LOG(LogNVENCCaps, Warning, TEXT("NVENC capability query failed – loader was unable to resolve the runtime."));
            return false;
        }

#if !PLATFORM_WINDOWS
        UE_LOG(LogNVENCCaps, Warning, TEXT("NVENC capability probing is only supported on Windows."));
        return false;
#else
        TRefCountPtr<ID3D11Device> Device;
        TRefCountPtr<ID3D11DeviceContext> Context;

#if OMNI_WITH_D3D11_RHI
        {
            const UINT DeviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
            const D3D_FEATURE_LEVEL FeatureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
            D3D_FEATURE_LEVEL CreatedLevel = D3D_FEATURE_LEVEL_11_0;

            HRESULT CreateDeviceResult = D3D11CreateDevice(
                nullptr,
                D3D_DRIVER_TYPE_HARDWARE,
                nullptr,
                DeviceFlags,
                FeatureLevels,
                UE_ARRAY_COUNT(FeatureLevels),
                D3D11_SDK_VERSION,
                Device.GetInitReference(),
                &CreatedLevel,
                Context.GetInitReference());

            if (FAILED(CreateDeviceResult))
            {
                UE_LOG(LogNVENCCaps, Verbose, TEXT("Temporary D3D11 device creation for NVENC caps failed (0x%08x)."), CreateDeviceResult);
                Device = nullptr;
                Context = nullptr;
            }
        }
#endif // OMNI_WITH_D3D11_RHI

#if OMNI_WITH_D3D12_RHI
        if (!Device.IsValid())
        {
            const D3D_FEATURE_LEVEL FeatureLevels[] =
            {
                D3D_FEATURE_LEVEL_12_1,
                D3D_FEATURE_LEVEL_12_0,
                D3D_FEATURE_LEVEL_11_1,
                D3D_FEATURE_LEVEL_11_0
            };

            TRefCountPtr<ID3D12Device> D3D12Device;
            if (!CreateProbeD3D12Device(D3D12Device))
            {
                UE_LOG(LogNVENCCaps, Warning, TEXT("Unable to create D3D12 device for NVENC capability query."));
                return false;
            }

            D3D12_COMMAND_QUEUE_DESC QueueDesc = {};
            QueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            QueueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
            QueueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

            TRefCountPtr<ID3D12CommandQueue> CommandQueue;
            HRESULT QueueResult = D3D12Device->CreateCommandQueue(&QueueDesc, IID_PPV_ARGS(CommandQueue.GetInitReference()));
            if (FAILED(QueueResult))
            {
                UE_LOG(LogNVENCCaps, Warning, TEXT("Failed to create D3D12 command queue for NVENC capability query (0x%08x)."), QueueResult);
                return false;
            }

            ID3D12CommandQueue* CommandQueues[] = { CommandQueue.GetReference() };

            HRESULT BridgeResult = D3D11On12CreateDevice(
                D3D12Device.GetReference(),
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                FeatureLevels,
                UE_ARRAY_COUNT(FeatureLevels),
                reinterpret_cast<IUnknown**>(CommandQueues),
                UE_ARRAY_COUNT(CommandQueues),
                0,
                Device.GetInitReference(),
                Context.GetInitReference(),
                nullptr);

            if (FAILED(BridgeResult))
            {
                UE_LOG(LogNVENCCaps, Warning, TEXT("D3D11On12CreateDevice failed during NVENC capability query (0x%08x)."), BridgeResult);
                return false;
            }
        }
#endif // OMNI_WITH_D3D12_RHI

        if (!Device.IsValid())
        {
            UE_LOG(LogNVENCCaps, Warning, TEXT("Unable to create a DirectX device for NVENC capability query."));
            return false;
        }

        OmniNVENC::FNVENCSession Session;
        if (!Session.Open(Codec, Device.GetReference(), NV_ENC_DEVICE_TYPE_DIRECTX))
        {
            UE_LOG(LogNVENCCaps, Warning, TEXT("NVENC capability query failed – unable to open session for %s."), *FNVENCDefs::CodecToString(Codec));
            return false;
        }

        ON_SCOPE_EXIT
        {
            Session.Destroy();
            if (Context.IsValid())
            {
                Context->Flush();
            }
        };

        using TNvEncGetEncodeCaps = NVENCSTATUS(NVENCAPI*)(void*, GUID, NV_ENC_CAPS_PARAM*, int*);
        TNvEncGetEncodeCaps GetEncodeCapsFn = Session.GetFunctionList().nvEncGetEncodeCaps;
        if (!GetEncodeCapsFn)
        {
            UE_LOG(LogNVENCCaps, Warning, TEXT("NVENC runtime does not expose NvEncGetEncodeCaps."));
            return false;
        }

        auto ToWindowsGuid = [](const FGuid& InGuid)
        {
            GUID Guid;
            Guid.Data1 = static_cast<uint32>(InGuid.A);
            Guid.Data2 = static_cast<uint16>((static_cast<uint32>(InGuid.B) >> 16) & 0xFFFF);
            Guid.Data3 = static_cast<uint16>(static_cast<uint32>(InGuid.B) & 0xFFFF);

            const uint32 C = static_cast<uint32>(InGuid.C);
            const uint32 D = static_cast<uint32>(InGuid.D);

            Guid.Data4[0] = static_cast<uint8>((C >> 24) & 0xFF);
            Guid.Data4[1] = static_cast<uint8>((C >> 16) & 0xFF);
            Guid.Data4[2] = static_cast<uint8>((C >> 8) & 0xFF);
            Guid.Data4[3] = static_cast<uint8>(C & 0xFF);
            Guid.Data4[4] = static_cast<uint8>((D >> 24) & 0xFF);
            Guid.Data4[5] = static_cast<uint8>((D >> 16) & 0xFF);
            Guid.Data4[6] = static_cast<uint8>((D >> 8) & 0xFF);
            Guid.Data4[7] = static_cast<uint8>(D & 0xFF);
            return Guid;
        };

        const GUID CodecGuid = ToWindowsGuid(FNVENCDefs::CodecGuid(Codec));

        auto QueryCapability = [&](NV_ENC_CAPS Capability, int32 DefaultValue = 0) -> int32
        {
            NV_ENC_CAPS_PARAM CapsParam = {};
            CapsParam.version = FNVENCDefs::PatchStructVersion(NV_ENC_CAPS_PARAM_VER, Session.GetApiVersion());
            CapsParam.capsToQuery = Capability;

            int CapsValue = DefaultValue;
            NVENCSTATUS Status = GetEncodeCapsFn(Session.GetEncoderHandle(), CodecGuid, &CapsParam, &CapsValue);
            if (Status != NV_ENC_SUCCESS)
            {
                UE_LOG(LogNVENCCaps, Verbose, TEXT("NvEncGetEncodeCaps(%d) returned %s"), static_cast<int32>(Capability), *FNVENCDefs::StatusToString(Status));
                return DefaultValue;
            }
            return CapsValue;
        };

        OutCapabilities.bSupports10Bit = QueryCapability(NV_ENC_CAPS_SUPPORT_10BIT_ENCODE) != 0;
        OutCapabilities.bSupportsBFrames = QueryCapability(NV_ENC_CAPS_NUM_MAX_BFRAMES) > 0;
        OutCapabilities.bSupportsYUV444 = QueryCapability(NV_ENC_CAPS_SUPPORT_YUV444_ENCODE) != 0;
        OutCapabilities.bSupportsLookahead = QueryCapability(NV_ENC_CAPS_SUPPORT_LOOKAHEAD) != 0;
        OutCapabilities.bSupportsAdaptiveQuantization = QueryCapability(NV_ENC_CAPS_SUPPORT_TEMPORAL_AQ) != 0;
        OutCapabilities.MaxWidth = QueryCapability(NV_ENC_CAPS_WIDTH_MAX);
        OutCapabilities.MaxHeight = QueryCapability(NV_ENC_CAPS_HEIGHT_MAX);

        UE_LOG(LogNVENCCaps, Verbose, TEXT("Queried NVENC caps for %s: %s"), *FNVENCDefs::CodecToString(Codec), *FNVENCCaps::ToDebugString(OutCapabilities));
        return true;
#endif
    }

    FString FNVENCCaps::ToDebugString(const FNVENCCapabilities& Caps)
    {
        return FString::Printf(TEXT("10bit=%s BFrames=%s YUV444=%s Lookahead=%s AQ=%s MaxResolution=%dx%d"),
            Caps.bSupports10Bit ? TEXT("yes") : TEXT("no"),
            Caps.bSupportsBFrames ? TEXT("yes") : TEXT("no"),
            Caps.bSupportsYUV444 ? TEXT("yes") : TEXT("no"),
            Caps.bSupportsLookahead ? TEXT("yes") : TEXT("no"),
            Caps.bSupportsAdaptiveQuantization ? TEXT("yes") : TEXT("no"),
            Caps.MaxWidth,
            Caps.MaxHeight);
    }
}

#else

namespace OmniNVENC
{
    bool FNVENCCaps::Query(ENVENCCodec, FNVENCCapabilities& OutCapabilities)
    {
        OutCapabilities = FNVENCCapabilities();
        return false;
    }

    FString FNVENCCaps::ToDebugString(const FNVENCCapabilities& Caps)
    {
        return FString::Printf(TEXT("NVENC disabled (Max %dx%d)"), Caps.MaxWidth, Caps.MaxHeight);
    }
}

#endif // WITH_OMNI_NVENC

