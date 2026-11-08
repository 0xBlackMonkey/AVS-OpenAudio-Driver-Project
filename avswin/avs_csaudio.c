// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 AVS OpenAudio Driver Project contributors
//
// avs_csaudio.c -- CSAudio broadcast protocol client.
//
// CoolStar's codec drivers (opengmaxcodec.sys for MAX98927/MAX98373,
// and presumably da7219.sys) don't power up/unmute their amplifier
// or output stage on their own -- they wait for a broadcast from the
// DSP-side driver telling them a stream is active. That protocol is
// NOT a Windows-standard mechanism (not WaveRT/KS) -- it's a private
// CoolStar convention built on the standard WDM Executive Callback
// Object API (ExCreateCallback/ExNotifyCallback), confirmed entirely
// from opengmaxcodec.c's own open-source receiving side:
//
//   UNICODE_STRING CSAudioCallbackAPI;
//   RtlInitUnicodeString(&CSAudioCallbackAPI, L"\\CallBack\\CsAudioCallbackAPI");
//   ExCreateCallback(&pDevice->CSAudioAPICallback, &attributes, TRUE, TRUE);
//   ExRegisterCallback(pDevice->CSAudioAPICallback, CsAudioCallbackFunction, pDevice);
//
//   VOID CsAudioCallbackFunction(IN PGMAX_CONTEXT pDevice, CsAudioArg* arg, PVOID Argument2)
//   {
//       if (Argument2 == &CsAudioArg2) return; // ignore our own broadcasts
//       CsAudioArg localArg;
//       RtlCopyMemory(&localArg, arg, min(arg->argSz, sizeof(CsAudioArg)));
//       if (localArg.endpointType == CSAudioEndpointTypeDSP &&
//           localArg.endpointRequest == CSAudioEndpointRegister) { CSAudioRegisterEndpoint(pDevice); }
//       else if (localArg.endpointType != CSAudioEndpointTypeSpeaker) { return; }
//       if (localArg.endpointRequest == CSAudioEndpointStop) { ... WdfDeviceResumeIdle ... }
//       if (localArg.endpointRequest == CSAudioEndpointStart) { ... WdfDeviceStopIdle ... }
//   }
//
// This is a single, SYSTEM-WIDE named object (not per-device) --
// every CSAudio-family codec driver registers against the same
// object, and any driver (including ours) can open it (OBJ_OPENIF)
// and broadcast via ExNotifyCallback, which fans out to every
// registered receiver.
//
// Numeric values, confirmed from opengmaxcodec.h (github.com/coolstar/
// opengmaxcodec): endpointType DSP=0, Speaker=1, Headphone=2, MicArray=3,
// MicJack=4; endpointRequest Register=0, Start=1, Stop=2,
// OverrideFormat=3. Still read from avswin.cfg (same defaults) so other
// codec drivers can be accommodated without recompiling.

#include "driver.h"

#define AVS_CSAUDIO_CALLBACK_NAME  L"\\CallBack\\CsAudioCallbackAPI"

// struct CsAudioArg -- confirmed fields (argSz, endpointType,
// endpointRequest) and their order from "RtlCopyMemory(&localArg,
// arg, min(arg->argSz, sizeof(CsAudioArg)))" and the field-by-field
// comparisons in CsAudioCallbackFunction. Field TYPES (ULONG each)
// and whether there are additional trailing fields are assumed, not
// confirmed -- argSz-bounded copy on the receiving side means an
// oversized guess here is safe (receiver never reads past its own
// sizeof), but an UNDERSIZED ArgSz could truncate fields the real
// struct expects, so ArgSz is set to our own full struct size.
typedef struct _AVS_CSAUDIO_ARG
{
    ULONG ArgSz;
    ULONG EndpointType;
    ULONG EndpointRequest;
} AVS_CSAUDIO_ARG;

// Opens (or creates, if no codec driver has registered yet -- order
// doesn't matter thanks to OBJ_OPENIF) the shared CSAudio callback
// object and broadcasts one EndpointType/EndpointRequest pair to
// every registered codec driver instance.
static NTSTATUS
AvsCsAudioNotify(
    _In_ ULONG EndpointType,
    _In_ ULONG EndpointRequest
    )
{
    UNICODE_STRING callbackName;
    RtlInitUnicodeString(&callbackName, AVS_CSAUDIO_CALLBACK_NAME);

    OBJECT_ATTRIBUTES attrs;
    InitializeObjectAttributes(&attrs, &callbackName,
        OBJ_KERNEL_HANDLE | OBJ_OPENIF | OBJ_CASE_INSENSITIVE | OBJ_PERMANENT,
        NULL, NULL);

    PCALLBACK_OBJECT callbackObj = NULL;
    NTSTATUS status = ExCreateCallback(&callbackObj, &attrs, TRUE, TRUE);
    if (!NT_SUCCESS(status) || callbackObj == NULL)
    {
        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "AVS: CSAudio -- ExCreateCallback failed: 0x%X\n", status));
        return NT_SUCCESS(status) ? STATUS_UNSUCCESSFUL : status;
    }

    AVS_CSAUDIO_ARG arg;
    arg.ArgSz = sizeof(arg);
    arg.EndpointType = EndpointType;
    arg.EndpointRequest = EndpointRequest;

    // Unique-ish marker, mirroring the self-ignore convention seen in
    // opengmaxcodec.c ("if (Argument2 == &CsAudioArg2) return;") --
    // harmless for us since we don't register our own receiver on
    // this object, but costs nothing to follow the same convention.
    static ULONG selfMarker;

    AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "AVS: CSAudio -- notifying endpointType=%lu endpointRequest=%lu\n",
        EndpointType, EndpointRequest));

    ExNotifyCallback(callbackObj, &arg, &selfMarker);

    ObDereferenceObject(callbackObj);

    return STATUS_SUCCESS;
}

// Broadcasts DSP registration -- confirmed as the trigger for
// CSAudioRegisterEndpoint() on the receiving side. Call once, early
// (this driver's equivalent of the real csaudiosstavs/csaudiointcsof
// announcing itself).
NTSTATUS
AvsCsAudioRegisterDsp(
    _In_ PAVS_DEVICE_CONTEXT ctx
    )
{
    UNREFERENCED_PARAMETER(ctx);

    ULONG typeDsp = AvsConfigReadUlong("CSAudioEndpointTypeDSP", 0);
    ULONG requestRegister = AvsConfigReadUlong("CSAudioEndpointRegister", 0);

    return AvsCsAudioNotify(typeDsp, requestRegister);
}

// Broadcasts stream start/stop for one endpoint type. opengmaxcodec only
// reacts to CSAudioEndpointTypeSpeaker; the headphone codec driver's
// type value comes from avswin.cfg per endpoint (Endpoint<n>CsAudioType).
NTSTATUS
AvsCsAudioEndpointStartStop(
    _In_ PAVS_DEVICE_CONTEXT ctx,
    _In_ ULONG EndpointType,
    _In_ BOOLEAN Start
    )
{
    return AvsCsAudioNotify(EndpointType, Start ? ctx->CfgCsAudioStart : ctx->CfgCsAudioStop);
}
