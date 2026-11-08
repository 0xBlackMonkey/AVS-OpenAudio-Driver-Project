// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 AVS OpenAudio Driver Project contributors
//
// avs_api.h -- the interface between the PortCls/WaveRT layer (C++:
// adapter.cpp, minwavert.cpp, mintopo.cpp) and the C driver underneath
// (DSP boot, IPC, ported Linux core, HDA host streams).
//
// Kernel base types only (wdm.h/ntddk.h), so the C++ side never includes
// driver.h and the C side never sees portcls.h.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _AVS_DEVICE_CONTEXT* PAVS_DEVICE;
typedef struct _AVS_RT_STREAM* PAVS_RT_STREAM;

#define AVS_MAX_ENDPOINTS      4

// Endpoint kinds (avswin.cfg: Endpoint<n>Type=speaker|headphones|mic|headsetmic)
#define AVS_EP_TYPE_SPEAKER    0
#define AVS_EP_TYPE_HEADPHONE  1
#define AVS_EP_TYPE_MIC        2    // built-in microphones (DMIC)
#define AVS_EP_TYPE_HEADSET_MIC 3   // microphone of a plugged headset

#define AVS_EP_IS_CAPTURE(t)   ((t) == AVS_EP_TYPE_MIC || (t) == AVS_EP_TYPE_HEADSET_MIC)
#define AVS_EP_HAS_JACK(t)     ((t) == AVS_EP_TYPE_HEADPHONE || (t) == AVS_EP_TYPE_HEADSET_MIC)

typedef struct _AVS_EP_INFO
{
    ULONG Type;
    BOOLEAN Capture;            // TRUE: recording endpoint (FE is a capture path)
    ULONG Rate;
    ULONG Channels;
    ULONG ContainerBits;
    ULONG ValidBits;
    CHAR  FeName[64];
} AVS_EP_INFO, *PAVS_EP_INFO;

// Same numbering as KSSTATE (STOP=0, ACQUIRE=1, PAUSE=2, RUN=3).
#define AVS_RT_STATE_STOP      0
#define AVS_RT_STATE_ACQUIRE   1
#define AVS_RT_STATE_PAUSE     2
#define AVS_RT_STATE_RUN       3

// ---- device (avs_boot.c) ----------------------------------------------------
// Create: allocation only. Start: query ADSP interface from the bus driver,
// boot the DSP, bring up the ported core, discover endpoints. Stop: undo
// Start. Destroy: free. All PASSIVE_LEVEL.
NTSTATUS AvsDeviceCreate(_In_ PDEVICE_OBJECT Fdo, _In_ PDEVICE_OBJECT Pdo, _Out_ PAVS_DEVICE* Out);
NTSTATUS AvsDeviceStart(_In_ PAVS_DEVICE Dev);
VOID     AvsDeviceStop(_In_ PAVS_DEVICE Dev);
VOID     AvsDeviceDestroy(_In_ PAVS_DEVICE Dev);
VOID     AvsDeviceSetPower(_In_ PAVS_DEVICE Dev, _In_ DEVICE_POWER_STATE NewState);

// FALSE if endpoint @Index is not configured / its FE was not found.
BOOLEAN  AvsDeviceGetEndpoint(_In_ PAVS_DEVICE Dev, _In_ ULONG Index, _Out_ PAVS_EP_INFO Info);

// ---- jack presence (avs_jack.c) ----------------------------------------------
// TRUE for endpoints whose connector state is actually reported (headphones,
// once the da7219 HID channel is up).
BOOLEAN  AvsDeviceHasJackDetect(_In_ PAVS_DEVICE Dev, _In_ ULONG Endpoint);
// Speakers: always TRUE. Headphones: last state from da7219.sys (TRUE while
// still unknown, so the endpoint never disappears just because nothing
// reported yet).
BOOLEAN  AvsDeviceIsJackConnected(_In_ PAVS_DEVICE Dev, _In_ ULONG Endpoint);
// Called (PASSIVE/APC level) whenever that endpoint's jack state changes.
// Fn = NULL unregisters; returns only after a running callback finished.
typedef VOID (*AVS_JACK_NOTIFY)(_In_ PVOID Context);
VOID     AvsDeviceRegisterJackNotify(_In_ PAVS_DEVICE Dev, _In_ ULONG Endpoint,
                                     _In_opt_ AVS_JACK_NOTIFY Fn, _In_opt_ PVOID Context);

// ---- one WaveRT render stream (avs_port_win.c) ---------------------------
NTSTATUS AvsRtStreamCreate(_In_ PAVS_DEVICE Dev, _In_ ULONG Endpoint, _Out_ PAVS_RT_STREAM* Out);
VOID     AvsRtStreamDestroy(_In_ PAVS_RT_STREAM Stream);
// Cyclic DMA buffer, physically contiguous (Pa = first byte). Size 0 detaches.
VOID     AvsRtStreamSetBuffer(_In_ PAVS_RT_STREAM Stream, _In_ PHYSICAL_ADDRESS Pa, _In_ ULONG Size);
// One-step KS state transitions; the C side drives host DMA + DSP paths.
NTSTATUS AvsRtStreamSetState(_In_ PAVS_RT_STREAM Stream, _In_ ULONG NewState);
// Byte offset of the DMA read position inside the cyclic buffer.
// Callable at <= DISPATCH_LEVEL.
ULONG    AvsRtStreamGetPosition(_In_ PAVS_RT_STREAM Stream);

// ---- avswin.cfg (avs_config.c) -----------------------------------------------
ULONG    AvsConfigReadUlong(_In_ PCSTR Key, _In_ ULONG DefaultValue);

#ifdef __cplusplus
}
#endif
