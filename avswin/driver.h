// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 AVS OpenAudio Driver Project contributors
//
// driver.h -- C half of the AVS driver (DSP boot, IPC, HDA host streams,
// glue to the ported Linux core). The PortCls/WaveRT layer (*.cpp) sits on
// top and only sees avs_api.h.
//
// We are a function driver on sklhdaudbus's CSAUDIO\ADSP child PDO. The
// bus driver owns the PCI device (8086:9D71) and hands us mapped BARs,
// the NHLT, the IPC interrupt and HDA host streams through the
// GUID_ADSP_BUS_INTERFACE query-interface contract (adsp.h).

#pragma once

#include <ntddk.h>
#include <initguid.h>
#include <hdaudio.h>

// adsp.h (from sklhdaudbus) uses the Win32 BOOL type, which isn't part
// of the standard kernel-mode header set -- define it ourselves rather
// than pulling in <windef.h>, which conflicts with kernel headers.
typedef int BOOL;

// _PCI_BAR layout must match sklhdaudbus's fdo.h exactly -- this is the
// struct GetResources() fills in, memory already mapped by the bus driver.
union avs_baseaddr {
    PVOID Base;
    UINT8* baseptr;
};

typedef struct _PCI_BAR {
    union avs_baseaddr Base;
    ULONG Len;
} PCI_BAR, * PPCI_BAR;

#include "adsp.h"
#include "avs_regs.h"
#include "avs_api.h"
#include "port/avs_port_bridge.h"   // the only header shared with the ported Linux core

#define AVS_CTX_POOL_TAG    'xCvA'

// Kernel log of the C part. DbgPrintEx (not KdPrintEx) so it exists in
// Release builds too -- the same diagnostics for everyone, and no
// "unused variable" warnings for values that are only logged.
// Usage keeps KdPrintEx's double parentheses: AVS_KDPRINT((id, level, fmt, ...)).
// ERROR_LEVEL lines always show in DebugView; INFO_LEVEL ones need the
// Debug Print Filter (or DebugView's "Enable Verbose Kernel Output").
#define AVS_KDPRINT(_x_)    DbgPrintEx _x_

// Data files: avswin.cfg, topologies (*-tplg.bin), dsp_basefw.bin
#define AVS_DATA_DIR        L"\\SystemRoot\\System32\\drivers\\avs\\"
#define AVS_CSAUDIO_NONE    0xFFFFFFFFu

// ---- HDA host stream (one per open WaveRT stream) ---------------------------
// sklhdaudbus owns the stream registers; GetRenderStream/PrepareDSP give us
// a stream + its BDL array, which we fill with our buffer's address.
typedef struct _AVS_HOST_STREAM
{
    HANDLE      Handle;         // NULL = not open
    UINT8       Tag;            // copier gateway vindex = Tag - 1 (pcm.c)
    PVOID       Bdl;            // BDL array owned by sklhdaudbus
    INT         Frags;
    BOOLEAN     Capture;        // input stream (SD search range)
    ULONG       Size;           // cyclic buffer size (CBL)
    PUCHAR volatile Sd;         // our SDn register block in BAR0 (found by tag), for LPIB
} AVS_HOST_STREAM, *PAVS_HOST_STREAM;

// ---- endpoint (one Windows render device = one topology FE) -----------------
typedef struct _AVS_ENDPOINT
{
    BOOLEAN     Valid;
    BOOLEAN     InUse;          // one WaveRT stream per endpoint
    AVS_EP_INFO Info;
    ULONG       CsAudioType;    // CSAudio broadcast endpointType for start/stop
                                // (AVS_CSAUDIO_NONE: no broadcast)
} AVS_ENDPOINT;

// ---- C side of one WaveRT stream (avs_port_win.c) ---------------------------
typedef struct _AVS_RT_STREAM
{
    struct _AVS_DEVICE_CONTEXT* Ctx;
    LIST_ENTRY  Link;           // ctx->StreamList, for suspend/resume
    ULONG       Endpoint;
    ULONG       State;          // AVS_RT_STATE_*
    PHYSICAL_ADDRESS BufPa;
    ULONG       BufSize;
    AVS_HOST_STREAM Host;
    struct avs_core_stream* Core;   // DSP paths, NULL = not open
} AVS_RT_STREAM;

// ---- Device context -----------------------------------------------------
typedef struct _AVS_DEVICE_CONTEXT
{
    PDEVICE_OBJECT Fdo;                // PortCls FDO
    PDEVICE_OBJECT Pdo;                // CSAUDIO\ADSP child of sklhdaudbus

    ADSP_BUS_INTERFACE AdspInterface;   // IRP_MN_QUERY_INTERFACE to the PDO
    BOOLEAN     AdspInterfaceValid;
    BOOLEAN     InterruptRegistered;
    BOOLEAN     HwStarted;             // AvsDeviceStart completed (interface + resources)
    DEVICE_POWER_STATE PowerState;

    PCI_BAR     HdaBar;                // BAR0, from GetResources
    PCI_BAR     DspBar;                // BAR4, from GetResources -- our register window
    PVOID       Ppcap;
    NHLT_INFO   Nhlt;                  // NHLT blob from bus driver
    BUS_INTERFACE_STANDARD PciConfig;  // raw PCI config space access

    BOOLEAN     DspFirmwareLoaded;   // set TRUE once we see AVS_NOTIFY_FW_READY

    // Buffers allocated by AvsCldmaPrepare, consumed by
    // AvsCldmaTriggerAndWaitBasefw, freed by AvsCldmaRelease.
    PVOID       FwDataBuf;
    PHYSICAL_ADDRESS FwDataPhys;
    PVOID       FwBdlBuf;
    ULONG       FwPayloadSize;

    // IPC transport (send + wait for reply). The ISR callback runs at
    // the bus driver's real interrupt IRQL, where KeSetEvent is not
    // safe to call directly -- so it stashes the reply and defers
    // completion to this DPC, which runs at DISPATCH_LEVEL.
    KDPC        IpcDpc;
    KEVENT      IpcDoneEvent;
    volatile ULONG IpcReplyHipct;
    volatile ULONG IpcReplyHipcte;
    BOOLEAN     IpcInfraInitialized;

    // FW_READY notification (separate handshake from FW_STATUS polling,
    // upstream avs_dsp_load_basefw()); deferred through its own DPC.
    KDPC        FwReadyDpc;
    KEVENT      FwReadyEvent;

    // avswin.cfg values used on hot paths, read once at start
    BOOLEAN     IpcDumpPayloads;         // log every IPC message + hex-dump payloads
    BOOLEAN     CfgDebugStreams;         // host stream register dump on start
    ULONG       CfgHostStreamFrags;      // BDL entries per cyclic buffer
    ULONG       CfgCsAudioStart;         // CSAudio endpointRequest values
    ULONG       CfgCsAudioStop;

    // IPC reply payload capture: the ISR copies the uplink mailbox here
    // BEFORE acking HIPCT (upstream avs_dsp_receive_rx() order).
    PUCHAR      IpcRxBuf;               // AVS_MAILBOX_SIZE, non-paged
    volatile ULONG IpcRxExpect;
    volatile ULONG IpcRxSize;

    // Ported Linux AVS core (port/), see avs_port_win.c. Every call into
    // the core (and so every IPC) happens under CoreLock, PASSIVE_LEVEL.
    KMUTEX      CoreLock;
    struct avs_core *PortCore;
    PVOID       PortTplgBuf[4];         // topology file images, kept while the core lives
    ULONG       PortLinkMask;           // SSP ports used by the loaded topologies

    AVS_ENDPOINT Endpoints[AVS_MAX_ENDPOINTS];
    LIST_ENTRY  StreamList;             // AVS_RT_STREAM.Link, under CoreLock

    // Headphone jack via da7219.sys's HID collection (avs_jack.c)
    FAST_MUTEX  JackLock;               // notify registrations + their calls
    AVS_JACK_NOTIFY JackNotify[AVS_MAX_ENDPOINTS];
    PVOID       JackNotifyCtx[AVS_MAX_ENDPOINTS];
    volatile LONG JackState;            // -1 unknown, else SND_JACK_* bits (0 = unplugged)
    volatile LONG JackReaderRunning;    // a reader thread owns the HID handle
    volatile LONG JackStopping;
    BOOLEAN     JackEnabled;
    PVOID       JackPnpEntry;           // IoRegisterPlugPlayNotification handle
    PKTHREAD    JackThread;
    PIRP volatile JackIrp;              // the reader's own IRP (cancelled on stop)
    KSPIN_LOCK  JackIrpLock;            // JackIrp pointer vs. IoCancelIrp/IoFreeIrp
    WCHAR       JackMatchId[48];        // e.g. VID_2DCF&PID_7219 (upper case)
    WCHAR       JackMatchCol[16];       // e.g. COL02 (upper case, may be empty)

} AVS_DEVICE_CONTEXT, *PAVS_DEVICE_CONTEXT;

// ---- RESOURCE_EVENT notification payload (AVS_NOTIFY_RESOURCE_EVENT=5) ----
// Confirmed byte-for-byte against messages.h's struct avs_notify_res_data.
// Lives in the SRAM uplink mailbox, same window GET_FW_CONFIG/MODULES_INFO
// replies use -- must be read BEFORE acking (clearing) HIPCT_BUSY.
#pragma pack(push, 1)
typedef struct _AVS_NOTIFY_RES_DATA
{
    ULONG ResourceType;
    ULONG ResourceId;
    ULONG EventType;
    ULONG Reserved;
    ULONG Data[6];
} AVS_NOTIFY_RES_DATA;
C_ASSERT(sizeof(AVS_NOTIFY_RES_DATA) == 40);
#pragma pack(pop)


// ---- Interrupt -----------------------------------------------------------
// Called back by sklhdaudbus (via RegisterInterrupt) when ADSPIS_IPC or
// ADSPIS_CLDMA fires. Runs at the bus driver's interrupt context: register
// access only, real work deferred to DPCs (PADSP_INTERRUPT_CALLBACK, adsp.h).
BOOL AvsAdspInterruptCallback(_In_ PVOID Context);

// ---- DSP boot steps ----------------------------------------------------------
NTSTATUS AvsDspCoreResetStall(_In_ PAVS_DEVICE_CONTEXT ctx, ULONG coreMask);
NTSTATUS AvsDspReleaseStallAndWaitRom(_In_ PAVS_DEVICE_CONTEXT ctx, ULONG coreMask);
NTSTATUS AvsCldmaPrepare(_In_ PAVS_DEVICE_CONTEXT ctx);
NTSTATUS AvsCldmaTriggerAndWaitBasefw(_In_ PAVS_DEVICE_CONTEXT ctx);
VOID     AvsCldmaRelease(_In_ PAVS_DEVICE_CONTEXT ctx);
// int_control(false) + stall/reset/power-off of @coreMask (avs_dsp_core_disable)
VOID     AvsDspShutdown(_In_ PAVS_DEVICE_CONTEXT ctx, ULONG coreMask);

// ---- IPC transport (send + wait) ------------------------------------------
VOID AvsIpcInitInfra(_In_ PAVS_DEVICE_CONTEXT ctx);
VOID AvsIpcOnReplyFromIsr(_In_ PAVS_DEVICE_CONTEXT ctx, _In_ ULONG Hipct, _In_ ULONG Hipcte);
VOID AvsIpcOnFwReadyFromIsr(_In_ PAVS_DEVICE_CONTEXT ctx);
// Full transport used by the ported core: payload out, reply payload in.
NTSTATUS AvsIpcSendRaw(
    _In_ PAVS_DEVICE_CONTEXT ctx,
    _In_ ULONG Primary,
    _In_ ULONG Extension,
    _In_reads_bytes_opt_(PayloadSize) const UCHAR* Payload,
    _In_ ULONG PayloadSize,
    _In_ ULONG RxExpect,
    _Out_writes_bytes_opt_(AVS_MAILBOX_SIZE) PUCHAR RxBuf,
    _Out_opt_ PULONG RxSize,
    _In_ ULONG TimeoutMs,
    _Out_ PULONG ReplyPrimary,
    _Out_ PULONG ReplyExtension);
NTSTATUS AvsIpcSendWithPayloadAndWait(
    _In_ PAVS_DEVICE_CONTEXT ctx,
    _In_ ULONG Primary,
    _In_ ULONG Extension,
    _In_reads_bytes_(PayloadSize) PUCHAR Payload,
    _In_ ULONG PayloadSize,
    _In_ ULONG TimeoutMs,
    _Out_ PULONG ReplyPrimary,
    _Out_ PULONG ReplyExtension);

// ---- SRAM mailbox payload access -------------------------------------------
NTSTATUS AvsMailboxReadUplink(
    _In_ PAVS_DEVICE_CONTEXT ctx,
    _Out_writes_bytes_(Size) PUCHAR Buffer,
    _In_ ULONG Size);
NTSTATUS AvsMailboxWriteDownlink(
    _In_ PAVS_DEVICE_CONTEXT ctx,
    _In_reads_bytes_(Size) PUCHAR Buffer,
    _In_ ULONG Size);

// ---- NHLT parsing -----------------------------------------------------------
VOID AvsNhltDumpEndpoints(_In_ PAVS_DEVICE_CONTEXT ctx);
NTSTATUS AvsNhltFindFormatConfig(
    _In_ PAVS_DEVICE_CONTEXT ctx,
    _In_ UCHAR LinkType,
    _In_ UCHAR Direction,
    _In_ UCHAR VirtualBusId,
    _In_ ULONG NumChannels,
    _In_ ULONG SamplingFreq,
    _In_ ULONG ValidBitDepth,
    _In_ ULONG BitDepth,
    _Out_ PUCHAR* OutCapsPtr,
    _Out_ ULONG* OutCapsSize);

// ---- Config file (tunable values without recompiling) ----------------------
// AvsConfigReadUlong() is declared in avs_api.h (the C++ side uses it too).
// Copies Key's value (up to first whitespace) into Buf. FALSE if missing.
BOOLEAN AvsConfigReadString(_In_ PCSTR Key, _Out_writes_(BufLen) PCHAR Buf, _In_ ULONG BufLen);
// Reads a whole file into a new non-paged buffer (free with AvsFreeFileBuffer).
NTSTATUS AvsReadFileToBuffer(_In_ PCWSTR Path, _In_ ULONG MaxSize, _Out_ PVOID* Buffer, _Out_ PULONG Size);
VOID AvsFreeFileBuffer(_In_opt_ PVOID Buffer);

// ---- Ported Linux AVS core glue (avs_port_win.c) ---------------------------
// Init: core create, topologies, SSP link power, volume. Endpoints: fills
// ctx->Endpoints from avswin.cfg (needs an initialized core). Shutdown:
// destroys the core. Suspend/Resume: tear down / rebuild open streams.
// All with CoreLock held by the caller.
NTSTATUS AvsPortInit(_In_ PAVS_DEVICE_CONTEXT ctx);
VOID     AvsPortDiscoverEndpoints(_In_ PAVS_DEVICE_CONTEXT ctx);
VOID     AvsPortShutdown(_In_ PAVS_DEVICE_CONTEXT ctx);
VOID     AvsPortSuspendStreams(_In_ PAVS_DEVICE_CONTEXT ctx);
VOID     AvsPortResumeStreams(_In_ PAVS_DEVICE_CONTEXT ctx);
VOID     AvsPortBootBeep(_In_ PAVS_DEVICE_CONTEXT ctx);

// ---- headphone jack (avs_jack.c) --------------------------------------------
VOID     AvsJackInit(_In_ PAVS_DEVICE_CONTEXT ctx);      // at create
NTSTATUS AvsJackStart(_In_ PAVS_DEVICE_CONTEXT ctx);     // after endpoints exist
VOID     AvsJackStop(_In_ PAVS_DEVICE_CONTEXT ctx);

// ---- HDA multi-link (BAR0) power sequencing --------------------------------
NTSTATUS AvsHdaLinkPowerUp(_In_ PAVS_DEVICE_CONTEXT ctx, _In_ ULONG LinkIndex);

// ---- HDA host stream via sklhdaudbus (avs_hdastream.c) ---------------------
// Open: GetRenderStream in the FE format, PrepareDSP, BDL -> our buffer.
// Fmt->Capture selects GetCaptureStream instead of GetRenderStream.
NTSTATUS AvsHostStreamOpen(
    _In_ PAVS_DEVICE_CONTEXT ctx,
    _Out_ PAVS_HOST_STREAM hs,
    _In_ const AVS_EP_INFO* Fmt,
    _In_ PHYSICAL_ADDRESS BufferPa,
    _In_ ULONG BufferSize,
    _In_ INT Frags);
VOID  AvsHostStreamTrigger(_In_ PAVS_DEVICE_CONTEXT ctx, _In_ PAVS_HOST_STREAM hs, _In_ BOOLEAN Start);
ULONG AvsHostStreamPosition(_In_ PAVS_DEVICE_CONTEXT ctx, _In_ PAVS_HOST_STREAM hs);
VOID  AvsHostStreamClose(_In_ PAVS_DEVICE_CONTEXT ctx, _Inout_ PAVS_HOST_STREAM hs);
// Read-only register dump (SDn found by tag) + LPIB/posbuf sampling.
VOID  AvsHostStreamDumpRegs(_In_ PAVS_DEVICE_CONTEXT ctx, _In_ PAVS_HOST_STREAM hs, _In_ PCSTR Label);
VOID  AvsHostStreamProbePosition(_In_ PAVS_DEVICE_CONTEXT ctx, _In_ PAVS_HOST_STREAM hs, _In_ PCSTR Label);
// Fills a buffer with a 440 Hz sine (diagnostic beep).
VOID  AvsFillTestTone(_Out_writes_bytes_(Size) PUCHAR Buffer, _In_ ULONG Size, _In_ ULONG SampleRate,
                      _In_ ULONG ContainerBits, _In_ ULONG NumChannels);

// ---- Generic hex dump (IRQL-safe, uses only DbgPrintEx) ----------------------
VOID AvsDumpHex(_In_ PCSTR Label, _In_reads_bytes_(Size) const UCHAR* Buf, _In_ ULONG Size);

// ---- CSAudio broadcast protocol (wake CoolStar codec drivers) -------------
NTSTATUS AvsCsAudioRegisterDsp(_In_ PAVS_DEVICE_CONTEXT ctx);
// Start/stop broadcast for one endpoint type (CSAudioEndpointType*).
NTSTATUS AvsCsAudioEndpointStartStop(_In_ PAVS_DEVICE_CONTEXT ctx, _In_ ULONG EndpointType, _In_ BOOLEAN Start);
