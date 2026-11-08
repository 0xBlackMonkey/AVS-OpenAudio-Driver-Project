// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 AVS OpenAudio Driver Project contributors
//
// minwavert.cpp -- WaveRT miniport (one per endpoint) and its stream,
// render or capture (direction from the endpoint).
//
// WaveRT pull mode: the audio engine writes into (render) or reads from
// (capture) a cyclic buffer and follows our DMA position; there is no data
// copying in the driver.
//   AllocateAudioBuffer: physically contiguous pages from PortCls, mapped
//                        non-cached (HDA DMA is non-snooped here, DEVC
//                        NOSNOOP=1), handed to the C side for the BDL
//   SetState           : AvsRtStreamSetState -> host DMA + DSP paths
//   GetPosition        : HDA LPIB of our host stream
// Exactly one format per endpoint: the topology FE format (the DSP copier
// converts to whatever the codec link needs).

#include "avs_pcls.h"

// HDA DMA engine FIFO in front of the host copier; reported as latency.
#define AVS_HW_FIFO_BYTES   768

class CMiniportWaveRT;

// ----------------------------------------------------------------- stream

class CMiniportWaveRTStream : public IMiniportWaveRTStream, public CUnknown
{
public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CMiniportWaveRTStream);
    ~CMiniportWaveRTStream();

    IMP_IMiniportWaveRTStream;

    NTSTATUS Init(_In_ CMiniportWaveRT* Miniport, _In_ PPORTWAVERTSTREAM PortStream);

private:
    CMiniportWaveRT*    m_Miniport;
    PPORTWAVERTSTREAM   m_PortStream;
    PAVS_RT_STREAM      m_Rt;
    PMDL                m_Mdl;
    ULONG               m_BufferSize;
    KSSTATE             m_State;
};

// ---------------------------------------------------------------- miniport

class CMiniportWaveRT : public IMiniportWaveRT, public CUnknown
{
public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CMiniportWaveRT);
    ~CMiniportWaveRT();

    IMP_IMiniportWaveRT;

    VOID Setup(_In_ PAVS_DEVICE Dev, _In_ ULONG Endpoint);
    BOOLEAN IsFormatSupported(_In_ PKSDATAFORMAT Format);

    PAVS_DEVICE     m_Dev;
    ULONG           m_Endpoint;
    AVS_EP_INFO     m_Info;
    PPORTWAVERT     m_Port;

private:
    KSDATARANGE_AUDIO       m_PcmRange;
    PKSDATARANGE            m_PcmRanges[1];
    KSDATARANGE             m_BridgeRange;
    PKSDATARANGE            m_BridgeRanges[1];
    PCPIN_DESCRIPTOR        m_Pins[2];
    PCNODE_DESCRIPTOR       m_Nodes[1];
    PCCONNECTION_DESCRIPTOR m_Connections[2];
    GUID                    m_Categories[3];
    GUID                    m_NodeType;     // DAC (render) / ADC (capture)
    PCFILTER_DESCRIPTOR     m_Filter;
};

// Debug print of a KS data format / range header (+ the audio part if any).
static VOID
AvsLogFormat(
    _In_ ULONG Endpoint,
    _In_ PCSTR What,
    _In_opt_ PKSDATAFORMAT F
    )
{
    if (F == NULL)
    {
        AVS_LOG("AVS: Wave%lu %s: (null)\n", Endpoint, What);
        return;
    }
    AVS_LOG("AVS: Wave%lu %s: size=%lu major=%08lX sub=%08lX spec=%08lX\n", Endpoint, What,
        F->FormatSize, F->MajorFormat.Data1, F->SubFormat.Data1, F->Specifier.Data1);

    if (IsEqualGUIDAligned(F->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) &&
        F->FormatSize >= sizeof(KSDATAFORMAT_WAVEFORMATEX))
    {
        PWAVEFORMATEX w = &((PKSDATAFORMAT_WAVEFORMATEX)F)->WaveFormatEx;

        AVS_LOG("AVS: Wave%lu %s:   tag=0x%04X ch=%u rate=%lu bits=%u align=%u cb=%u valid=%u\n",
            Endpoint, What, w->wFormatTag, w->nChannels, w->nSamplesPerSec, w->wBitsPerSample,
            w->nBlockAlign, w->cbSize,
            (w->wFormatTag == WAVE_FORMAT_EXTENSIBLE && w->cbSize >= 22)
                ? ((PWAVEFORMATEXTENSIBLE)w)->Samples.wValidBitsPerSample : w->wBitsPerSample);
    }
}

NTSTATUS
AvsCreateMiniportWaveRT(
    _Out_ PUNKNOWN* Unknown,
    _In_ PAVS_DEVICE Dev,
    _In_ ULONG Endpoint
    )
{
    CMiniportWaveRT* obj = new(POOL_FLAG_NON_PAGED, AVS_PCLS_POOL_TAG) CMiniportWaveRT(NULL);

    *Unknown = NULL;
    if (obj == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    obj->Setup(Dev, Endpoint);
    *Unknown = PUNKNOWN(PMINIPORTWAVERT(obj));
    (*Unknown)->AddRef();
    return STATUS_SUCCESS;
}

CMiniportWaveRT::~CMiniportWaveRT()
{
    if (m_Port != NULL)
    {
        m_Port->Release();
        m_Port = NULL;
    }
}

// Builds this endpoint's filter description (pins, DAC node, data ranges).
VOID
CMiniportWaveRT::Setup(
    _In_ PAVS_DEVICE Dev,
    _In_ ULONG Endpoint
    )
{
    m_Dev = Dev;
    m_Endpoint = Endpoint;
    AvsDeviceGetEndpoint(Dev, Endpoint, &m_Info);

    // PCM, exactly the FE format
    RtlZeroMemory(&m_PcmRange, sizeof(m_PcmRange));
    m_PcmRange.DataRange.FormatSize = sizeof(KSDATARANGE_AUDIO);
    m_PcmRange.DataRange.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    m_PcmRange.DataRange.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    m_PcmRange.DataRange.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
    m_PcmRange.MaximumChannels = m_Info.Channels;
    m_PcmRange.MinimumBitsPerSample = m_Info.ContainerBits;
    m_PcmRange.MaximumBitsPerSample = m_Info.ContainerBits;
    m_PcmRange.MinimumSampleFrequency = m_Info.Rate;
    m_PcmRange.MaximumSampleFrequency = m_Info.Rate;
    m_PcmRanges[0] = PKSDATARANGE(&m_PcmRange);

    // analog bridge to the topology filter
    RtlZeroMemory(&m_BridgeRange, sizeof(m_BridgeRange));
    m_BridgeRange.FormatSize = sizeof(KSDATARANGE);
    m_BridgeRange.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    m_BridgeRange.SubFormat = KSDATAFORMAT_SUBTYPE_ANALOG;
    m_BridgeRange.Specifier = KSDATAFORMAT_SPECIFIER_NONE;
    m_BridgeRanges[0] = &m_BridgeRange;

    RtlZeroMemory(m_Pins, sizeof(m_Pins));
    // pin 0: PCM pin, one instance -- render sink (engine writes into us)
    // or capture source (engine reads from us)
    m_Pins[AVS_WAVE_PIN_STREAM].MaxGlobalInstanceCount = 1;
    m_Pins[AVS_WAVE_PIN_STREAM].MaxFilterInstanceCount = 1;
    m_Pins[AVS_WAVE_PIN_STREAM].KsPinDescriptor.DataRangesCount = 1;
    m_Pins[AVS_WAVE_PIN_STREAM].KsPinDescriptor.DataRanges = m_PcmRanges;
    m_Pins[AVS_WAVE_PIN_STREAM].KsPinDescriptor.DataFlow =
        m_Info.Capture ? KSPIN_DATAFLOW_OUT : KSPIN_DATAFLOW_IN;
    m_Pins[AVS_WAVE_PIN_STREAM].KsPinDescriptor.Communication = KSPIN_COMMUNICATION_SINK;
    m_Pins[AVS_WAVE_PIN_STREAM].KsPinDescriptor.Category = &KSCATEGORY_AUDIO;
    // pin 1: analog bridge to/from the topology filter
    m_Pins[AVS_WAVE_PIN_BRIDGE].KsPinDescriptor.DataRangesCount = 1;
    m_Pins[AVS_WAVE_PIN_BRIDGE].KsPinDescriptor.DataRanges = m_BridgeRanges;
    m_Pins[AVS_WAVE_PIN_BRIDGE].KsPinDescriptor.DataFlow =
        m_Info.Capture ? KSPIN_DATAFLOW_IN : KSPIN_DATAFLOW_OUT;
    m_Pins[AVS_WAVE_PIN_BRIDGE].KsPinDescriptor.Communication = KSPIN_COMMUNICATION_NONE;
    m_Pins[AVS_WAVE_PIN_BRIDGE].KsPinDescriptor.Category = &KSCATEGORY_AUDIO;

    m_NodeType = m_Info.Capture ? KSNODETYPE_ADC : KSNODETYPE_DAC;
    RtlZeroMemory(m_Nodes, sizeof(m_Nodes));
    m_Nodes[0].Type = &m_NodeType;

    // render:  stream pin -> DAC -> bridge pin
    // capture: bridge pin -> ADC -> stream pin
    m_Connections[0].FromNode = PCFILTER_NODE;
    m_Connections[0].FromNodePin = m_Info.Capture ? AVS_WAVE_PIN_BRIDGE : AVS_WAVE_PIN_STREAM;
    m_Connections[0].ToNode = 0;
    m_Connections[0].ToNodePin = KSNODEPIN_STANDARD_IN;
    m_Connections[1].FromNode = 0;
    m_Connections[1].FromNodePin = KSNODEPIN_STANDARD_OUT;
    m_Connections[1].ToNode = PCFILTER_NODE;
    m_Connections[1].ToNodePin = m_Info.Capture ? AVS_WAVE_PIN_STREAM : AVS_WAVE_PIN_BRIDGE;

    m_Categories[0] = KSCATEGORY_AUDIO;
    m_Categories[1] = m_Info.Capture ? KSCATEGORY_CAPTURE : KSCATEGORY_RENDER;
    m_Categories[2] = KSCATEGORY_REALTIME;

    RtlZeroMemory(&m_Filter, sizeof(m_Filter));
    m_Filter.Version = 0;
    m_Filter.PinSize = sizeof(PCPIN_DESCRIPTOR);
    m_Filter.PinCount = ARRAYSIZE(m_Pins);
    m_Filter.Pins = m_Pins;
    m_Filter.NodeSize = sizeof(PCNODE_DESCRIPTOR);
    m_Filter.NodeCount = ARRAYSIZE(m_Nodes);
    m_Filter.Nodes = m_Nodes;
    m_Filter.ConnectionCount = ARRAYSIZE(m_Connections);
    m_Filter.Connections = m_Connections;
    m_Filter.CategoryCount = ARRAYSIZE(m_Categories);
    m_Filter.Categories = m_Categories;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveRT::NonDelegatingQueryInterface(
    _In_ REFIID Interface,
    _COM_Outptr_ PVOID* Object
    )
{
    if (IsEqualGUIDAligned(Interface, IID_IUnknown))
    {
        *Object = PVOID(PUNKNOWN(PMINIPORTWAVERT(this)));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniport))
    {
        *Object = PVOID(PMINIPORT(this));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveRT))
    {
        *Object = PVOID(PMINIPORTWAVERT(this));
    }
    else
    {
        *Object = NULL;
        return STATUS_INVALID_PARAMETER;
    }
    PUNKNOWN(*Object)->AddRef();
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveRT::Init(
    _In_ PUNKNOWN UnknownAdapter,
    _In_ PRESOURCELIST ResourceList,
    _In_ PPORTWAVERT Port
    )
{
    UNREFERENCED_PARAMETER(UnknownAdapter);
    UNREFERENCED_PARAMETER(ResourceList);

    m_Port = Port;
    m_Port->AddRef();
    AVS_LOG("AVS: Wave%lu Init (FE \"%s\" %lu Hz %lu/%lu bit %lu ch)\n", m_Endpoint,
        m_Info.FeName, m_Info.Rate, m_Info.ContainerBits, m_Info.ValidBits, m_Info.Channels);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveRT::GetDescription(
    _Out_ PPCFILTER_DESCRIPTOR* Description
    )
{
    AVS_LOG("AVS: Wave%lu GetDescription\n", m_Endpoint);
    *Description = &m_Filter;
    return STATUS_SUCCESS;
}

// Our own intersection for the PCM pin: the one FE format, returned as
// WAVEFORMATEXTENSIBLE (what the Windows audio engine works with), if it
// fits inside the client's range. The bridge pin is left to PortCls.
STDMETHODIMP_(NTSTATUS)
CMiniportWaveRT::DataRangeIntersection(
    _In_ ULONG PinId,
    _In_ PKSDATARANGE DataRange,
    _In_ PKSDATARANGE MatchingDataRange,
    _In_ ULONG OutputBufferLength,
    _Out_writes_bytes_to_opt_(OutputBufferLength, *ResultantFormatLength) PVOID ResultantFormat,
    _Out_ PULONG ResultantFormatLength
    )
{
    const ULONG need = sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEXTENSIBLE);
    PKSDATAFORMAT out;
    PWAVEFORMATEXTENSIBLE wfx;

    UNREFERENCED_PARAMETER(MatchingDataRange);

    AVS_LOG("AVS: Wave%lu DataRangeIntersection pin %lu, out buffer %lu\n",
        m_Endpoint, PinId, OutputBufferLength);
    AvsLogFormat(m_Endpoint, "  client range", DataRange);

    if (PinId != AVS_WAVE_PIN_STREAM)
    {
        return STATUS_NOT_IMPLEMENTED;
    }

    if (!IsEqualGUIDAligned(DataRange->MajorFormat, KSDATAFORMAT_TYPE_AUDIO) &&
        !IsEqualGUIDAligned(DataRange->MajorFormat, KSDATAFORMAT_TYPE_WILDCARD))
    {
        return STATUS_NO_MATCH;
    }
    if (!IsEqualGUIDAligned(DataRange->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) &&
        !IsEqualGUIDAligned(DataRange->SubFormat, KSDATAFORMAT_SUBTYPE_WILDCARD))
    {
        return STATUS_NO_MATCH;
    }
    if (IsEqualGUIDAligned(DataRange->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX))
    {
        // a real audio range: our format must lie inside it
        if (DataRange->FormatSize >= sizeof(KSDATARANGE_AUDIO))
        {
            PKSDATARANGE_AUDIO r = (PKSDATARANGE_AUDIO)DataRange;

            if (r->MaximumChannels < m_Info.Channels ||
                r->MinimumBitsPerSample > m_Info.ContainerBits ||
                r->MaximumBitsPerSample < m_Info.ContainerBits ||
                r->MinimumSampleFrequency > m_Info.Rate ||
                r->MaximumSampleFrequency < m_Info.Rate)
            {
                AVS_LOG("AVS: Wave%lu   -> NO_MATCH\n", m_Endpoint);
                return STATUS_NO_MATCH;
            }
        }
    }
    else if (!IsEqualGUIDAligned(DataRange->Specifier, KSDATAFORMAT_SPECIFIER_WILDCARD))
    {
        return STATUS_NO_MATCH;
    }

    // size query
    if (OutputBufferLength == 0)
    {
        *ResultantFormatLength = need;
        return STATUS_BUFFER_OVERFLOW;
    }
    if (OutputBufferLength < need || ResultantFormat == NULL)
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    out = (PKSDATAFORMAT)ResultantFormat;
    wfx = (PWAVEFORMATEXTENSIBLE)(out + 1);
    RtlZeroMemory(out, need);

    wfx->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wfx->Format.nChannels = (WORD)m_Info.Channels;
    wfx->Format.nSamplesPerSec = m_Info.Rate;
    wfx->Format.wBitsPerSample = (WORD)m_Info.ContainerBits;
    wfx->Format.nBlockAlign = (WORD)(m_Info.Channels * m_Info.ContainerBits / 8);
    wfx->Format.nAvgBytesPerSec = wfx->Format.nBlockAlign * m_Info.Rate;
    wfx->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wfx->Samples.wValidBitsPerSample = (WORD)m_Info.ValidBits;
    wfx->dwChannelMask = (m_Info.Channels == 1) ? KSAUDIO_SPEAKER_MONO : KSAUDIO_SPEAKER_STEREO;
    wfx->SubFormat = KSDATAFORMAT_SUBTYPE_PCM;

    out->FormatSize = need;
    out->Flags = 0;
    out->SampleSize = wfx->Format.nBlockAlign;
    out->Reserved = 0;
    out->MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    out->SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    out->Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;

    *ResultantFormatLength = need;
    AVS_LOG("AVS: Wave%lu   -> %lu Hz %lu/%lu bit %lu ch (WAVEFORMATEXTENSIBLE)\n", m_Endpoint,
        m_Info.Rate, m_Info.ContainerBits, m_Info.ValidBits, m_Info.Channels);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveRT::GetDeviceDescription(
    _Out_ PDEVICE_DESCRIPTION DeviceDescription
    )
{
    RtlZeroMemory(DeviceDescription, sizeof(DEVICE_DESCRIPTION));
    DeviceDescription->Version = DEVICE_DESCRIPTION_VERSION;
    DeviceDescription->Master = TRUE;
    DeviceDescription->ScatterGather = FALSE;
    DeviceDescription->Dma32BitAddresses = TRUE;
    DeviceDescription->InterfaceType = PCIBus;
    DeviceDescription->MaximumLength = 0xFFFFFFFF;
    return STATUS_SUCCESS;
}

// Exactly the FE format: rate, channels, container bits. Valid bits may
// equal the FE's valid bits or the container (the copier ignores the pad).
BOOLEAN
CMiniportWaveRT::IsFormatSupported(
    _In_ PKSDATAFORMAT Format
    )
{
    PWAVEFORMATEX wfx;
    ULONG validBits;

    if (Format == NULL || Format->FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEX) ||
        !IsEqualGUIDAligned(Format->MajorFormat, KSDATAFORMAT_TYPE_AUDIO) ||
        !IsEqualGUIDAligned(Format->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX))
    {
        return FALSE;
    }
    wfx = &((PKSDATAFORMAT_WAVEFORMATEX)Format)->WaveFormatEx;

    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
    {
        PWAVEFORMATEXTENSIBLE ext = (PWAVEFORMATEXTENSIBLE)wfx;

        if (Format->FormatSize < sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEXTENSIBLE) ||
            !IsEqualGUIDAligned(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM))
        {
            return FALSE;
        }
        validBits = ext->Samples.wValidBitsPerSample;
    }
    else if (wfx->wFormatTag == WAVE_FORMAT_PCM)
    {
        validBits = wfx->wBitsPerSample;
    }
    else
    {
        return FALSE;
    }

    return wfx->nChannels == m_Info.Channels &&
           wfx->nSamplesPerSec == m_Info.Rate &&
           wfx->wBitsPerSample == m_Info.ContainerBits &&
           (validBits == m_Info.ValidBits || validBits == m_Info.ContainerBits);
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveRT::NewStream(
    _Out_ PMINIPORTWAVERTSTREAM* Stream,
    _In_ PPORTWAVERTSTREAM PortStream,
    _In_ ULONG Pin,
    _In_ BOOLEAN Capture,
    _In_ PKSDATAFORMAT DataFormat
    )
{
    CMiniportWaveRTStream* stream;
    NTSTATUS status;

    *Stream = NULL;

    AVS_LOG("AVS: Wave%lu NewStream pin %lu capture %u\n", m_Endpoint, Pin, Capture);
    AvsLogFormat(m_Endpoint, "  format", DataFormat);

    if (Capture != m_Info.Capture || Pin != AVS_WAVE_PIN_STREAM)
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (!IsFormatSupported(DataFormat))
    {
        AVS_LOG("AVS: Wave%lu   -> format refused\n", m_Endpoint);
        return STATUS_NO_MATCH;
    }

    stream = new(POOL_FLAG_NON_PAGED, AVS_PCLS_POOL_TAG) CMiniportWaveRTStream(NULL);
    if (stream == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    stream->AddRef();

    status = stream->Init(this, PortStream);
    if (!NT_SUCCESS(status))
    {
        stream->Release();
        return status;
    }

    *Stream = PMINIPORTWAVERTSTREAM(stream);    // our reference goes to PortCls
    return STATUS_SUCCESS;
}

// ------------------------------------------------------------ stream impl

NTSTATUS
CMiniportWaveRTStream::Init(
    _In_ CMiniportWaveRT* Miniport,
    _In_ PPORTWAVERTSTREAM PortStream
    )
{
    NTSTATUS status;

    m_Miniport = Miniport;
    m_Miniport->AddRef();
    m_PortStream = PortStream;
    m_PortStream->AddRef();
    m_State = KSSTATE_STOP;

    status = AvsRtStreamCreate(Miniport->m_Dev, Miniport->m_Endpoint, &m_Rt);
    if (!NT_SUCCESS(status))
    {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "AVS: Wave%lu NewStream -- AvsRtStreamCreate failed: 0x%X\n",
            Miniport->m_Endpoint, status);
        m_Rt = NULL;
    }
    return status;
}

CMiniportWaveRTStream::~CMiniportWaveRTStream()
{
    AVS_LOG("AVS: Wave%lu stream closed\n", m_Miniport ? m_Miniport->m_Endpoint : 99);
    if (m_Rt != NULL)
    {
        AvsRtStreamDestroy(m_Rt);   // stops DMA + DSP paths if still running
        m_Rt = NULL;
    }
    if (m_Mdl != NULL && m_PortStream != NULL)
    {
        m_PortStream->FreePagesFromMdl(m_Mdl);
        m_Mdl = NULL;
    }
    if (m_PortStream != NULL)
    {
        m_PortStream->Release();
        m_PortStream = NULL;
    }
    if (m_Miniport != NULL)
    {
        m_Miniport->Release();
        m_Miniport = NULL;
    }
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::NonDelegatingQueryInterface(
    _In_ REFIID Interface,
    _COM_Outptr_ PVOID* Object
    )
{
    if (IsEqualGUIDAligned(Interface, IID_IUnknown))
    {
        *Object = PVOID(PUNKNOWN(PMINIPORTWAVERTSTREAM(this)));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveRTStream))
    {
        *Object = PVOID(PMINIPORTWAVERTSTREAM(this));
    }
    else
    {
        *Object = NULL;
        return STATUS_INVALID_PARAMETER;
    }
    PUNKNOWN(*Object)->AddRef();
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::AllocateAudioBuffer(
    _In_ ULONG RequestedSize,
    _Out_ PMDL* AudioBufferMdl,
    _Out_ ULONG* ActualSize,
    _Out_ ULONG* OffsetFromFirstPage,
    _Out_ MEMORY_CACHING_TYPE* CacheType
    )
{
    PHYSICAL_ADDRESS low, high, pa;
    ULONG blockAlign, unit, size;
    PMDL mdl;
    PVOID va;

    *AudioBufferMdl = NULL;
    *ActualSize = 0;
    *OffsetFromFirstPage = 0;
    *CacheType = MmNonCached;

    if (m_Mdl != NULL)
    {
        return STATUS_DEVICE_BUSY;
    }

    // Whole pages (HDA: 128-byte aligned fragments, sizes multiple of 128,
    // any HostStreamFrags up to 32), and whole frames.
    blockAlign = (m_Miniport->m_Info.ContainerBits / 8) * m_Miniport->m_Info.Channels;
    if (blockAlign == 0)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }
    unit = PAGE_SIZE;
    while ((unit % blockAlign) != 0)
    {
        unit += PAGE_SIZE;
    }
    if (RequestedSize < unit)
    {
        RequestedSize = unit;
    }
    if (RequestedSize > 4 * 1024 * 1024)
    {
        RequestedSize = 4 * 1024 * 1024;
    }
    size = ((RequestedSize + unit - 1) / unit) * unit;

    // Below 4 GB, as the proven test-tone buffer was.
    low.QuadPart = 0;
    high.QuadPart = 0xFFFFFFFFULL;
    mdl = m_PortStream->AllocateContiguousPagesForMdl(low, high, size);
    if (mdl == NULL)
    {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "AVS: Wave%lu -- AllocateContiguousPagesForMdl(%lu) failed\n",
            m_Miniport->m_Endpoint, size);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    pa = m_PortStream->GetPhysicalPageAddress(mdl, 0);

    // Start from silence. Same (non-cached) attribute as every other
    // mapping of these pages.
    va = m_PortStream->MapAllocatedPages(mdl, MmNonCached);
    if (va != NULL)
    {
        RtlZeroMemory(va, size);
        m_PortStream->UnmapAllocatedPages(va, mdl);
    }

    AvsRtStreamSetBuffer(m_Rt, pa, size);
    m_Mdl = mdl;
    m_BufferSize = size;

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "AVS: Wave%lu -- audio buffer %lu bytes (requested %lu) pa=0x%llX, non-cached\n",
        m_Miniport->m_Endpoint, size, RequestedSize, pa.QuadPart);

    *AudioBufferMdl = mdl;
    *ActualSize = size;
    *OffsetFromFirstPage = 0;
    *CacheType = MmNonCached;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(VOID)
CMiniportWaveRTStream::FreeAudioBuffer(
    _In_opt_ PMDL AudioBufferMdl,
    _In_ ULONG BufferSize
    )
{
    PHYSICAL_ADDRESS none;

    UNREFERENCED_PARAMETER(BufferSize);

    none.QuadPart = 0;
    AvsRtStreamSetBuffer(m_Rt, none, 0);    // DMA off this buffer first

    if (AudioBufferMdl != NULL)
    {
        m_PortStream->FreePagesFromMdl(AudioBufferMdl);
    }
    if (AudioBufferMdl == m_Mdl)
    {
        m_Mdl = NULL;
        m_BufferSize = 0;
    }
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::SetFormat(
    _In_ PKSDATAFORMAT DataFormat
    )
{
    // Only one format exists; anything else is refused.
    AvsLogFormat(m_Miniport->m_Endpoint, "SetFormat", DataFormat);
    return m_Miniport->IsFormatSupported(DataFormat) ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::SetState(
    _In_ KSSTATE State
    )
{
    NTSTATUS status = AvsRtStreamSetState(m_Rt, (ULONG)State);

    AVS_LOG("AVS: Wave%lu SetState %d -> %d: 0x%X\n", m_Miniport->m_Endpoint,
        (int)m_State, (int)State, status);
    if (NT_SUCCESS(status))
    {
        m_State = State;
    }
    return status;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::GetPosition(
    _Out_ PKSAUDIO_POSITION Position
    )
{
    ULONG pos = AvsRtStreamGetPosition(m_Rt);

    Position->PlayOffset = pos;
    Position->WriteOffset = pos;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(VOID)
CMiniportWaveRTStream::GetHWLatency(
    _Out_ KSRTAUDIO_HWLATENCY* hwLatency
    )
{
    hwLatency->FifoSize = AVS_HW_FIFO_BYTES;
    hwLatency->ChipsetDelay = 0;
    hwLatency->CodecDelay = 0;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::GetPositionRegister(
    _Out_ KSRTAUDIO_HWREGISTER* Register
    )
{
    UNREFERENCED_PARAMETER(Register);
    return STATUS_NOT_SUPPORTED;    // the engine polls GetPosition instead
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::GetClockRegister(
    _Out_ KSRTAUDIO_HWREGISTER* Register
    )
{
    UNREFERENCED_PARAMETER(Register);
    return STATUS_NOT_SUPPORTED;
}
