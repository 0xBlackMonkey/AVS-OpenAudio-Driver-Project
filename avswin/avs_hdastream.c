// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 AVS OpenAudio Driver Project contributors
//
// avs_hdastream.c -- HDA host streams (the PCM DMA from system memory to
// the DSP's host copier gateway), one per open WaveRT stream.
//
// sklhdaudbus implements the stream engine and exposes it through
// ADSP_BUS_INTERFACE (adsp.h): GetRenderStream / PrepareDSP / TriggerDSP /
// StreamPosition / CleanupDSP / FreeStream. Confirmed from its adsp.cpp:
// PrepareDSP does NOT allocate the sample buffer -- its bdlBuf out-param is
// the BDL descriptor array (NumBlocks * 16 bytes), which WE fill with the
// physical address of our own buffer. The buffer now comes from PortCls
// (IPortWaveRTStream::AllocateContiguousPagesForMdl, see minwavert.cpp).
//
// The stream tag selects the DSP gateway: host copier vindex = tag - 1
// (upstream pcm.c, confirmed on hardware).
//
// Cache coherency (confirmed on hardware): PCI DEVC(0x78)=0x2800, NOSNOOP=1,
// so the controller reads DRAM directly -> audio buffers must be non-cached
// (minwavert.cpp maps them MmNonCached). The BDL array belongs to
// sklhdaudbus, so our writes to it are pushed out with clflush.

#include "driver.h"
#include <intrin.h>   // _mm_clflush / _mm_mfence

// struct HDAC_BDLENTRY -- confirmed byte-for-byte against fdo.h.
// This is what PrepareDSP's bdlBuf out-param actually points to: an
// array of these, one per fragment, which WE must fill with our
// buffer's physical address (split lo/hi) and length.
#pragma pack(push, 1)
typedef struct _AVS_HDAC_BDLENTRY
{
    ULONG LowAddr;
    ULONG HighAddr;
    ULONG Len;
    ULONG Ioc;
} AVS_HDAC_BDLENTRY;
C_ASSERT(sizeof(AVS_HDAC_BDLENTRY) == 16);
#pragma pack(pop)

// PCI config: Intel PCH HDA "DEVC" register, bit 11 = NOSNOOP (from
// memory of Linux hda_intel.c INTEL_SCH_HDA_DEVC -- read-only use here).
#define AVS_PCIREG_DEVC             0x78

// BAR0 HDA controller registers (public HD Audio spec / hda_register.h)
#define AVS_HDA_REG_GCAP            0x00
#define AVS_HDA_REG_DPLBASE         0x70
#define AVS_HDA_REG_DPUBASE         0x74
#define AVS_HDA_SD_BASE             0x80
#define AVS_HDA_SD_SIZE             0x20
#define AVS_HDA_SD_CTL              0x00
#define AVS_HDA_SD_LPIB             0x04
#define AVS_HDA_SD_CBL              0x08
#define AVS_HDA_SD_LVI              0x0C
#define AVS_HDA_SD_FIFOS            0x10
#define AVS_HDA_SD_FMT              0x12
#define AVS_HDA_SD_BDPL             0x18
#define AVS_HDA_SD_BDPU             0x1C

// Writes back + invalidates every 64-byte cache line covering [Va, Va+Size).
// Cache maintenance only -- never modifies memory contents.
static VOID
AvsFlushCacheRange(
    _In_ PVOID Va,
    _In_ ULONG Size
    )
{
    ULONG_PTR start = (ULONG_PTR)Va & ~(ULONG_PTR)63;
    ULONG_PTR end = (ULONG_PTR)Va + Size;

    for (ULONG_PTR p = start; p < end; p += 64)
    {
        _mm_clflush((const void*)p);
    }
    _mm_mfence();
}

#define AVS_TEST_TONE_FREQ_HZ       440

// One full sine period, 64 points, amplitude 32767 (integer table: no
// kernel floating point needed).
static const SHORT AvsSine64[64] = {
         0,   3212,   6393,   9512,  12539,  15446,  18204,  20787,
     23170,  25329,  27245,  28898,  30273,  31356,  32137,  32609,
     32767,  32609,  32137,  31356,  30273,  28898,  27245,  25329,
     23170,  20787,  18204,  15446,  12539,   9512,   6393,   3212,
         0,  -3212,  -6393,  -9512, -12539, -15446, -18204, -20787,
    -23170, -25329, -27245, -28898, -30273, -31356, -32137, -32609,
    -32767, -32609, -32137, -31356, -30273, -28898, -27245, -25329,
    -23170, -20787, -18204, -15446, -12539,  -9512,  -6393,  -3212,
};

// Fills Buffer with an interleaved 440 Hz sine, all channels equal.
// Phase accumulator in 16.16 fixed point over the 64-entry table; the
// buffer is replayed in a loop by the DMA, so a small click at the wrap
// point is expected. Amplitude: avswin.cfg TestToneAmplitude (16-bit scale).
// 32-bit containers get the sample MSB-aligned (<< 16).
VOID
AvsFillTestTone(
    _Out_writes_bytes_(Size) PUCHAR Buffer,
    _In_ ULONG Size,
    _In_ ULONG SampleRate,
    _In_ ULONG ContainerBits,
    _In_ ULONG NumChannels
    )
{
    ULONG amplitude = AvsConfigReadUlong("TestToneAmplitude", 3000);
    if (amplitude > 32767)
    {
        amplitude = 32767;
    }

    ULONG bytesPerSample = ContainerBits / 8;
    ULONG frameBytes = bytesPerSample * NumChannels;
    ULONG frames = (frameBytes != 0) ? Size / frameBytes : 0;
    // table steps per sample, 16.16: 64 * f / rate
    ULONG step = (ULONG)(((ULONGLONG)64 * AVS_TEST_TONE_FREQ_HZ << 16) / SampleRate);
    ULONG phase = 0;

    RtlZeroMemory(Buffer, Size);

    for (ULONG f = 0; f < frames; f++)
    {
        LONG v = ((LONG)AvsSine64[(phase >> 16) & 63] * (LONG)amplitude) / 32767;
        PUCHAR frame = Buffer + (SIZE_T)f * frameBytes;

        for (ULONG ch = 0; ch < NumChannels; ch++)
        {
            if (bytesPerSample == 2)
            {
                ((PSHORT)frame)[ch] = (SHORT)v;
            }
            else if (bytesPerSample == 4)
            {
                ((PLONG)frame)[ch] = v * 65536;
            }
        }
        phase += step;
    }
}

// ---- Read-only BAR0 stream diagnostics --------------------------------------
//
// Register layout from the public HD Audio spec / include/sound/hda_register.h:
//   GCAP    0x00 (16) : OSS=bits15:12, ISS=bits11:8, BSS=bits7:3
//   DPLBASE 0x70      : bit0 = DMA position buffer enable
//   SDn base = 0x80 + n*0x20, input streams first, then output, then bidir
//     +0x00 CTL (24-bit; RUN=bit1, STRM tag=bits23:20) | +0x03 STS (8-bit)
//     +0x04 LPIB  +0x08 CBL  +0x0C LVI(16)  +0x10 FIFOS(16)  +0x12 FMT(16)
//     +0x18 BDPL  +0x1C BDPU
// Nothing here writes a register -- STS is W1C but we only read it.

static PUCHAR
AvsHdaFindStreamSd(
    _In_ PAVS_DEVICE_CONTEXT ctx,
    _In_ PAVS_HOST_STREAM hs,
    _Out_opt_ PULONG OutIndex
    )
{
    if (ctx->HdaBar.Base.Base == NULL || hs->Handle == NULL || hs->Tag == 0)
    {
        return NULL;
    }

    PUCHAR bar0 = ctx->HdaBar.Base.baseptr;
    USHORT gcap = READ_REGISTER_USHORT((PUSHORT)(bar0 + AVS_HDA_REG_GCAP));
    ULONG iss = (gcap >> 8) & 0xFu;
    ULONG oss = (gcap >> 12) & 0xFu;
    ULONG bss = (gcap >> 3) & 0x1Fu;

    // Render tags and capture tags are numbered independently, so search
    // only the SDs of our direction: inputs [0, iss), outputs [iss,
    // iss+oss), bidirectional after that.
    ULONG first = hs->Capture ? 0 : iss;
    ULONG last = hs->Capture ? iss : iss + oss;

    for (ULONG idx = first; idx < last && idx < 30; idx++)
    {
        PUCHAR sd = bar0 + AVS_HDA_SD_BASE + idx * AVS_HDA_SD_SIZE;
        ULONG ctl = READ_REGISTER_ULONG((PULONG)(sd + AVS_HDA_SD_CTL)) & 0x00FFFFFFu;
        ULONG tag = (ctl >> 20) & 0xFu;
        if (tag == hs->Tag)
        {
            if (OutIndex != NULL)
            {
                *OutIndex = idx;
            }
            return sd;
        }
    }
    if (bss != 0)
    {
        // then the bidirectional ones (none on this controller so far)
        for (ULONG idx = iss + oss; idx < iss + oss + bss && idx < 30; idx++)
        {
            PUCHAR sd = bar0 + AVS_HDA_SD_BASE + idx * AVS_HDA_SD_SIZE;
            ULONG ctl = READ_REGISTER_ULONG((PULONG)(sd + AVS_HDA_SD_CTL)) & 0x00FFFFFFu;
            if (((ctl >> 20) & 0xFu) == hs->Tag)
            {
                if (OutIndex != NULL)
                {
                    *OutIndex = idx;
                }
                return sd;
            }
        }
    }

    return NULL;
}

VOID
AvsHostStreamDumpRegs(
    _In_ PAVS_DEVICE_CONTEXT ctx,
    _In_ PAVS_HOST_STREAM hs,
    _In_ PCSTR Label
    )
{
    if (ctx->HdaBar.Base.Base == NULL)
    {
        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "AVS: [%s] HDA regs -- BAR0 not mapped\n", Label));
        return;
    }

    PUCHAR bar0 = ctx->HdaBar.Base.baseptr;
    USHORT gcap = READ_REGISTER_USHORT((PUSHORT)(bar0 + AVS_HDA_REG_GCAP));
    ULONG dplbase = READ_REGISTER_ULONG((PULONG)(bar0 + AVS_HDA_REG_DPLBASE));
    ULONG dpubase = READ_REGISTER_ULONG((PULONG)(bar0 + AVS_HDA_REG_DPUBASE));
    ULONG ppctl = (ctx->Ppcap != NULL)
        ? READ_REGISTER_ULONG((PULONG)((PUCHAR)ctx->Ppcap + AZX_REG_PP_PPCTL)) : 0xDEADBEEFu;

    AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "AVS: [%s] GCAP=0x%04X (ISS=%u OSS=%u BSS=%u) DPLBASE=0x%08X (posbuf %s) DPUBASE=0x%08X PPCTL=0x%08X\n",
        Label, gcap, (gcap >> 8) & 0xF, (gcap >> 12) & 0xF, (gcap >> 3) & 0x1F,
        dplbase, (dplbase & 1u) ? "ENABLED" : "DISABLED", dpubase, ppctl));

    ULONG idx = 0;
    PUCHAR sd = AvsHdaFindStreamSd(ctx, hs, &idx);
    if (sd == NULL)
    {
        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "AVS: [%s] no output SD carries our stream tag %u\n", Label, hs->Tag));
        return;
    }

    ULONG ctlsts = READ_REGISTER_ULONG((PULONG)(sd + AVS_HDA_SD_CTL));
    ULONG ctl = ctlsts & 0x00FFFFFFu;
    ULONG sts = ctlsts >> 24;
    ULONG lpib = READ_REGISTER_ULONG((PULONG)(sd + AVS_HDA_SD_LPIB));
    ULONG cbl = READ_REGISTER_ULONG((PULONG)(sd + AVS_HDA_SD_CBL));
    USHORT lvi = READ_REGISTER_USHORT((PUSHORT)(sd + AVS_HDA_SD_LVI));
    USHORT fifos = READ_REGISTER_USHORT((PUSHORT)(sd + AVS_HDA_SD_FIFOS));
    USHORT fmt = READ_REGISTER_USHORT((PUSHORT)(sd + AVS_HDA_SD_FMT));
    ULONG bdpl = READ_REGISTER_ULONG((PULONG)(sd + AVS_HDA_SD_BDPL));
    ULONG bdpu = READ_REGISTER_ULONG((PULONG)(sd + AVS_HDA_SD_BDPU));

    // FMT for 48 kHz / 16-bit / 2ch is 0x0011 (BASE=0 MULT=0 DIV=0 BITS=001 CHAN=0001)
    AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "AVS: [%s] SD%lu (tag %u): CTL=0x%06X (RUN=%u SRST=%u) STS=0x%02X "
        "LPIB=%lu CBL=%lu LVI=%u FIFOS=%u FMT=0x%04X BDP=0x%08X:%08X PROCEN=%u\n",
        Label, idx, hs->Tag, ctl, (ctl >> 1) & 1u, ctl & 1u, sts,
        lpib, cbl, lvi, fifos, fmt, bdpu, bdpl,
        (ctx->Ppcap != NULL) ? ((ppctl >> idx) & 1u) : 0xFFu));
}

VOID
AvsHostStreamProbePosition(
    _In_ PAVS_DEVICE_CONTEXT ctx,
    _In_ PAVS_HOST_STREAM hs,
    _In_ PCSTR Label
    )
{
    if (hs->Handle == NULL)
    {
        return;
    }

    PUCHAR sd = AvsHdaFindStreamSd(ctx, hs, NULL);
    static const ULONG delaysMs[5] = { 0, 10, 40, 150, 300 };
    ULONG prevLpib = 0, prevPos = 0;
    BOOLEAN lpibMoved = FALSE, posMoved = FALSE;

    for (ULONG i = 0; i < 5; i++)
    {
        if (delaysMs[i] != 0)
        {
            LARGE_INTEGER delay;
            delay.QuadPart = -((LONGLONG)delaysMs[i] * 10000);
            KeDelayExecutionThread(KernelMode, FALSE, &delay);
        }

        ULONG pos = (ctx->AdspInterface.StreamPosition != NULL)
            ? ctx->AdspInterface.StreamPosition(ctx->AdspInterface.Context, hs->Handle)
            : 0xFFFFFFFFu;
        ULONG lpib = (sd != NULL) ? READ_REGISTER_ULONG((PULONG)(sd + AVS_HDA_SD_LPIB)) : 0xFFFFFFFFu;
        ULONG sts = (sd != NULL) ? (READ_REGISTER_ULONG((PULONG)(sd + AVS_HDA_SD_CTL)) >> 24) : 0xFFu;

        if (i > 0)
        {
            if (lpib != prevLpib) lpibMoved = TRUE;
            if (pos != prevPos) posMoved = TRUE;
        }
        prevLpib = lpib;
        prevPos = pos;

        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
            "AVS: [%s] t+%lums: posbuf=%lu LPIB=%lu SD_STS=0x%02X (BCIS=%u FIFOE=%u DESE=%u)\n",
            Label, delaysMs[i], pos, lpib, sts,
            (sts >> 2) & 1u, (sts >> 3) & 1u, (sts >> 4) & 1u));
    }

    AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "AVS: [%s] VERDICT: LPIB %s, posbuf %s\n", Label,
        lpibMoved ? "MOVING (host DMA really runs)" : "FROZEN",
        posMoved ? "MOVING" : "FROZEN (normal if DPLBASE disabled)"));
}

// ------------------------------------------------------------ host stream

NTSTATUS
AvsHostStreamOpen(
    _In_ PAVS_DEVICE_CONTEXT ctx,
    _Out_ PAVS_HOST_STREAM hs,
    _In_ const AVS_EP_INFO* Fmt,
    _In_ PHYSICAL_ADDRESS BufferPa,
    _In_ ULONG BufferSize,
    _In_ INT Frags
    )
{
    HDAUDIO_STREAM_FORMAT format;
    HANDLE handle = NULL;
    UINT8 tag = 0;
    PVOID bdlArray = NULL;
    NTSTATUS status;

    RtlZeroMemory(hs, sizeof(*hs));

    PGET_STREAM getStream = Fmt->Capture ? ctx->AdspInterface.GetCaptureStream
                                         : ctx->AdspInterface.GetRenderStream;

    if (getStream == NULL || ctx->AdspInterface.PrepareDSP == NULL)
    {
        return STATUS_NOT_SUPPORTED;
    }
    if (Frags <= 0 || BufferSize == 0 || (BufferSize % (ULONG)Frags) != 0 ||
        ((BufferSize / (ULONG)Frags) % 128) != 0 || (BufferPa.QuadPart & 127) != 0)
    {
        // HDA: BDL buffers 128-byte aligned (PortCls pages are), lengths
        // kept to multiples of 128 (Linux constrains periods the same way).
        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "AVS: host stream -- bad geometry: size=%lu frags=%d pa=0x%llX\n",
            BufferSize, Frags, BufferPa.QuadPart));
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(&format, sizeof(format));
    format.SampleRate = Fmt->Rate;
    format.ValidBitsPerSample = (USHORT)Fmt->ValidBits;
    format.ContainerSize = (USHORT)Fmt->ContainerBits;
    format.NumberOfChannels = (USHORT)Fmt->Channels;

    status = getStream(ctx->AdspInterface.Context, format, &handle, &tag);
    if (!NT_SUCCESS(status) || handle == NULL)
    {
        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "AVS: host stream -- Get%sStream failed: 0x%X\n",
            Fmt->Capture ? "Capture" : "Render", status));
        return NT_SUCCESS(status) ? STATUS_INSUFFICIENT_RESOURCES : status;
    }

    status = ctx->AdspInterface.PrepareDSP(ctx->AdspInterface.Context, handle,
        BufferSize, Frags, &bdlArray);
    if (!NT_SUCCESS(status) || bdlArray == NULL)
    {
        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "AVS: host stream -- PrepareDSP failed: 0x%X\n", status));
        if (ctx->AdspInterface.FreeStream != NULL)
        {
            NTSTATUS st = ctx->AdspInterface.FreeStream(ctx->AdspInterface.Context, handle);
            UNREFERENCED_PARAMETER(st);
        }
        return NT_SUCCESS(status) ? STATUS_UNSUCCESSFUL : status;
    }

    // Frags equal fragments of the physically contiguous buffer. IOC on
    // every entry, as in the test-tone build that played cleanly.
    {
        AVS_HDAC_BDLENTRY* bdl = (AVS_HDAC_BDLENTRY*)bdlArray;
        ULONG fragSize = BufferSize / (ULONG)Frags;

        for (INT f = 0; f < Frags; f++)
        {
            ULONGLONG pa = (ULONGLONG)BufferPa.QuadPart + ((ULONGLONG)f * fragSize);
            bdl[f].LowAddr = (ULONG)(pa & 0xFFFFFFFFu);
            bdl[f].HighAddr = (ULONG)(pa >> 32);
            bdl[f].Len = fragSize;
            bdl[f].Ioc = 1;
        }
        AvsFlushCacheRange(bdlArray, (ULONG)Frags * sizeof(AVS_HDAC_BDLENTRY));
        KeMemoryBarrier();
    }

    hs->Handle = handle;
    hs->Capture = Fmt->Capture;
    hs->Tag = tag;
    hs->Bdl = bdlArray;
    hs->Frags = Frags;
    hs->Size = BufferSize;
    hs->Sd = AvsHdaFindStreamSd(ctx, hs, NULL);

    AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "AVS: host stream -- %s tag %u, %lu Hz %lu/%lu bit %lu ch, buffer pa=0x%llX size=%lu frags=%d, SD %s\n",
        Fmt->Capture ? "capture" : "render", tag, Fmt->Rate, Fmt->ContainerBits, Fmt->ValidBits, Fmt->Channels,
        BufferPa.QuadPart, BufferSize, Frags, hs->Sd ? "found" : "not found yet"));

    return STATUS_SUCCESS;
}

VOID
AvsHostStreamTrigger(
    _In_ PAVS_DEVICE_CONTEXT ctx,
    _In_ PAVS_HOST_STREAM hs,
    _In_ BOOLEAN Start
    )
{
    if (hs->Handle == NULL || ctx->AdspInterface.TriggerDSP == NULL)
    {
        return;
    }
    ctx->AdspInterface.TriggerDSP(ctx->AdspInterface.Context, hs->Handle, Start);
    if (hs->Sd == NULL)
    {
        hs->Sd = AvsHdaFindStreamSd(ctx, hs, NULL);
    }
}

// LPIB (link position in buffer) -- proven to move on this hardware.
// StreamPosition() as fallback when our SD can't be located.
ULONG
AvsHostStreamPosition(
    _In_ PAVS_DEVICE_CONTEXT ctx,
    _In_ PAVS_HOST_STREAM hs
    )
{
    PUCHAR sd = hs->Sd;
    ULONG size = hs->Size;
    ULONG pos;

    if (hs->Handle == NULL || size == 0)
    {
        return 0;
    }
    if (sd != NULL)
    {
        pos = READ_REGISTER_ULONG((PULONG)(sd + AVS_HDA_SD_LPIB));
    }
    else if (ctx->AdspInterface.StreamPosition != NULL)
    {
        pos = ctx->AdspInterface.StreamPosition(ctx->AdspInterface.Context, hs->Handle);
    }
    else
    {
        pos = 0;
    }
    return (pos == 0xFFFFFFFFu) ? 0 : pos % size;
}

VOID
AvsHostStreamClose(
    _In_ PAVS_DEVICE_CONTEXT ctx,
    _Inout_ PAVS_HOST_STREAM hs
    )
{
    HANDLE handle = hs->Handle;
    UINT8 tag = hs->Tag;

    if (handle == NULL)
    {
        return;
    }
    // Position readers check Handle/Size; clear them before the stream goes.
    hs->Sd = NULL;
    hs->Size = 0;
    hs->Handle = NULL;
    KeMemoryBarrier();

    if (ctx->AdspInterface.TriggerDSP != NULL)
    {
        ctx->AdspInterface.TriggerDSP(ctx->AdspInterface.Context, handle, FALSE);
    }
    if (ctx->AdspInterface.CleanupDSP != NULL)
    {
        NTSTATUS st = ctx->AdspInterface.CleanupDSP(ctx->AdspInterface.Context, handle);
        UNREFERENCED_PARAMETER(st);
    }
    if (ctx->AdspInterface.FreeStream != NULL)
    {
        NTSTATUS st = ctx->AdspInterface.FreeStream(ctx->AdspInterface.Context, handle);
        UNREFERENCED_PARAMETER(st);
    }
    AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "AVS: host stream -- tag %u released\n", tag));
    RtlZeroMemory(hs, sizeof(*hs));
}
