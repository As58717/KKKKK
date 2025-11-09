// Copyright Epic Games, Inc. All Rights Reserved.

#include "NVENC/NVENCSession.h"

#if WITH_OMNI_NVENC

#include "NVENC/NVENCCommon.h"
#include "NVENC/NVENCDefs.h"
#include "NVENC/NVENCParameters.h"
#include "NVENC/NVEncodeAPILoader.h"
#include "Logging/LogMacros.h"
#include "HAL/PlatformProcess.h"

#include "Misc/ScopeExit.h"
#include "Math/UnrealMathUtility.h"

#ifndef UE_NVENC_HAS_FLUSH_FUNCTION
#if defined(NVENCAPI_MAJOR_VERSION) && NVENCAPI_MAJOR_VERSION < 12
#define UE_NVENC_HAS_FLUSH_FUNCTION 1
#else
#define UE_NVENC_HAS_FLUSH_FUNCTION 0
#endif
#endif

DEFINE_LOG_CATEGORY_STATIC(LogNVENCSession, Log, All);

namespace OmniNVENC
{
    namespace
    {
#if PLATFORM_WINDOWS
        GUID ToWindowsGuid(const FGuid& InGuid)
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
        }

        FGuid FromWindowsGuid(const GUID& InGuid)
        {
            const uint32 B = (static_cast<uint32>(InGuid.Data2) << 16) | static_cast<uint32>(InGuid.Data3);
            const uint32 C = (static_cast<uint32>(InGuid.Data4[0]) << 24)
                | (static_cast<uint32>(InGuid.Data4[1]) << 16)
                | (static_cast<uint32>(InGuid.Data4[2]) << 8)
                | static_cast<uint32>(InGuid.Data4[3]);
            const uint32 D = (static_cast<uint32>(InGuid.Data4[4]) << 24)
                | (static_cast<uint32>(InGuid.Data4[5]) << 16)
                | (static_cast<uint32>(InGuid.Data4[6]) << 8)
                | static_cast<uint32>(InGuid.Data4[7]);
            return FGuid(InGuid.Data1, B, C, D);
        }

        NV_ENC_BUFFER_FORMAT ToNVFormat(ENVENCBufferFormat Format)
        {
            switch (Format)
            {
            case ENVENCBufferFormat::P010:
                return NV_ENC_BUFFER_FORMAT_YUV420_10BIT;
            case ENVENCBufferFormat::BGRA:
                return NV_ENC_BUFFER_FORMAT_ARGB;
            case ENVENCBufferFormat::NV12:
            default:
                return NV_ENC_BUFFER_FORMAT_NV12;
            }
        }

        NV_ENC_PARAMS_RC_MODE ToNVRateControl(ENVENCRateControlMode Mode)
        {
            switch (Mode)
            {
            case ENVENCRateControlMode::CONSTQP:
                return NV_ENC_PARAMS_RC_CONSTQP;
            case ENVENCRateControlMode::VBR:
                return NV_ENC_PARAMS_RC_VBR;
            case ENVENCRateControlMode::CBR:
            default:
                return NV_ENC_PARAMS_RC_CBR;
            }
        }

        NV_ENC_MULTI_PASS ToNVMultiPass(ENVENCMultipassMode Mode)
        {
            switch (Mode)
            {
            case ENVENCMultipassMode::QUARTER:
                return NV_ENC_TWO_PASS_QUARTER_RESOLUTION;
            case ENVENCMultipassMode::FULL:
                return NV_ENC_TWO_PASS_FULL_RESOLUTION;
            case ENVENCMultipassMode::DISABLED:
            default:
                return NV_ENC_MULTI_PASS_DISABLED;
            }
        }

        template <typename TFunc>
        bool ValidateFunction(const ANSICHAR* Name, TFunc* Function)
        {
            if (!Function)
            {
                UE_LOG(LogNVENCSession, Error, TEXT("Required NVENC export '%s' is missing."), ANSI_TO_TCHAR(Name));
                return false;
            }
            return true;
        }
#endif
    }

    bool FNVENCSession::Open(ENVENCCodec Codec, void* InDevice, NV_ENC_DEVICE_TYPE InDeviceType)
    {
#if !PLATFORM_WINDOWS
        UE_LOG(LogNVENCSession, Warning, TEXT("NVENC session is only available on Windows builds."));
        return false;
#else
        if (bIsOpen)
        {
            return true;
        }

        if (!InDevice)
        {
            UE_LOG(LogNVENCSession, Error, TEXT("Failed to open NVENC session – no encoder device was provided."));
            return false;
        }

        FNVEncodeAPILoader& Loader = FNVEncodeAPILoader::Get();
        if (!Loader.Load())
        {
            UE_LOG(LogNVENCSession, Warning, TEXT("Failed to open NVENC session for codec %s – runtime is unavailable."), *FNVENCDefs::CodecToString(Codec));
            return false;
        }

        ApiVersion = NVENCAPI_VERSION;
        FNVENCAPIVersion NegotiatedVersion = FNVENCDefs::DecodeApiVersion(ApiVersion);

        using TNvEncodeAPIGetMaxSupportedVersion = NVENCSTATUS(NVENCAPI*)(uint32_t*);

        void* const NvencHandle = FNVENCCommon::GetHandle();
        if (NvencHandle)
        {
            void* const MaxVersionExport = FPlatformProcess::GetDllExport(NvencHandle, TEXT("NvEncodeAPIGetMaxSupportedVersion"));
            if (MaxVersionExport)
            {
                TNvEncodeAPIGetMaxSupportedVersion GetMaxSupportedVersion = reinterpret_cast<TNvEncodeAPIGetMaxSupportedVersion>(MaxVersionExport);
                uint32 RuntimeApiVersionRaw = 0;
                const NVENCSTATUS VersionStatus = GetMaxSupportedVersion ? GetMaxSupportedVersion(&RuntimeApiVersionRaw) : NV_ENC_ERR_INVALID_PTR;
                if (VersionStatus == NV_ENC_SUCCESS && RuntimeApiVersionRaw != 0)
                {
                    const FNVENCAPIVersion RuntimeVersion = FNVENCDefs::DecodeRuntimeVersion(RuntimeApiVersionRaw);
                    if (RuntimeVersion.Major != 0 || RuntimeVersion.Minor != 0)
                    {
                        if (FNVENCDefs::IsVersionOlder(RuntimeVersion, NegotiatedVersion))
                        {
                            UE_LOG(LogNVENCSession, Log,
                                TEXT("NVENC runtime API version %s (0x%08x) is lower than compile-time version %s (0x%08x). Downgrading."),
                                *FNVENCDefs::VersionToString(RuntimeVersion), RuntimeApiVersionRaw,
                                *FNVENCDefs::VersionToString(NegotiatedVersion), ApiVersion);
                            NegotiatedVersion = RuntimeVersion;
                            ApiVersion = FNVENCDefs::EncodeApiVersion(NegotiatedVersion);
                        }
                        else if (FNVENCDefs::IsVersionOlder(NegotiatedVersion, RuntimeVersion))
                        {
                            UE_LOG(LogNVENCSession, Verbose,
                                TEXT("NVENC runtime reports newer API version %s (0x%08x); using compile-time version %s (0x%08x)."),
                                *FNVENCDefs::VersionToString(RuntimeVersion), RuntimeApiVersionRaw,
                                *FNVENCDefs::VersionToString(NegotiatedVersion), ApiVersion);
                        }
                    }
                }
                else if (VersionStatus != NV_ENC_SUCCESS)
                {
                    UE_LOG(LogNVENCSession, Verbose, TEXT("NvEncodeAPIGetMaxSupportedVersion failed: %s"), *FNVENCDefs::StatusToString(VersionStatus));
                }
            }
            else
            {
                UE_LOG(LogNVENCSession, Verbose, TEXT("NVENC runtime does not export NvEncodeAPIGetMaxSupportedVersion."));
            }
        }

        const FNVENCAPIVersion MinimumSupportedVersion = FNVENCDefs::GetMinimumAPIVersion();
        if (FNVENCDefs::IsVersionOlder(NegotiatedVersion, MinimumSupportedVersion))
        {
            UE_LOG(LogNVENCSession, Error,
                TEXT("NVENC runtime API version %s (0x%08x) is below the minimum supported version %s (0x%08x)."),
                *FNVENCDefs::VersionToString(NegotiatedVersion), FNVENCDefs::EncodeApiVersion(NegotiatedVersion),
                *FNVENCDefs::VersionToString(MinimumSupportedVersion), FNVENCDefs::EncodeApiVersion(MinimumSupportedVersion));
            return false;
        }

        using TNvEncodeAPICreateInstance = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);

        TNvEncodeAPICreateInstance CreateInstance = reinterpret_cast<TNvEncodeAPICreateInstance>(Loader.GetFunctions().NvEncodeAPICreateInstance);

        if (!ValidateFunction("NvEncodeAPICreateInstance", CreateInstance))
        {
            return false;
        }

        FMemory::Memzero(FunctionList);
        FunctionList.version = FNVENCDefs::PatchStructVersion(NV_ENCODE_API_FUNCTION_LIST_VER, ApiVersion);

        NVENCSTATUS Status = CreateInstance(&FunctionList);
        if (Status != NV_ENC_SUCCESS)
        {
            UE_LOG(LogNVENCSession, Error, TEXT("NvEncodeAPICreateInstance failed: %s"), *FNVENCDefs::StatusToString(Status));
            return false;
        }

        using TNvEncOpenEncodeSessionEx = NVENCSTATUS(NVENCAPI*)(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS*, void**);
        TNvEncOpenEncodeSessionEx OpenSession = reinterpret_cast<TNvEncOpenEncodeSessionEx>(FunctionList.nvEncOpenEncodeSessionEx);

        if (!ValidateFunction("NvEncOpenEncodeSessionEx", OpenSession))
        {
            return false;
        }

        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS OpenParams = {};
        OpenParams.version = FNVENCDefs::PatchStructVersion(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER, ApiVersion);
        OpenParams.apiVersion = ApiVersion;
        OpenParams.device = InDevice;
        OpenParams.deviceType = InDeviceType;

        Status = OpenSession(&OpenParams, &Encoder);
        if (Status != NV_ENC_SUCCESS)
        {
            UE_LOG(LogNVENCSession, Error, TEXT("NvEncOpenEncodeSessionEx failed: %s"), *FNVENCDefs::StatusToString(Status));
            Encoder = nullptr;
            return false;
        }

        Device = InDevice;
        DeviceType = InDeviceType;
        CurrentParameters.Codec = Codec;
        bIsOpen = true;
        return true;
#endif
    }

    bool FNVENCSession::Initialize(const FNVENCParameters& Parameters)
    {
#if !PLATFORM_WINDOWS
        UE_LOG(LogNVENCSession, Warning, TEXT("Cannot initialise NVENC session on this platform."));
        return false;
#else
        if (!bIsOpen || !Encoder)
        {
            UE_LOG(LogNVENCSession, Warning, TEXT("Cannot initialise NVENC session – encoder is not open."));
            return false;
        }

        using TNvEncGetEncodePresetConfig = NVENCSTATUS(NVENCAPI*)(void*, GUID, GUID, NV_ENC_PRESET_CONFIG*);
        using TNvEncGetEncodePresetConfigEx = NVENCSTATUS(NVENCAPI*)(void*, GUID, GUID, NV_ENC_TUNING_INFO, NV_ENC_PRESET_CONFIG*);
        using TNvEncInitializeEncoder = NVENCSTATUS(NVENCAPI*)(void*, NV_ENC_INITIALIZE_PARAMS*);
        using TNvEncGetEncodePresetGUIDs = NVENCSTATUS(NVENCAPI*)(void*, GUID, GUID*, uint32, uint32*);

        TNvEncGetEncodePresetConfig GetPresetConfig = FunctionList.nvEncGetEncodePresetConfig;
        TNvEncGetEncodePresetConfigEx GetPresetConfigEx = reinterpret_cast<TNvEncGetEncodePresetConfigEx>(FunctionList.nvEncGetEncodePresetConfigEx);
        TNvEncInitializeEncoder InitializeEncoder = FunctionList.nvEncInitializeEncoder;
        TNvEncGetEncodePresetGUIDs GetPresetGUIDs = reinterpret_cast<TNvEncGetEncodePresetGUIDs>(FunctionList.nvEncGetEncodePresetGUIDs);
        NVENCSTATUS Status = NV_ENC_SUCCESS;

        if (!ValidateFunction("NvEncGetEncodePresetConfig", GetPresetConfig) || !ValidateFunction("NvEncInitializeEncoder", InitializeEncoder))
        {
            return false;
        }

        GUID CodecGuid = ToWindowsGuid(FNVENCDefs::CodecGuid(Parameters.Codec));
        struct FPresetCandidate
        {
            GUID Guid;
            NV_ENC_TUNING_INFO Tuning = NV_ENC_TUNING_INFO_HIGH_QUALITY;
            FString Description;
        };

        TArray<FPresetCandidate> PresetCandidates;
        PresetCandidates.Reserve(12);

        auto AddCandidate = [&PresetCandidates](const GUID& InGuid, NV_ENC_TUNING_INFO Tuning, const FString& Description)
        {
            for (const FPresetCandidate& Existing : PresetCandidates)
            {
                if (FMemory::Memcmp(&Existing.Guid, &InGuid, sizeof(GUID)) == 0)
                {
                    return;
                }
            }

            FPresetCandidate Candidate;
            Candidate.Guid = InGuid;
            Candidate.Tuning = Tuning;
            Candidate.Description = Description;
            PresetCandidates.Add(Candidate);
        };

        AddCandidate(ToWindowsGuid(FNVENCDefs::PresetLowLatencyGuid()), NV_ENC_TUNING_INFO_LOW_LATENCY, TEXT("NV_ENC_PRESET_LOW_LATENCY_HQ"));
        AddCandidate(ToWindowsGuid(FNVENCDefs::PresetDefaultGuid()), NV_ENC_TUNING_INFO_HIGH_QUALITY, TEXT("NV_ENC_PRESET_DEFAULT"));
        AddCandidate(ToWindowsGuid(FNVENCDefs::PresetP1Guid()), NV_ENC_TUNING_INFO_LOW_LATENCY, TEXT("NV_ENC_PRESET_P1"));
        AddCandidate(ToWindowsGuid(FNVENCDefs::PresetP2Guid()), NV_ENC_TUNING_INFO_LOW_LATENCY, TEXT("NV_ENC_PRESET_P2"));
        AddCandidate(ToWindowsGuid(FNVENCDefs::PresetP3Guid()), NV_ENC_TUNING_INFO_HIGH_QUALITY, TEXT("NV_ENC_PRESET_P3"));
        AddCandidate(ToWindowsGuid(FNVENCDefs::PresetP4Guid()), NV_ENC_TUNING_INFO_HIGH_QUALITY, TEXT("NV_ENC_PRESET_P4"));
        AddCandidate(ToWindowsGuid(FNVENCDefs::PresetP5Guid()), NV_ENC_TUNING_INFO_HIGH_QUALITY, TEXT("NV_ENC_PRESET_P5"));
        AddCandidate(ToWindowsGuid(FNVENCDefs::PresetP6Guid()), NV_ENC_TUNING_INFO_HIGH_QUALITY, TEXT("NV_ENC_PRESET_P6"));
        AddCandidate(ToWindowsGuid(FNVENCDefs::PresetP7Guid()), NV_ENC_TUNING_INFO_LOSSLESS, TEXT("NV_ENC_PRESET_P7"));

        if (GetPresetGUIDs)
        {
            uint32 AvailablePresetCount = 0;
            NVENCSTATUS EnumStatus = GetPresetGUIDs(Encoder, CodecGuid, nullptr, 0, &AvailablePresetCount);
            if (EnumStatus == NV_ENC_SUCCESS && AvailablePresetCount > 0)
            {
                TArray<GUID> RuntimePresets;
                RuntimePresets.SetNumZeroed(AvailablePresetCount);

                EnumStatus = GetPresetGUIDs(Encoder, CodecGuid, RuntimePresets.GetData(), AvailablePresetCount, &AvailablePresetCount);
                if (EnumStatus == NV_ENC_SUCCESS)
                {
                    RuntimePresets.SetNum(AvailablePresetCount);
                    for (const GUID& RuntimeGuid : RuntimePresets)
                    {
                        const FString FriendlyName = FNVENCDefs::PresetGuidToString(FromWindowsGuid(RuntimeGuid));
                        AddCandidate(RuntimeGuid, NV_ENC_TUNING_INFO_HIGH_QUALITY, FriendlyName);
                    }
                }
            }
        }

        NV_ENC_PRESET_CONFIG PresetConfig = {};
        int32 SelectedPresetIndex = INDEX_NONE;
        NVENCSTATUS LastPresetStatus = NV_ENC_SUCCESS;

        for (int32 CandidateIndex = 0; CandidateIndex < PresetCandidates.Num(); ++CandidateIndex)
        {
            NV_ENC_PRESET_CONFIG AttemptConfig = {};
            AttemptConfig.version = FNVENCDefs::PatchStructVersion(NV_ENC_PRESET_CONFIG_VER, ApiVersion);
            AttemptConfig.presetCfg.version = FNVENCDefs::PatchStructVersion(NV_ENC_CONFIG_VER, ApiVersion);

            LastPresetStatus = GetPresetConfig(Encoder, CodecGuid, PresetCandidates[CandidateIndex].Guid, &AttemptConfig);

            if (LastPresetStatus != NV_ENC_SUCCESS && GetPresetConfigEx)
            {
                LastPresetStatus = GetPresetConfigEx(Encoder, CodecGuid, PresetCandidates[CandidateIndex].Guid, PresetCandidates[CandidateIndex].Tuning, &AttemptConfig);
            }
            if (LastPresetStatus == NV_ENC_SUCCESS)
            {
                PresetConfig = AttemptConfig;
                SelectedPresetIndex = CandidateIndex;
                break;
            }

            const FString PresetName = PresetCandidates[CandidateIndex].Description.IsEmpty()
                ? FNVENCDefs::PresetGuidToString(FromWindowsGuid(PresetCandidates[CandidateIndex].Guid))
                : PresetCandidates[CandidateIndex].Description;
            UE_LOG(LogNVENCSession, Warning, TEXT("NvEncGetEncodePresetConfig failed for %s preset: %s"), *PresetName, *FNVENCDefs::StatusToString(LastPresetStatus));
        }

        if (SelectedPresetIndex == INDEX_NONE)
        {
            UE_LOG(LogNVENCSession, Error, TEXT("NvEncGetEncodePresetConfig failed for all attempted presets: %s"), *FNVENCDefs::StatusToString(LastPresetStatus));
            return false;
        }

        const FPresetCandidate& SelectedPreset = PresetCandidates[SelectedPresetIndex];
        const FString SelectedPresetName = SelectedPreset.Description.IsEmpty()
            ? FNVENCDefs::PresetGuidToString(FromWindowsGuid(SelectedPreset.Guid))
            : SelectedPreset.Description;

        if (SelectedPresetIndex > 0)
        {
            UE_LOG(LogNVENCSession, Log, TEXT("Using fallback NVENC preset %s after trying %d options."), *SelectedPresetName, SelectedPresetIndex + 1);
        }

        EncodeConfig = PresetConfig.presetCfg;
        EncodeConfig.version = FNVENCDefs::PatchStructVersion(NV_ENC_CONFIG_VER, ApiVersion);
        EncodeConfig.rcParams.rateControlMode = ToNVRateControl(Parameters.RateControlMode);
        EncodeConfig.rcParams.averageBitRate = Parameters.TargetBitrate;
        EncodeConfig.rcParams.maxBitRate = Parameters.MaxBitrate;
        EncodeConfig.rcParams.enableLookahead = Parameters.bEnableLookahead ? 1u : 0u;
        EncodeConfig.rcParams.enableAQ = Parameters.bEnableAdaptiveQuantization ? 1u : 0u;
        EncodeConfig.rcParams.enableTemporalAQ = Parameters.bEnableAdaptiveQuantization ? 1u : 0u;
        EncodeConfig.rcParams.enableInitialRCQP = (Parameters.QPMax >= 0 || Parameters.QPMin >= 0) ? 1u : 0u;
        EncodeConfig.rcParams.constQP.qpInterB = Parameters.QPMax >= 0 ? Parameters.QPMax : EncodeConfig.rcParams.constQP.qpInterB;
        EncodeConfig.rcParams.constQP.qpInterP = Parameters.QPMax >= 0 ? Parameters.QPMax : EncodeConfig.rcParams.constQP.qpInterP;
        EncodeConfig.rcParams.constQP.qpIntra = Parameters.QPMin >= 0 ? Parameters.QPMin : EncodeConfig.rcParams.constQP.qpIntra;
        EncodeConfig.rcParams.multiPass = ToNVMultiPass(Parameters.MultipassMode);
        EncodeConfig.gopLength = Parameters.GOPLength == 0 ? NVENC_INFINITE_GOPLENGTH : Parameters.GOPLength;
        EncodeConfig.frameIntervalP = 1;
        EncodeConfig.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;
        EncodeConfig.mvPrecision = NV_ENC_MV_PRECISION_QUARTER_PEL;

        if (Parameters.Codec == ENVENCCodec::H264)
        {
            EncodeConfig.profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
            EncodeConfig.encodeCodecConfig.h264Config.idrPeriod = EncodeConfig.gopLength;
        }
        else
        {
            EncodeConfig.profileGUID = NV_ENC_HEVC_PROFILE_MAIN_GUID;
            EncodeConfig.encodeCodecConfig.hevcConfig.idrPeriod = EncodeConfig.gopLength;
        }

        NvBufferFormat = ToNVFormat(Parameters.BufferFormat);

        InitializeParams = {};
        InitializeParams.version = FNVENCDefs::PatchStructVersion(NV_ENC_INITIALIZE_PARAMS_VER, ApiVersion);
        InitializeParams.encodeGUID = CodecGuid;
        InitializeParams.presetGUID = SelectedPreset.Guid;
        InitializeParams.tuningInfo = SelectedPreset.Tuning;
        InitializeParams.encodeWidth = Parameters.Width;
        InitializeParams.encodeHeight = Parameters.Height;
        InitializeParams.darWidth = Parameters.Width;
        InitializeParams.darHeight = Parameters.Height;
        InitializeParams.frameRateNum = Parameters.Framerate == 0 ? 60 : Parameters.Framerate;
        InitializeParams.frameRateDen = 1;
        InitializeParams.enablePTD = 1;
        InitializeParams.encodeConfig = &EncodeConfig;
        InitializeParams.maxEncodeWidth = Parameters.Width;
        InitializeParams.maxEncodeHeight = Parameters.Height;
        InitializeParams.bufferFormat = NvBufferFormat;
        InitializeParams.enableEncodeAsync = 0;

        Status = InitializeEncoder(Encoder, &InitializeParams);
        if (Status != NV_ENC_SUCCESS)
        {
            UE_LOG(LogNVENCSession, Error, TEXT("NvEncInitializeEncoder failed: %s"), *FNVENCDefs::StatusToString(Status));
            return false;
        }

        CurrentParameters = Parameters;
        bIsInitialised = true;
        UE_LOG(LogNVENCSession, Verbose, TEXT("NVENC session initialised: %s"), *FNVENCParameterMapper::ToDebugString(CurrentParameters));
        return true;
#endif
    }

    bool FNVENCSession::Reconfigure(const FNVENCParameters& Parameters)
    {
#if !PLATFORM_WINDOWS
        return false;
#else
        if (!bIsInitialised)
        {
            UE_LOG(LogNVENCSession, Warning, TEXT("Cannot reconfigure NVENC session – encoder has not been initialised."));
            return false;
        }

        using TNvEncReconfigureEncoder = NVENCSTATUS(NVENCAPI*)(void*, NV_ENC_RECONFIGURE_PARAMS*);

        TNvEncReconfigureEncoder ReconfigureEncoder = FunctionList.nvEncReconfigureEncoder;
        if (!ValidateFunction("NvEncReconfigureEncoder", ReconfigureEncoder))
        {
            return false;
        }

        NV_ENC_CONFIG NewConfig = EncodeConfig;
        NewConfig.rcParams.rateControlMode = ToNVRateControl(Parameters.RateControlMode);
        NewConfig.rcParams.averageBitRate = Parameters.TargetBitrate;
        NewConfig.rcParams.maxBitRate = Parameters.MaxBitrate;
        NewConfig.rcParams.enableLookahead = Parameters.bEnableLookahead ? 1u : 0u;
        NewConfig.rcParams.enableAQ = Parameters.bEnableAdaptiveQuantization ? 1u : 0u;
        NewConfig.rcParams.enableTemporalAQ = Parameters.bEnableAdaptiveQuantization ? 1u : 0u;
        NewConfig.rcParams.multiPass = ToNVMultiPass(Parameters.MultipassMode);
        NewConfig.gopLength = Parameters.GOPLength == 0 ? NVENC_INFINITE_GOPLENGTH : Parameters.GOPLength;

        NV_ENC_RECONFIGURE_PARAMS ReconfigureParams = {};
        ReconfigureParams.version = FNVENCDefs::PatchStructVersion(NV_ENC_RECONFIGURE_PARAMS_VER, ApiVersion);
        ReconfigureParams.reInitEncodeParams = InitializeParams;
        ReconfigureParams.reInitEncodeParams.version = FNVENCDefs::PatchStructVersion(NV_ENC_INITIALIZE_PARAMS_VER, ApiVersion);
        ReconfigureParams.reInitEncodeParams.encodeWidth = Parameters.Width;
        ReconfigureParams.reInitEncodeParams.encodeHeight = Parameters.Height;
        ReconfigureParams.reInitEncodeParams.darWidth = Parameters.Width;
        ReconfigureParams.reInitEncodeParams.darHeight = Parameters.Height;
        ReconfigureParams.reInitEncodeParams.encodeConfig = &NewConfig;
        ReconfigureParams.reInitEncodeParams.maxEncodeWidth = Parameters.Width;
        ReconfigureParams.reInitEncodeParams.maxEncodeHeight = Parameters.Height;
        ReconfigureParams.reInitEncodeParams.bufferFormat = NvBufferFormat;
        ReconfigureParams.forceIDR = 1;
        ReconfigureParams.resetEncoder = 1;

        NVENCSTATUS Status = ReconfigureEncoder(Encoder, &ReconfigureParams);
        if (Status != NV_ENC_SUCCESS)
        {
            UE_LOG(LogNVENCSession, Error, TEXT("NvEncReconfigureEncoder failed: %s"), *FNVENCDefs::StatusToString(Status));
            return false;
        }

        EncodeConfig = NewConfig;
        InitializeParams = ReconfigureParams.reInitEncodeParams;
        CurrentParameters = Parameters;
        UE_LOG(LogNVENCSession, Verbose, TEXT("NVENC session reconfigured: %s"), *FNVENCParameterMapper::ToDebugString(CurrentParameters));
        return true;
#endif
    }

    void FNVENCSession::Flush()
    {
#if PLATFORM_WINDOWS
        if (!bIsInitialised)
        {
            return;
        }

#if UE_NVENC_HAS_FLUSH_FUNCTION
        using TNvEncFlushEncoderQueue = NVENCSTATUS(NVENCAPI*)(void*, void*);
        TNvEncFlushEncoderQueue FlushEncoder = FunctionList.nvEncFlushEncoderQueue;
        if (FlushEncoder)
        {
            NVENCSTATUS Status = FlushEncoder(Encoder, nullptr);
            if (Status != NV_ENC_SUCCESS && Status != NV_ENC_ERR_NEED_MORE_INPUT)
            {
                UE_LOG(LogNVENCSession, Warning, TEXT("NvEncFlushEncoderQueue returned %s"), *FNVENCDefs::StatusToString(Status));
            }
        }
#endif
#endif
    }

    void FNVENCSession::Destroy()
    {
#if PLATFORM_WINDOWS
        if (!bIsOpen)
        {
            return;
        }

        using TNvEncDestroyEncoder = NVENCSTATUS(NVENCAPI*)(void*);
        TNvEncDestroyEncoder DestroyEncoder = FunctionList.nvEncDestroyEncoder;
        if (Encoder && DestroyEncoder)
        {
            NVENCSTATUS Status = DestroyEncoder(Encoder);
            if (Status != NV_ENC_SUCCESS)
            {
                UE_LOG(LogNVENCSession, Warning, TEXT("NvEncDestroyEncoder returned %s"), *FNVENCDefs::StatusToString(Status));
            }
        }

        Encoder = nullptr;
        Device = nullptr;
        bIsInitialised = false;
        bIsOpen = false;
        FunctionList = {};
#endif
        CurrentParameters = FNVENCParameters();
        ApiVersion = NVENCAPI_VERSION;
    }

    bool FNVENCSession::GetSequenceParams(TArray<uint8>& OutData)
    {
#if !PLATFORM_WINDOWS
        return false;
#else
        if (!bIsInitialised || !Encoder)
        {
            return false;
        }

        using TNvEncGetSequenceParams = NVENCSTATUS(NVENCAPI*)(void*, NV_ENC_SEQUENCE_PARAM_PAYLOAD*);
        TNvEncGetSequenceParams GetSequenceParamsFn = FunctionList.nvEncGetSequenceParams;
        if (!GetSequenceParamsFn)
        {
            UE_LOG(LogNVENCSession, Warning, TEXT("NvEncGetSequenceParams is unavailable in this NVENC runtime."));
            return false;
        }

        uint32 OutputSize = 0;
        TArray<uint8> Buffer;
        Buffer.SetNumZeroed(1024);

        NV_ENC_SEQUENCE_PARAM_PAYLOAD Payload = {};
        Payload.version = FNVENCDefs::PatchStructVersion(NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER, ApiVersion);
        Payload.inBufferSize = Buffer.Num();
        Payload.spsppsBuffer = Buffer.GetData();
        Payload.outSPSPPSPayloadSize = &OutputSize;

        NVENCSTATUS Status = GetSequenceParamsFn(Encoder, &Payload);
        if (Status != NV_ENC_SUCCESS)
        {
            UE_LOG(LogNVENCSession, Warning, TEXT("NvEncGetSequenceParams failed: %s"), *FNVENCDefs::StatusToString(Status));
            return false;
        }

        if (OutputSize == 0)
        {
            return false;
        }

        if (OutputSize > static_cast<uint32>(Buffer.Num()))
        {
            Buffer.SetNumZeroed(OutputSize);
            Payload.inBufferSize = Buffer.Num();
            Payload.spsppsBuffer = Buffer.GetData();

            Status = GetSequenceParamsFn(Encoder, &Payload);
            if (Status != NV_ENC_SUCCESS)
            {
                UE_LOG(LogNVENCSession, Warning, TEXT("NvEncGetSequenceParams failed on resized buffer: %s"), *FNVENCDefs::StatusToString(Status));
                return false;
            }

            OutputSize = FMath::Min<uint32>(OutputSize, static_cast<uint32>(Buffer.Num()));
        }

        Buffer.SetNum(OutputSize);
        OutData = MoveTemp(Buffer);
        return true;
#endif
    }
}

#endif // WITH_OMNI_NVENC

