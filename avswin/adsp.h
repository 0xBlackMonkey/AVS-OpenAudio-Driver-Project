// SPDX-License-Identifier: BSD-3-Clause
//
// adsp.h -- ADSP_BUS_INTERFACE contract of sklhdaudbus.sys.
//
// Copied verbatim from coolstar/sklhdaudbus (https://github.com/coolstar/sklhdaudbus).
// This is the public query-interface contract the bus driver exposes to
// its ADSP child device (CSAUDIO\ADSP&CTLR_VEN_xxxx&CTLR_DEV_xxxx); our
// function driver queries GUID_ADSP_BUS_INTERFACE on that PDO.
//
// Its license, reproduced as it requires:
//
// Copyright 2024 CoolStar
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its
//    contributors may be used to endorse or promote products derived from
//    this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#ifndef __ADSP_INTERFACE
#define __ADSP_INTERFACE

//
// The ADSP_BUS_INTERFACE interface GUID
//
// {752A2CAE-3455-4D18-A184-8B34B22632CE}
DEFINE_GUID(GUID_ADSP_BUS_INTERFACE,
    0x752a2cae, 0x3455, 0x4d18, 0xa1, 0x84, 0x8b, 0x34, 0xb2, 0x26, 0x32, 0xce);

typedef struct _NHLT_INFO {
    PVOID nhlt;
    UINT64 nhltSz;
} NHLT_INFO, * PNHLT_INFO;

typedef _Must_inspect_result_ NTSTATUS(*PGET_ADSP_RESOURCES) (_In_ PVOID _context, _Out_ struct _PCI_BAR* hdaBar, _Out_ struct _PCI_BAR* adspBar, PVOID* ppcap, PNHLT_INFO nhltInfo, _Out_ BUS_INTERFACE_STANDARD* pciConfig);
typedef _Must_inspect_result_ NTSTATUS(*PDSP_SET_POWER_STATE) (_In_ PVOID _context, _In_ DEVICE_POWER_STATE newPowerState);
typedef _Must_inspect_result_ BOOL(*PADSP_INTERRUPT_CALLBACK)(PVOID context);
typedef _Must_inspect_result_ NTSTATUS(*PREGISTER_ADSP_INTERRUPT) (_In_ PVOID _context, _In_ PADSP_INTERRUPT_CALLBACK callback, _In_ PVOID callbackContext);
typedef _Must_inspect_result_ NTSTATUS(*PUNREGISTER_ADSP_INTERRUPT) (_In_ PVOID _context);
typedef _Must_inspect_result_ NTSTATUS(*PGET_STREAM)(_In_ PVOID _context, HDAUDIO_STREAM_FORMAT StreamFormat, PHANDLE Handle, _Out_ UINT8* streamTag);
typedef _Must_inspect_result_ NTSTATUS(*PFREE_STREAM)(_In_ PVOID _context, _In_ HANDLE Handle);
typedef _Must_inspect_result_ NTSTATUS(*PDSP_PREPARE_STREAM)(_In_ PVOID _context, _In_ HANDLE Handle, _In_ unsigned int ByteSize, _In_ int frags, _Out_ PVOID* bdlBuf);
typedef _Must_inspect_result_ NTSTATUS(*PDSP_CLEANUP_STREAM)(_In_ PVOID _context, _In_ HANDLE Handle);
typedef _Must_inspect_result_ UINT32(*PDSP_STREAM_POSITION)(_In_ PVOID _context, _In_ HANDLE Handle);
typedef void (*PDSP_START_STOP_STREAM)(_In_ PVOID _context, _In_ HANDLE Handle, BOOL startStop);
typedef void (*PDSP_ENABLE_SPIB)(_In_ PVOID _context, _In_ HANDLE Handle, UINT32 value);
typedef void (*PDSP_DISABLE_SPIB)(_In_ PVOID _context, _In_ HANDLE Handle);

typedef struct _ADSP_BUS_INTERFACE
{
    USHORT                    Size;
    USHORT                    Version;
    PVOID                     Context;
    PINTERFACE_REFERENCE      InterfaceReference;
    PINTERFACE_DEREFERENCE    InterfaceDereference;

    UINT16 CtlrDevId;
    PGET_ADSP_RESOURCES           GetResources;
    PDSP_SET_POWER_STATE          SetDSPPowerState;
    PREGISTER_ADSP_INTERRUPT      RegisterInterrupt;
    PUNREGISTER_ADSP_INTERRUPT    UnregisterInterrupt;
    PGET_STREAM                   GetRenderStream;
    PGET_STREAM                   GetCaptureStream;
    PFREE_STREAM                  FreeStream;
    PDSP_PREPARE_STREAM           PrepareDSP;
    PDSP_CLEANUP_STREAM           CleanupDSP;
    PDSP_START_STOP_STREAM        TriggerDSP;
    PDSP_STREAM_POSITION          StreamPosition;

    PDSP_ENABLE_SPIB              DSPEnableSPIB;
    PDSP_DISABLE_SPIB             DSPDisableSPIB;
} ADSP_BUS_INTERFACE, * PADSP_BUS_INTERFACE;

#ifndef ADSP_DECL
ADSP_BUS_INTERFACE ADSP_BusInterface(PVOID Context);
#endif
#endif
