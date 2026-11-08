// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 AVS OpenAudio Driver Project contributors
//
// avs_nhlt.c -- NHLT (Non-HD-Audio Link Table) ACPI blob parser.
//
// NHLT is a publicly documented Intel ACPI table format (Intel Smart
// Sound Technology NHLT Specification), not driver-specific source --
// struct layouts below are confirmed byte-for-byte against the
// upstream Linux struct definitions in include/sound/intel-nhlt.h,
// independently reimplemented for Windows types.
//
// sklhdaudbus hands us a pointer to this blob already mapped (via
// GetResources()'s NHLT_INFO out-param) -- we don't fetch or validate
// it via ACPI ourselves, just walk what we're given.
//
// Endpoints describe every non-HDA link (I2S/SSP, DMIC, SoundWire) the
// platform exposes, each tagged with its vendor/device/subsystem IDs
// (identifying which codec sits on it) and a virtual_bus_id (which
// physical SSP/I2S instance). This is exactly what's needed to build
// a COPIER module's node_id for talking to an external codec, e.g.
// AVS_DMA_I2S_LINK_OUTPUT + the right vindex.

#include "driver.h"

#pragma pack(push, 1)

// Standard 36-byte ACPI table header, reproduced here only to skip
// past it safely -- we don't need every field, just enough to walk to
// endpoint_count/desc[] and sanity-check the signature.
typedef struct _AVS_ACPI_TABLE_HEADER
{
    UCHAR  Signature[4];
    ULONG  Length;
    UCHAR  Revision;
    UCHAR  Checksum;
    UCHAR  OemId[6];
    UCHAR  OemTableId[8];
    ULONG  OemRevision;
    UCHAR  CreatorId[4];
    ULONG  CreatorRevision;
} AVS_ACPI_TABLE_HEADER;
C_ASSERT(sizeof(AVS_ACPI_TABLE_HEADER) == 36);

typedef struct _AVS_NHLT_TABLE
{
    AVS_ACPI_TABLE_HEADER Header;
    UCHAR                 EndpointCount;
    // endpoints follow immediately, variable length
} AVS_NHLT_TABLE;
C_ASSERT(sizeof(AVS_NHLT_TABLE) == 37);

// struct nhlt_specific_cfg { u32 size; u8 caps[]; } -- device-specific
// config blob, variable length per endpoint.
typedef struct _AVS_NHLT_SPECIFIC_CFG
{
    ULONG Size;
    // caps[] follows, Size bytes
} AVS_NHLT_SPECIFIC_CFG;
C_ASSERT(sizeof(AVS_NHLT_SPECIFIC_CFG) == 4);

// struct nhlt_endpoint -- confirmed field-for-field against
// include/sound/intel-nhlt.h.
typedef struct _AVS_NHLT_ENDPOINT
{
    ULONG  Length;         // total size of this endpoint entry, use to
                            // skip to the next one without parsing formats
    UCHAR  LinkType;        // enum nhlt_link_type: HDA=0 DSP=1 DMIC=2 SSP=3
    UCHAR  InstanceId;
    USHORT VendorId;
    USHORT DeviceId;
    USHORT RevisionId;
    ULONG  SubsystemId;
    UCHAR  DeviceType;      // enum nhlt_device_type: BT=0 DMIC=1 I2S=4
    UCHAR  Direction;
    UCHAR  VirtualBusId;    // which physical SSP/I2S instance
    AVS_NHLT_SPECIFIC_CFG Config;
    // Config.caps[Config.Size] follows, then format descriptors
} AVS_NHLT_ENDPOINT;
C_ASSERT(sizeof(AVS_NHLT_ENDPOINT) == 23);

#pragma pack(pop)

static const char*
AvsNhltLinkTypeName(
    UCHAR LinkType
    )
{
    switch (LinkType)
    {
        case 0: return "HDA";
        case 1: return "DSP";
        case 2: return "DMIC";
        case 3: return "SSP";
        default: return "?";
    }
}

static const char*
AvsNhltDeviceTypeName(
    UCHAR DeviceType
    )
{
    switch (DeviceType)
    {
        case 0: return "BT";
        case 1: return "DMIC";
        case 4: return "I2S";
        default: return "?";
    }
}

// struct wav_fmt / wav_fmt_ext / nhlt_fmt_cfg / nhlt_fmt -- confirmed
// byte-for-byte against include/sound/intel-nhlt.h. These describe
// the per-format capability blob living inside each NHLT endpoint,
// right after that endpoint's own Config.Size bytes -- confirmed
// from upstream intel-nhlt.c: "fmt = (struct nhlt_fmt *)(epnt->
// config.caps + epnt->config.size)".
#pragma pack(push, 1)
typedef struct _AVS_WAV_FMT
{
    USHORT FmtTag;
    USHORT Channels;
    ULONG  SamplesPerSec;
    ULONG  AvgBytesPerSec;
    USHORT BlockAlign;
    USHORT BitsPerSample;
    USHORT CbSize;
} AVS_WAV_FMT;
C_ASSERT(sizeof(AVS_WAV_FMT) == 18);

typedef struct _AVS_WAV_FMT_EXT
{
    AVS_WAV_FMT Fmt;
    USHORT      Sample; // valid_bits_per_sample / samples_per_block / reserved
    ULONG       ChannelMask;
    UCHAR       SubFmt[16];
} AVS_WAV_FMT_EXT;
C_ASSERT(sizeof(AVS_WAV_FMT_EXT) == 40);

// nhlt_fmt_cfg = wav_fmt_ext (40 bytes, fixed) + nhlt_specific_cfg
// (u32 size, then caps[size] bytes -- variable length, so each entry
// must be walked one at a time using its own declared size, not a
// fixed sizeof()).
#pragma pack(pop)

#define AVS_NHLT_WAV_FMT_EXT_SIZE   40u

// Walks the format list inside a single NHLT endpoint (right after
// its own Config.Size bytes) looking for an entry whose wav_fmt_ext
// matches the given audio parameters. On match, returns a pointer to
// that format's specific_cfg.caps and its size -- this is the blob
// path.c's avs_copier_create() copies into a link-copier's
// gtw_cfg.config; sending an empty config there is what caused our
// MOD_NOT_INITIALIZED failure.
static BOOLEAN
AvsNhltMatchFormatInEndpoint(
    _In_ AVS_NHLT_ENDPOINT* Endpoint,
    _In_ ULONGLONG BlobEnd,
    _In_ ULONG NumChannels,
    _In_ ULONG SamplingFreq,
    _In_ ULONG ValidBitDepth,
    _In_ ULONG BitDepth,
    _Out_ PUCHAR* OutCapsPtr,
    _Out_ ULONG* OutCapsSize
    )
{
    PUCHAR fmtListBase = (PUCHAR)&Endpoint->Config + sizeof(ULONG) + Endpoint->Config.Size;
    if ((ULONGLONG)fmtListBase + 1 > BlobEnd)
    {
        return FALSE;
    }

    UCHAR fmtCount = *fmtListBase;
    PUCHAR cursor = fmtListBase + 1;

    AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "AVS: NHLT endpoint (link=%u dir=%u vbus_id=%u) has %u format(s):\n",
        Endpoint->LinkType, Endpoint->Direction, Endpoint->VirtualBusId, fmtCount));

    for (UCHAR i = 0; i < fmtCount; i++)
    {
        if ((ULONGLONG)cursor + AVS_NHLT_WAV_FMT_EXT_SIZE + sizeof(ULONG) > BlobEnd)
        {
            break;
        }

        AVS_WAV_FMT_EXT* fmtExt = (AVS_WAV_FMT_EXT*)cursor;
        PUCHAR specificCfg = cursor + AVS_NHLT_WAV_FMT_EXT_SIZE;
        ULONG capsSize = *(PULONG)specificCfg;
        PUCHAR caps = specificCfg + sizeof(ULONG);

        if ((ULONGLONG)caps + capsSize > BlobEnd)
        {
            break;
        }

        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
            "AVS:   [%u] channels=%u rate=%lu bits_per_sample=%u valid_bits=%u "
            "channel_mask=0x%08lX caps_size=%lu\n",
            i, fmtExt->Fmt.Channels, fmtExt->Fmt.SamplesPerSec,
            fmtExt->Fmt.BitsPerSample, fmtExt->Sample,
            fmtExt->ChannelMask, capsSize));

        if (fmtExt->Fmt.Channels == NumChannels &&
            fmtExt->Fmt.SamplesPerSec == SamplingFreq &&
            fmtExt->Fmt.BitsPerSample == BitDepth &&
            fmtExt->Sample == ValidBitDepth)
        {
            *OutCapsPtr = caps;
            *OutCapsSize = capsSize;
            return TRUE;
        }

        cursor = caps + capsSize;
    }

    return FALSE;
}

// Finds the endpoint matching (LinkType, Direction, VirtualBusId) and,
// within it, the format matching the given audio parameters. Returns
// STATUS_SUCCESS with *OutCapsPtr/*OutCapsSize set on a match,
// STATUS_NOT_FOUND otherwise.
NTSTATUS
AvsNhltFindFormatConfig(
    _In_ PAVS_DEVICE_CONTEXT ctx,
    _In_ UCHAR LinkType,
    _In_ UCHAR Direction,
    _In_ UCHAR VirtualBusId,
    _In_ ULONG NumChannels,
    _In_ ULONG SamplingFreq,
    _In_ ULONG ValidBitDepth,
    _In_ ULONG BitDepth,
    _Out_ PUCHAR* OutCapsPtr,
    _Out_ ULONG* OutCapsSize
    )
{
    if (ctx->Nhlt.nhlt == NULL || ctx->Nhlt.nhltSz < sizeof(AVS_NHLT_TABLE))
    {
        return STATUS_NOT_FOUND;
    }

    AVS_NHLT_TABLE* table = (AVS_NHLT_TABLE*)ctx->Nhlt.nhlt;
    ULONGLONG blobEnd = (ULONGLONG)ctx->Nhlt.nhlt + ctx->Nhlt.nhltSz;
    PUCHAR cursor = (PUCHAR)ctx->Nhlt.nhlt + sizeof(AVS_NHLT_TABLE);

    for (ULONG i = 0; i < table->EndpointCount; i++)
    {
        if ((ULONGLONG)cursor + sizeof(AVS_NHLT_ENDPOINT) > blobEnd)
        {
            break;
        }

        AVS_NHLT_ENDPOINT* ep = (AVS_NHLT_ENDPOINT*)cursor;
        if (ep->Length < sizeof(AVS_NHLT_ENDPOINT) ||
            (ULONGLONG)cursor + ep->Length > blobEnd)
        {
            break;
        }

        if (ep->LinkType == LinkType && ep->Direction == Direction &&
            ep->VirtualBusId == VirtualBusId)
        {
            if (AvsNhltMatchFormatInEndpoint(ep, blobEnd, NumChannels, SamplingFreq,
                    ValidBitDepth, BitDepth, OutCapsPtr, OutCapsSize))
            {
                AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                    "AVS: NHLT format match: link=%u dir=%u vbus_id=%u -> caps_size=%lu\n",
                    LinkType, Direction, VirtualBusId, *OutCapsSize));

                // Raw dump of the endpoint-level config (ep->Config) and the
                // matched format's caps blob -- the latter is the SSP register
                // set (SSCx/SSPSP/SSTSA/...) the firmware programs into the
                // port, i.e. the actual frame format the DSP will drive.
                // Decoded offline from the log, not here.
                AvsDumpHex("NHLT endpoint-level config",
                    (const UCHAR*)&ep->Config + sizeof(ULONG), ep->Config.Size);
                AvsDumpHex("NHLT matched format caps blob",
                    *OutCapsPtr, *OutCapsSize);

                return STATUS_SUCCESS;
            }
        }

        cursor += ep->Length;
    }

    AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
        "AVS: NHLT format not found: link=%u dir=%u vbus_id=%u ch=%lu freq=%lu vbd=%lu bd=%lu\n",
        LinkType, Direction, VirtualBusId, NumChannels, SamplingFreq, ValidBitDepth, BitDepth));
    return STATUS_NOT_FOUND;
}

// Walks the NHLT blob and logs every endpoint's identifying fields.
// Read-only, best-effort -- any length/bounds mismatch just stops the
// walk early rather than risk reading past the mapped blob.
VOID
AvsNhltDumpEndpoints(
    _In_ PAVS_DEVICE_CONTEXT ctx
    )
{
    if (ctx->Nhlt.nhlt == NULL || ctx->Nhlt.nhltSz < sizeof(AVS_NHLT_TABLE))
    {
        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "AVS: NHLT -- blob missing or too small to contain a header\n"));
        return;
    }

    AVS_NHLT_TABLE* table = (AVS_NHLT_TABLE*)ctx->Nhlt.nhlt;

    if (RtlCompareMemory(table->Header.Signature, "NHLT", 4) != 4)
    {
        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "AVS: NHLT -- signature mismatch (got %.4s), refusing to parse\n",
            table->Header.Signature));
        return;
    }

    AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "AVS: NHLT -- table length=0x%lX, endpoint_count=%u\n",
        table->Header.Length, table->EndpointCount));

    ULONGLONG blobEnd = (ULONGLONG)ctx->Nhlt.nhlt + ctx->Nhlt.nhltSz;
    PUCHAR cursor = (PUCHAR)ctx->Nhlt.nhlt + sizeof(AVS_NHLT_TABLE);

    for (ULONG i = 0; i < table->EndpointCount; i++)
    {
        if ((ULONGLONG)cursor + sizeof(AVS_NHLT_ENDPOINT) > blobEnd)
        {
            AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                "AVS: NHLT -- endpoint %lu would read past blob end, stopping\n", i));
            break;
        }

        AVS_NHLT_ENDPOINT* ep = (AVS_NHLT_ENDPOINT*)cursor;

        if (ep->Length < sizeof(AVS_NHLT_ENDPOINT) ||
            (ULONGLONG)cursor + ep->Length > blobEnd)
        {
            AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                "AVS: NHLT -- endpoint %lu has bad length=0x%lX, stopping\n",
                i, ep->Length));
            break;
        }

        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
            "AVS: NHLT [%lu] link=%s(%u) device=%s(%u) dir=%u vbus_id=%u "
            "vendor=0x%04X dev=0x%04X rev=0x%04X subsys=0x%08lX cfg_size=%lu len=0x%lX\n",
            i,
            AvsNhltLinkTypeName(ep->LinkType), ep->LinkType,
            AvsNhltDeviceTypeName(ep->DeviceType), ep->DeviceType,
            ep->Direction, ep->VirtualBusId,
            ep->VendorId, ep->DeviceId, ep->RevisionId, ep->SubsystemId,
            ep->Config.Size, ep->Length));

        cursor += ep->Length;
    }
}
