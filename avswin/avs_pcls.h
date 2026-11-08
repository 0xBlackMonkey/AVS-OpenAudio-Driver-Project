// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 AVS OpenAudio Driver Project contributors
//
// avs_pcls.h -- common header of the PortCls/WaveRT layer (C++).
//
// Per endpoint the adapter registers two subdevices, the classic WaveRT
// layout. Render (speakers, headphones):
//
//   "Wave<n>" (IMiniportWaveRT)               "Topo<n>" (IMiniportTopology)
//   pin 0: render sink (PCM)                   pin 0: bridge from wave ---.
//      -> DAC node -> pin 1: bridge out  ---- physical ----^             |
//                                                    pin 1: jack out (speaker /
//                                                           headphones category)
// Capture (microphones) is the mirror image: topology pin 1 = microphone
// input -> pin 0 = bridge out -> physical -> wave pin 1 bridge in -> ADC
// node -> wave pin 0 = capture source (PCM out to the audio engine).
//
// No volume/mute nodes: Windows then applies the endpoint volume in
// software. The DSP gain is fixed (avswin.cfg DspVolumePercent).

#pragma once

// We provide kernel new/delete ourselves (adapter.cpp); stdunk.h's own
// versions are skipped under this define.
#define _NEW_DELETE_OPERATORS_

extern "C" {
#include <wdm.h>
}
#include <portcls.h>
#include <stdunk.h>
#include <ksmedia.h>
#include <ntstrsafe.h>

#include "avs_api.h"

#define AVS_PCLS_POOL_TAG       'lPvA'

// Always-on log of the PortCls layer (DbgPrintEx works in every build).
#define AVS_LOG(...)  DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, __VA_ARGS__)

// Wave filter pins (both directions)
#define AVS_WAVE_PIN_STREAM     0   // PCM pin the audio engine connects to
#define AVS_WAVE_PIN_BRIDGE     1   // analog bridge to the topology filter
// Topology filter pins (both directions)
#define AVS_TOPO_PIN_WAVE       0   // bridge to the wave filter
#define AVS_TOPO_PIN_JACK       1   // the physical connector (speaker/jack/mic)

// Kernel-mode C++ allocation (no exceptions; returns NULL on failure).
PVOID __cdecl operator new(size_t Size, POOL_FLAGS Flags, ULONG Tag);
// operator delete(PVOID) itself is provided by stdunk.lib
void __cdecl operator delete(PVOID Ptr, size_t Size);
void __cdecl operator delete[](PVOID Ptr);
void __cdecl operator delete[](PVOID Ptr, size_t Size);

NTSTATUS AvsCreateMiniportWaveRT(_Out_ PUNKNOWN* Unknown, _In_ PAVS_DEVICE Dev, _In_ ULONG Endpoint);
NTSTATUS AvsCreateMiniportTopology(_Out_ PUNKNOWN* Unknown, _In_ PAVS_DEVICE Dev, _In_ ULONG Endpoint);
