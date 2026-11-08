// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 AVS OpenAudio Driver Project contributors
//
// avs_port_win.c -- Windows half of the bridge to the ported Linux AVS
// core (port/, see port/avs_port_bridge.h), and the C side of the WaveRT
// streams.
//
//  - avs_bridge_ipc_send(): the core's IPC requests go through our proven
//    HIPC transport (avs_ipc.c), reply payload captured in the ISR.
//  - AvsPortInit()/AvsPortShutdown(): everything after FW_READY. Which
//    codecs exist, which SSP they sit on and which FE feeds which Windows
//    endpoint is configuration (avswin.cfg); the rest comes from the
//    topology files themselves:
//
//      Tplg0=max98927-tplg.bin      (in System32\drivers\avs, like avswin.cfg)
//      Tplg0SspMask=0x1             (BIT(n) = SSPn, board wiring)
//      Tplg1=da7219-tplg.bin
//      Tplg1SspMask=0x2
//      Endpoint0Fe=ssp0p_fe         Endpoint0Type=speaker
//      Endpoint1Fe=ssp1p_fe         Endpoint1Type=headphones
//      Endpoint2Fe="DMIC FE"        Endpoint2Type=mic
//      DspVolumePercent=50          (0..100 of the topology control range)
//
//  - AvsRtStream*(): one WaveRT render stream = one HDA host stream + one
//    FE/BE path pair in the DSP, driven by KS state changes.

#include "driver.h"
#include <ntstrsafe.h>

#define AVS_PORT_MAX_TPLG_FILES  4
#define AVS_PORT_TPLG_MAX_SIZE   (256 * 1024)

// ---------------------------------------------------------------- bridge IPC

int
avs_bridge_ipc_send(
    void* host,
    unsigned int primary,
    unsigned int ext,
    const void* payload,
    unsigned int payload_size,
    unsigned int rx_expect,
    void* rx_buf,
    unsigned int* rx_size,
    unsigned int* reply_primary,
    unsigned int* reply_ext,
    unsigned int timeout_ms
    )
{
    PAVS_DEVICE_CONTEXT ctx = (PAVS_DEVICE_CONTEXT)host;
    ULONG rp = 0, re = 0, rxs = 0;

    if (ctx == NULL || payload_size > AVS_MAILBOX_SIZE)
    {
        return -22; // -EINVAL
    }

    // Upstream waits 300 ms; our DPC-based completion adds a little
    // latency, so never go below 1 s here.
    if (timeout_ms < 1000)
    {
        timeout_ms = 1000;
    }

    NTSTATUS status = AvsIpcSendRaw(ctx, primary, ext,
        (const UCHAR*)payload, payload_size, rx_expect,
        (PUCHAR)rx_buf, &rxs, timeout_ms, &rp, &re);

    if (rx_size != NULL)
    {
        *rx_size = rxs;
    }
    *reply_primary = rp;
    *reply_ext = re;

    if (status == STATUS_TIMEOUT)
    {
        return -110; // -ETIMEDOUT
    }
    return NT_SUCCESS(status) ? 0 : -5; // -EIO
}

// ------------------------------------------------------------------- helpers

static NTSTATUS
AvsPortLoadTopologies(
    _In_ PAVS_DEVICE_CONTEXT ctx,
    _Out_ PULONG LinkMaskAll
    )
{
    ULONG loaded = 0;

    *LinkMaskAll = 0;

    for (ULONG i = 0; i < AVS_PORT_MAX_TPLG_FILES; i++)
    {
        CHAR key[24];
        CHAR name[64];
        WCHAR path[160];
        PVOID buf = NULL;
        ULONG size = 0;

        RtlStringCbPrintfA(key, sizeof(key), "Tplg%lu", i);
        if (!AvsConfigReadString(key, name, sizeof(name)))
        {
            continue;
        }
        RtlStringCbPrintfA(key, sizeof(key), "Tplg%luSspMask", i);
        ULONG mask = AvsConfigReadUlong(key, 0);

        // data directory first, then the old %SystemRoot% place
        RtlStringCbPrintfW(path, sizeof(path), AVS_DATA_DIR L"%S", name);
        NTSTATUS status = AvsReadFileToBuffer(path, AVS_PORT_TPLG_MAX_SIZE, &buf, &size);
        if (status == STATUS_OBJECT_NAME_NOT_FOUND || status == STATUS_OBJECT_PATH_NOT_FOUND)
        {
            RtlStringCbPrintfW(path, sizeof(path), L"\\SystemRoot\\%S", name);
            status = AvsReadFileToBuffer(path, AVS_PORT_TPLG_MAX_SIZE, &buf, &size);
        }
        if (!NT_SUCCESS(status))
        {
            AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                "AVS: port -- cannot read %ws: 0x%X\n", path, status));
            continue;
        }

        int ret = avs_core_load_tplg(ctx->PortCore, buf, size, mask);
        if (ret != 0)
        {
            AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                "AVS: port -- topology %s (ssp mask 0x%lx) failed to load: %d\n", name, mask, ret));
            AvsFreeFileBuffer(buf);
            continue;
        }

        // Kept while the core lives: cheap, and nothing in the core then
        // depends on whether a parser keeps a pointer into the file image.
        ctx->PortTplgBuf[i] = buf;
        *LinkMaskAll |= mask;
        loaded++;

        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
            "AVS: port -- topology %s loaded (%lu bytes, ssp mask 0x%lx)\n", name, size, mask));
    }

    return loaded ? STATUS_SUCCESS : STATUS_NOT_FOUND;
}

static VOID
AvsPortFreeTopologyBuffers(
    _In_ PAVS_DEVICE_CONTEXT ctx
    )
{
    for (ULONG i = 0; i < AVS_PORT_MAX_TPLG_FILES; i++)
    {
        AvsFreeFileBuffer(ctx->PortTplgBuf[i]);
        ctx->PortTplgBuf[i] = NULL;
    }
}

// ----------------------------------------------------------- core lifecycle

// CoreLock held. DSP must have reported FW_READY.
NTSTATUS
AvsPortInit(
    _In_ PAVS_DEVICE_CONTEXT ctx
    )
{
    NTSTATUS status;
    ULONG linkMask = 0;
    int ret;

    if (ctx->PortCore != NULL)
    {
        return STATUS_SUCCESS;
    }

    avs_core_set_nhlt(ctx->Nhlt.nhlt, ctx->Nhlt.nhltSz);

    ret = avs_core_create(ctx, &ctx->PortCore);
    if (ret != 0)
    {
        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "AVS: port -- core create (FW/HW config, MODULES_INFO) failed: %d\n", ret));
        ctx->PortCore = NULL;
        return STATUS_UNSUCCESSFUL;
    }

    status = AvsPortLoadTopologies(ctx, &linkMask);
    if (!NT_SUCCESS(status))
    {
        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "AVS: port -- no topology loaded (check Tplg0=... in avswin.cfg)\n"));
        AvsPortShutdown(ctx);
        return status;
    }
    ctx->PortLinkMask = linkMask;

    // HDA multi-link power-up per SSP used by the topologies. Not verified
    // against upstream (skl may not need it); everything is tested with it
    // on. HdaLinkPowerUp=0 in avswin.cfg skips it.
    if (AvsConfigReadUlong("HdaLinkPowerUp", 1) != 0)
    {
        for (ULONG ssp = 0; ssp < 8; ssp++)
        {
            if (linkMask & (1u << ssp))
            {
                AvsHdaLinkPowerUp(ctx, ssp);
            }
        }
    }

    // DSP-side gain, fixed. The Windows volume slider is applied in
    // software by the audio engine on top of it.
    avs_core_set_control_percent(ctx->PortCore, "DSP Volume", AvsConfigReadUlong("DspVolumePercent", 50));
    // DMIC gain: the topology's range ends at 0x7FFFFFFF (presumably 1.0).
    avs_core_set_control_percent(ctx->PortCore, "DMIC Volume", AvsConfigReadUlong("MicVolumePercent", 100));
    return STATUS_SUCCESS;
}

// CoreLock held. Streams must already be closed (AvsPortSuspendStreams).
VOID
AvsPortShutdown(
    _In_ PAVS_DEVICE_CONTEXT ctx
    )
{
    if (ctx->PortCore != NULL)
    {
        avs_core_destroy(ctx->PortCore);
        ctx->PortCore = NULL;
        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "AVS: port -- core destroyed\n"));
    }
    AvsPortFreeTopologyBuffers(ctx);
}

// ---------------------------------------------------------------- endpoints

static ULONG
AvsPortParseEpType(
    _In_ PCSTR Text
    )
{
    if (_stricmp(Text, "headsetmic") == 0 || _stricmp(Text, "headset-mic") == 0)
    {
        return AVS_EP_TYPE_HEADSET_MIC;
    }
    if (_stricmp(Text, "mic") == 0 || _stricmp(Text, "dmic") == 0 ||
        _stricmp(Text, "microphone") == 0)
    {
        return AVS_EP_TYPE_MIC;
    }
    if (_stricmp(Text, "headphones") == 0 || _stricmp(Text, "headphone") == 0 ||
        _stricmp(Text, "hp") == 0)
    {
        return AVS_EP_TYPE_HEADPHONE;
    }
    return AVS_EP_TYPE_SPEAKER;
}

static VOID
AvsPortAddEndpoint(
    _In_ PAVS_DEVICE_CONTEXT ctx,
    _In_ ULONG Index,
    _In_ PCSTR FeReq,
    _In_ ULONG Type
    )
{
    AVS_ENDPOINT* ep = &ctx->Endpoints[Index];
    struct avs_core_fmt fmt;
    CHAR key[32];
    int ret;

    ret = avs_core_find_playback(ctx->PortCore, FeReq, ep->Info.FeName,
        sizeof(ep->Info.FeName), &fmt);
    if (ret != 0)
    {
        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "AVS: endpoint %lu -- FE \"%s\" not in any loaded topology\n", Index, FeReq));
        return;
    }

    ep->Info.Type = Type;
    ep->Info.Capture = AVS_EP_IS_CAPTURE(Type) ? TRUE : FALSE;
    ep->Info.Rate = fmt.rate;
    ep->Info.Channels = fmt.channels;
    ep->Info.ContainerBits = fmt.bits;
    ep->Info.ValidBits = fmt.valid_bits;

    // CSAudio endpointType for start/stop broadcasts, enum from CoolStar's
    // opengmaxcodec.h: DSP=0, Speaker=1, Headphone=2, MicArray=3, MicJack=4.
    // opengmaxcodec only reacts to Speaker; da7219.sys doesn't listen to
    // CSAudio at all (its jack reports go out over HID). Override with
    // Endpoint<n>CsAudioType.
    // Microphones: no broadcast unless configured (no codec driver here
    // needs one; the DMIC sits in the PCH).
    RtlStringCbPrintfA(key, sizeof(key), "Endpoint%luCsAudioType", Index);
    ep->CsAudioType = AvsConfigReadUlong(key,
        (Type == AVS_EP_TYPE_HEADPHONE) ? 2 :
        (Type == AVS_EP_TYPE_SPEAKER) ? AvsConfigReadUlong("CSAudioEndpointTypeSpeaker", 1) :
        AVS_CSAUDIO_NONE);
    ep->Valid = TRUE;

    AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "AVS: endpoint %lu = %s, FE \"%s\": %lu Hz %lu/%lu bit %lu ch, CSAudio type %ld\n",
        Index,
        Type == AVS_EP_TYPE_HEADPHONE ? "headphones" :
        Type == AVS_EP_TYPE_MIC ? "microphones (built-in)" :
        Type == AVS_EP_TYPE_HEADSET_MIC ? "headset microphone" : "speakers",
        ep->Info.FeName,
        ep->Info.Rate, ep->Info.ContainerBits, ep->Info.ValidBits, ep->Info.Channels,
        ep->CsAudioType));
}

// CoreLock held, core up. Endpoint<n>Fe / Endpoint<n>Type in avswin.cfg;
// with none configured: one speaker endpoint on PlaybackFe (or the first
// "*p_fe" template), as the test-tone build did.
VOID
AvsPortDiscoverEndpoints(
    _In_ PAVS_DEVICE_CONTEXT ctx
    )
{
    BOOLEAN any = FALSE;

    RtlZeroMemory(ctx->Endpoints, sizeof(ctx->Endpoints));
    if (ctx->PortCore == NULL)
    {
        return;
    }

    for (ULONG i = 0; i < AVS_MAX_ENDPOINTS; i++)
    {
        CHAR key[32];
        CHAR fe[64];
        CHAR type[16];

        RtlStringCbPrintfA(key, sizeof(key), "Endpoint%luFe", i);
        if (!AvsConfigReadString(key, fe, sizeof(fe)))
        {
            continue;
        }
        any = TRUE;
        RtlStringCbPrintfA(key, sizeof(key), "Endpoint%luType", i);
        if (!AvsConfigReadString(key, type, sizeof(type)))
        {
            type[0] = 's';
            type[1] = '\0';
        }
        AvsPortAddEndpoint(ctx, i, fe, AvsPortParseEpType(type));
    }

    if (!any)
    {
        CHAR fe[64];

        if (!AvsConfigReadString("PlaybackFe", fe, sizeof(fe)))
        {
            fe[0] = '\0';
        }
        AvsPortAddEndpoint(ctx, 0, fe, AVS_EP_TYPE_SPEAKER);
    }
}

// ------------------------------------------------- stream hardware steps
// All with CoreLock held. Order follows upstream pcm.c + soc-pcm.c (see
// port/avs_port_bridge.h): host DMA start before the paths run; on stop
// BE then FE pause, host DMA stop, FE reset.

static NTSTATUS
AvsRtHwOpen(
    _In_ AVS_RT_STREAM* s
    )
{
    PAVS_DEVICE_CONTEXT ctx = s->Ctx;
    AVS_ENDPOINT* ep = &ctx->Endpoints[s->Endpoint];
    NTSTATUS status;
    int ret;

    if (s->Core != NULL)
    {
        return STATUS_SUCCESS;
    }
    if (ctx->PortCore == NULL)
    {
        return STATUS_DEVICE_NOT_READY;
    }
    if (s->BufSize == 0)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }

    status = AvsHostStreamOpen(ctx, &s->Host, &ep->Info, s->BufPa, s->BufSize,
        (INT)ctx->CfgHostStreamFrags);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    // pcm.c: FE path dma_id = hdac_stream(host_stream)->stream_tag - 1
    ret = avs_core_stream_open(ctx->PortCore, ep->Info.FeName,
        (unsigned int)s->Host.Tag - 1, &s->Core);
    if (ret != 0)
    {
        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "AVS: stream %lu -- DSP paths open failed: %d\n", s->Endpoint, ret));
        s->Core = NULL;
        AvsHostStreamClose(ctx, &s->Host);
        return STATUS_UNSUCCESSFUL;
    }
    return STATUS_SUCCESS;
}

static VOID
AvsRtHwClose(
    _In_ AVS_RT_STREAM* s
    )
{
    PAVS_DEVICE_CONTEXT ctx = s->Ctx;

    if (s->Core != NULL && ctx->PortCore != NULL)
    {
        avs_core_stream_close(ctx->PortCore, s->Core);
    }
    s->Core = NULL;
    AvsHostStreamClose(ctx, &s->Host);
}

static NTSTATUS
AvsRtHwStart(
    _In_ AVS_RT_STREAM* s
    )
{
    PAVS_DEVICE_CONTEXT ctx = s->Ctx;
    BOOLEAN debug = ctx->CfgDebugStreams;
    int ret;

    if (s->Core == NULL || ctx->PortCore == NULL)
    {
        return STATUS_DEVICE_NOT_READY;
    }

    if (debug)
    {
        AvsHostStreamDumpRegs(ctx, &s->Host, "before DMA start");
    }

    AvsHostStreamTrigger(ctx, &s->Host, TRUE);
    ret = avs_core_stream_run(ctx->PortCore, s->Core);
    if (ret != 0)
    {
        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "AVS: stream %lu -- DSP paths run failed: %d\n", s->Endpoint, ret));
        avs_core_stream_stop(ctx->PortCore, s->Core);
        AvsHostStreamTrigger(ctx, &s->Host, FALSE);
        avs_core_stream_reset(ctx->PortCore, s->Core);
        return STATUS_UNSUCCESSFUL;
    }

    if (ctx->Endpoints[s->Endpoint].CsAudioType != AVS_CSAUDIO_NONE)
    {
        AvsCsAudioEndpointStartStop(ctx, ctx->Endpoints[s->Endpoint].CsAudioType, TRUE);
    }

    if (debug)
    {
        AvsHostStreamDumpRegs(ctx, &s->Host, "after start");
    }
    return STATUS_SUCCESS;
}

static VOID
AvsRtHwStop(
    _In_ AVS_RT_STREAM* s
    )
{
    PAVS_DEVICE_CONTEXT ctx = s->Ctx;

    if (s->Core == NULL || ctx->PortCore == NULL)
    {
        return;
    }
    if (ctx->Endpoints[s->Endpoint].CsAudioType != AVS_CSAUDIO_NONE)
    {
        AvsCsAudioEndpointStartStop(ctx, ctx->Endpoints[s->Endpoint].CsAudioType, FALSE);
    }
    avs_core_stream_stop(ctx->PortCore, s->Core);
    AvsHostStreamTrigger(ctx, &s->Host, FALSE);
    avs_core_stream_reset(ctx->PortCore, s->Core);
}

// ------------------------------------------------------------ stream API

NTSTATUS
AvsRtStreamCreate(
    _In_ PAVS_DEVICE Dev,
    _In_ ULONG Endpoint,
    _Out_ PAVS_RT_STREAM* Out
    )
{
    PAVS_DEVICE_CONTEXT ctx = Dev;
    AVS_RT_STREAM* s;
    NTSTATUS status = STATUS_SUCCESS;

    *Out = NULL;
    if (Endpoint >= AVS_MAX_ENDPOINTS)
    {
        return STATUS_INVALID_PARAMETER;
    }

    s = (AVS_RT_STREAM*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*s), 'tRvA');
    if (s == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    s->Ctx = ctx;
    s->Endpoint = Endpoint;
    s->State = AVS_RT_STATE_STOP;

    KeWaitForSingleObject(&ctx->CoreLock, Executive, KernelMode, FALSE, NULL);
    if (!ctx->Endpoints[Endpoint].Valid)
    {
        status = STATUS_INVALID_DEVICE_REQUEST;
    }
    else if (ctx->Endpoints[Endpoint].InUse)
    {
        status = STATUS_INSUFFICIENT_RESOURCES;   // one stream per endpoint
    }
    else
    {
        ctx->Endpoints[Endpoint].InUse = TRUE;
        InsertTailList(&ctx->StreamList, &s->Link);
    }
    KeReleaseMutex(&ctx->CoreLock, FALSE);

    if (!NT_SUCCESS(status))
    {
        ExFreePoolWithTag(s, 'tRvA');
        return status;
    }
    *Out = s;
    return STATUS_SUCCESS;
}

VOID
AvsRtStreamDestroy(
    _In_ PAVS_RT_STREAM Stream
    )
{
    AVS_RT_STREAM* s = Stream;
    PAVS_DEVICE_CONTEXT ctx = s->Ctx;

    KeWaitForSingleObject(&ctx->CoreLock, Executive, KernelMode, FALSE, NULL);
    if (s->State == AVS_RT_STATE_RUN)
    {
        AvsRtHwStop(s);
    }
    AvsRtHwClose(s);
    RemoveEntryList(&s->Link);
    ctx->Endpoints[s->Endpoint].InUse = FALSE;
    KeReleaseMutex(&ctx->CoreLock, FALSE);

    ExFreePoolWithTag(s, 'tRvA');
}

VOID
AvsRtStreamSetBuffer(
    _In_ PAVS_RT_STREAM Stream,
    _In_ PHYSICAL_ADDRESS Pa,
    _In_ ULONG Size
    )
{
    AVS_RT_STREAM* s = Stream;
    PAVS_DEVICE_CONTEXT ctx = s->Ctx;

    KeWaitForSingleObject(&ctx->CoreLock, Executive, KernelMode, FALSE, NULL);
    if (Size == 0)
    {
        // Buffer going away: the DMA must not point at it any more.
        if (s->State == AVS_RT_STATE_RUN)
        {
            AvsRtHwStop(s);
        }
        AvsRtHwClose(s);
    }
    s->BufPa = Pa;
    s->BufSize = Size;
    KeReleaseMutex(&ctx->CoreLock, FALSE);
}

NTSTATUS
AvsRtStreamSetState(
    _In_ PAVS_RT_STREAM Stream,
    _In_ ULONG NewState
    )
{
    AVS_RT_STREAM* s = Stream;
    PAVS_DEVICE_CONTEXT ctx = s->Ctx;
    NTSTATUS status = STATUS_SUCCESS;

    if (NewState > AVS_RT_STATE_RUN)
    {
        return STATUS_INVALID_PARAMETER;
    }

    KeWaitForSingleObject(&ctx->CoreLock, Executive, KernelMode, FALSE, NULL);

    AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "AVS: stream %lu -- state %lu -> %lu\n", s->Endpoint, s->State, NewState));

    // KS moves one step at a time; walk the steps anyway.
    while (NT_SUCCESS(status) && s->State != NewState)
    {
        ULONG next = (NewState > s->State) ? s->State + 1 : s->State - 1;

        switch (next * 4 + s->State)
        {
        case AVS_RT_STATE_ACQUIRE * 4 + AVS_RT_STATE_STOP:      // hw_params + prepare
            // No buffer yet: open on PAUSE->RUN instead.
            if (s->BufSize != 0)
            {
                status = AvsRtHwOpen(s);
            }
            break;
        case AVS_RT_STATE_RUN * 4 + AVS_RT_STATE_PAUSE:         // trigger START
            status = AvsRtHwOpen(s);    // no-op unless lost across a power cycle
            if (NT_SUCCESS(status))
            {
                status = AvsRtHwStart(s);
            }
            break;
        case AVS_RT_STATE_PAUSE * 4 + AVS_RT_STATE_RUN:         // trigger STOP
            AvsRtHwStop(s);
            break;
        case AVS_RT_STATE_STOP * 4 + AVS_RT_STATE_ACQUIRE:      // hw_free
            AvsRtHwClose(s);
            break;
        default:                                                // ACQUIRE <-> PAUSE
            break;
        }
        if (NT_SUCCESS(status))
        {
            s->State = next;
        }
    }

    if (!NT_SUCCESS(status))
    {
        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "AVS: stream %lu -- state change to %lu failed: 0x%X (stays %lu)\n",
            s->Endpoint, NewState, status, s->State));
    }

    KeReleaseMutex(&ctx->CoreLock, FALSE);
    return status;
}

ULONG
AvsRtStreamGetPosition(
    _In_ PAVS_RT_STREAM Stream
    )
{
    return AvsHostStreamPosition(Stream->Ctx, &Stream->Host);
}

// ------------------------------------------------------ power transitions

// CoreLock held. DSP state is about to be lost: close every stream's
// hardware side, remember its KS state.
VOID
AvsPortSuspendStreams(
    _In_ PAVS_DEVICE_CONTEXT ctx
    )
{
    for (PLIST_ENTRY e = ctx->StreamList.Flink; e != &ctx->StreamList; e = e->Flink)
    {
        AVS_RT_STREAM* s = CONTAINING_RECORD(e, AVS_RT_STREAM, Link);

        if (s->State == AVS_RT_STATE_RUN)
        {
            AvsRtHwStop(s);
        }
        AvsRtHwClose(s);
    }
}

// CoreLock held, core back up: bring every stream back to its KS state.
VOID
AvsPortResumeStreams(
    _In_ PAVS_DEVICE_CONTEXT ctx
    )
{
    for (PLIST_ENTRY e = ctx->StreamList.Flink; e != &ctx->StreamList; e = e->Flink)
    {
        AVS_RT_STREAM* s = CONTAINING_RECORD(e, AVS_RT_STREAM, Link);
        NTSTATUS status = STATUS_SUCCESS;

        if (s->State >= AVS_RT_STATE_ACQUIRE)
        {
            status = AvsRtHwOpen(s);
        }
        if (NT_SUCCESS(status) && s->State == AVS_RT_STATE_RUN)
        {
            status = AvsRtHwStart(s);
        }
        if (!NT_SUCCESS(status))
        {
            AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                "AVS: stream %lu -- resume to state %lu failed: 0x%X\n",
                s->Endpoint, s->State, status));
        }
    }
}

// -------------------------------------------------------- boot beep (diag)

// BootBeepMs=N (default 0 = off): after start, play a 440 Hz sine on
// endpoint BootBeepEndpoint for N ms through exactly the same stream path
// WaveRT uses, with a register dump and position probe. Separates "DSP
// path broken" from "Windows/WaveRT layer broken". CoreLock held.
VOID
AvsPortBootBeep(
    _In_ PAVS_DEVICE_CONTEXT ctx
    )
{
    ULONG ms = AvsConfigReadUlong("BootBeepMs", 0);
    ULONG epIndex = AvsConfigReadUlong("BootBeepEndpoint", 0);
    const ULONG size = 6 * PAGE_SIZE;       // 24576: 2 frags x 12288 (multiple of 128 and of any frame size)
    PHYSICAL_ADDRESS low, high, boundary;
    AVS_RT_STREAM s;
    PVOID buf;

    if (ms == 0 || epIndex >= AVS_MAX_ENDPOINTS || !ctx->Endpoints[epIndex].Valid ||
        ctx->Endpoints[epIndex].Info.Capture)
    {
        return;
    }

    low.QuadPart = 0;
    high.QuadPart = 0xFFFFFFFFULL;
    boundary.QuadPart = 0;
    buf = MmAllocateContiguousMemorySpecifyCache(size, low, high, boundary, MmNonCached);
    if (buf == NULL)
    {
        return;
    }
    AvsFillTestTone((PUCHAR)buf, size, ctx->Endpoints[epIndex].Info.Rate,
        ctx->Endpoints[epIndex].Info.ContainerBits, ctx->Endpoints[epIndex].Info.Channels);
    KeMemoryBarrier();

    RtlZeroMemory(&s, sizeof(s));
    s.Ctx = ctx;
    s.Endpoint = epIndex;
    s.BufPa = MmGetPhysicalAddress(buf);
    s.BufSize = size;

    AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "AVS: boot beep on endpoint %lu for %lu ms\n", epIndex, ms));

    if (NT_SUCCESS(AvsRtHwOpen(&s)))
    {
        if (NT_SUCCESS(AvsRtHwStart(&s)))
        {
            LARGE_INTEGER delay;

            AvsHostStreamProbePosition(ctx, &s.Host, "boot beep");   // ~500 ms
            if (ms > 500)
            {
                delay.QuadPart = -((LONGLONG)(ms - 500) * 10000);
                KeDelayExecutionThread(KernelMode, FALSE, &delay);
            }
            AvsRtHwStop(&s);
        }
        AvsRtHwClose(&s);
    }

    MmFreeContiguousMemorySpecifyCache(buf, size, MmNonCached);
}
