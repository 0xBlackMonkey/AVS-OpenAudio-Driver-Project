// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 AVS OpenAudio Driver Project contributors
//
// avs_boot.c -- device bring-up underneath PortCls (WDM).
//
//   AvsDeviceStart : IRP_MN_QUERY_INTERFACE for GUID_ADSP_BUS_INTERFACE,
//                    GetResources, IPC interrupt, DSP boot, ported core,
//                    endpoints, jack detection
//   AvsDeviceStop  : the reverse, DSP left powered off
//   AvsDeviceSetPower : D3 = streams + core down, DSP off; D0 = full reboot
// Called from adapter.cpp (StartDevice, PnP stop/remove,
// IAdapterPowerManagement).

#include "driver.h"

// Exported by ntoskrnl, but declared only in ntifs.h (not ntddk.h/wdm.h).
NTKERNELAPI
PDEVICE_OBJECT
IoGetLowerDeviceObject(
    _In_ PDEVICE_OBJECT DeviceObject
    );

// ------------------------------------------------------------------ ISR

BOOL
AvsAdspInterruptCallback(
    _In_ PVOID Context
    )
{
    // Called by sklhdaudbus at its own interrupt context whenever
    // ADSPIS reports something pending for us. Only register access
    // here; anything that needs IRQL <= DISPATCH_LEVEL (KeSetEvent) is
    // deferred to DPCs by AvsIpcOnReplyFromIsr/AvsIpcOnFwReadyFromIsr.
    PAVS_DEVICE_CONTEXT ctx = (PAVS_DEVICE_CONTEXT)Context;

    if (ctx == NULL || ctx->DspBar.Base.Base == NULL)
    {
        return FALSE;
    }

    PUCHAR dspBar = ctx->DspBar.Base.baseptr;
    ULONG adspis = READ_REGISTER_ULONG((PULONG)(dspBar + AVS_ADSP_REG_ADSPIS));
    BOOLEAN handled = FALSE;

    if (adspis & AVS_ADSPIS_IPC)
    {
        // Mask further IPC-source interrupts while we process this one
        // -- confirmed from upstream avs_skl_ipc_interrupt() (skl.c) /
        // avs_cnl_ipc_interrupt() (cnl.c), both of which disable
        // HIPCCTL.DONE|BUSY on entry and restore them on exit. Guards
        // against a race between clearing the current HIPCT/HIPCIE
        // condition and a new one arriving mid-handler.
        PULONG hipcctlReg = (PULONG)(dspBar + SKL_ADSP_REG_HIPCCTL);
        WRITE_REGISTER_ULONG(hipcctlReg, 0);

        ULONG hipct = READ_REGISTER_ULONG((PULONG)(dspBar + SKL_ADSP_REG_HIPCT));

        if (hipct & SKL_ADSP_HIPCT_BUSY)
        {
            ULONG hipcte = READ_REGISTER_ULONG((PULONG)(dspBar + SKL_ADSP_REG_HIPCTE));
            ULONG content = hipct & 0x7FFFFFFFu; // strip hardware BUSY bit

            ULONG globalMsgType = (content >> 24) & 0x1Fu;
            ULONG notifyMsgType = (content >> 16) & 0xFFu;

            if (globalMsgType == 27 /* AVS_GLB_NOTIFICATION */)
            {
                if (notifyMsgType == 8 /* AVS_NOTIFY_FW_READY */)
                {
                    ctx->DspFirmwareLoaded = TRUE;
                    AvsIpcOnFwReadyFromIsr(ctx);
                    AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                        "AVS: [ISR] FW_READY notification acked (HIPCT=0x%08X HIPCTE=0x%08X)\n",
                        hipct, hipcte));
                }
                else if (notifyMsgType == AVS_NOTIFY_RESOURCE_EVENT)
                {
                    // Payload (struct avs_notify_res_data, 40 bytes) sits in
                    // the uplink mailbox -- must be read BEFORE the HIPCT ack
                    // below. READ_REGISTER_BUFFER_* is IRQL-safe.
                    //
                    // Decode tables below are from memory of SOF IPC4
                    // notification.h (same cAVS firmware family) -- the RAW
                    // numbers are what count; names are a hint to verify:
                    //   resource_type: 0 module instance, 1 pipeline,
                    //                  2 gateway, 3 EDF task
                    //   event_type:    0 budget violation, 1 mixer underrun,
                    //                  2 stream data segment, 3 process data err,
                    //                  4 stack overflow, 5 buffering mode chg,
                    //                  6 gateway underrun, 7 gateway overrun,
                    //                  8 EDF domain unstable, 9 watchdog expired
                    AVS_NOTIFY_RES_DATA res;
                    RtlZeroMemory(&res, sizeof(res));
                    AvsMailboxReadUplink(ctx, (PUCHAR)&res, sizeof(res));

                    static const char* const resTypeNames[] = {
                        "module", "pipeline", "gateway", "edf-task" };
                    static const char* const evtNames[] = {
                        "budget-violation", "mixer-underrun", "stream-data-segment",
                        "process-data-error", "stack-overflow", "buffering-mode-changed",
                        "GATEWAY-UNDERRUN", "GATEWAY-OVERRUN", "edf-unstable",
                        "watchdog-expired" };

                    AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                        "AVS: [ISR] RESOURCE_EVENT (HIPCT=0x%08X HIPCTE=0x%08X): "
                        "resource_type=%lu(%s) resource_id=0x%08X event_type=%lu(%s) reserved=0x%08X\n",
                        hipct, hipcte,
                        res.ResourceType, (res.ResourceType < 4) ? resTypeNames[res.ResourceType] : "?",
                        res.ResourceId,
                        res.EventType, (res.EventType < 10) ? evtNames[res.EventType] : "?",
                        res.Reserved));
                    AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                        "AVS: [ISR] RESOURCE_EVENT data = %08X %08X %08X %08X %08X %08X\n",
                        res.Data[0], res.Data[1], res.Data[2],
                        res.Data[3], res.Data[4], res.Data[5]));
                }
                else
                {
                    AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                        "AVS: [ISR] notification type=%lu (HIPCT=0x%08X HIPCTE=0x%08X)\n",
                        notifyMsgType, hipct, hipcte));
                }
            }
            else
            {
                // A reply to a request we sent -- hand it to the IPC
                // transport layer, which defers KeSetEvent to a DPC
                // (this callback runs at the ISR's real IRQL, where
                // KeSetEvent is not safe to call directly).
                AvsIpcOnReplyFromIsr(ctx, hipct, hipcte);
                if (ctx->IpcDumpPayloads)
                {
                    AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                        "AVS: [ISR] reply, global_type=%lu (HIPCT=0x%08X HIPCTE=0x%08X)\n",
                        globalMsgType, hipct, hipcte));
                }
            }

            // Acknowledge: clear BUSY by writing the value back.
            WRITE_REGISTER_ULONG((PULONG)(dspBar + SKL_ADSP_REG_HIPCT), hipct);
            handled = TRUE;
        }

        ULONG hipcie = READ_REGISTER_ULONG((PULONG)(dspBar + SKL_ADSP_REG_HIPCIE));
        if (hipcie & SKL_ADSP_HIPCIE_DONE)
        {
            // DSP acknowledged a message WE sent (the "our request was
            // received" handshake -- distinct from the actual reply
            // content, which arrives separately via HIPCT above).

            // Acknowledge: clear DONE by writing it back (write-1-to-clear),
            // same idiom as the HIPCT_BUSY clear above. Without this the
            // DONE condition never de-asserts and the interrupt storms.
            WRITE_REGISTER_ULONG((PULONG)(dspBar + SKL_ADSP_REG_HIPCIE), hipcie);
            handled = TRUE;
        }

        // Restore the local IPC interrupt gates we masked on entry.
        WRITE_REGISTER_ULONG(hipcctlReg, AVS_ADSP_HIPCCTL_BUSY | AVS_ADSP_HIPCCTL_DONE);
    }

    return handled ? TRUE : FALSE;
}

// ------------------------------------------------------- bus interface

// IRP_MN_QUERY_INTERFACE down our own stack (the PDO is sklhdaudbus's
// child) -- the WDM equivalent of KMDF's WdfFdoQueryForInterface.
static NTSTATUS
AvsQueryAdspInterface(
    _In_ PAVS_DEVICE_CONTEXT ctx
    )
{
    KEVENT event;
    IO_STATUS_BLOCK iosb;
    PIRP irp;
    PIO_STACK_LOCATION stack;
    PDEVICE_OBJECT target;
    NTSTATUS status;

    // Whatever sits directly below our FDO (the PDO, or a lower filter).
    target = IoGetLowerDeviceObject(ctx->Fdo);   // referenced
    if (target == NULL)
    {
        return STATUS_NO_SUCH_DEVICE;
    }

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    RtlZeroMemory(&iosb, sizeof(iosb));

    irp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP, target, NULL, 0, NULL, &event, &iosb);
    if (irp == NULL)
    {
        ObDereferenceObject(target);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    irp->IoStatus.Status = STATUS_NOT_SUPPORTED;   // required for PnP IRPs
    stack = IoGetNextIrpStackLocation(irp);
    stack->MajorFunction = IRP_MJ_PNP;
    stack->MinorFunction = IRP_MN_QUERY_INTERFACE;
    stack->Parameters.QueryInterface.InterfaceType = &GUID_ADSP_BUS_INTERFACE;
    stack->Parameters.QueryInterface.Size = sizeof(ADSP_BUS_INTERFACE);
    stack->Parameters.QueryInterface.Version = 1;
    stack->Parameters.QueryInterface.Interface = (PINTERFACE)&ctx->AdspInterface;
    stack->Parameters.QueryInterface.InterfaceSpecificData = NULL;

    RtlZeroMemory(&ctx->AdspInterface, sizeof(ctx->AdspInterface));

    status = IoCallDriver(target, irp);
    if (status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = iosb.Status;
    }
    ObDereferenceObject(target);

    if (NT_SUCCESS(status) && ctx->AdspInterface.GetResources == NULL)
    {
        status = STATUS_NOT_SUPPORTED;
    }
    return status;
}

// ------------------------------------------------------------ DSP boot

// Core reset/stall -> CLDMA prepare -> release stall, wait ROM -> basefw
// transfer -> FW_READY. Order from upstream avs_cldma_load_basefw() /
// avs_dsp_load_basefw().
static NTSTATUS
AvsDspBoot(
    _In_ PAVS_DEVICE_CONTEXT ctx
    )
{
    NTSTATUS status;
    LARGE_INTEGER timeout;

    AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "AVS: DSP boot, ADSPCS = 0x%08X\n",
        READ_REGISTER_ULONG((PULONG)(ctx->DspBar.Base.baseptr + AVS_ADSP_REG_ADSPCS))));

    // upstream: reinit_completion(&adev->fw_ready) right before the load
    KeClearEvent(&ctx->FwReadyEvent);
    ctx->DspFirmwareLoaded = FALSE;

    status = AvsDspCoreResetStall(ctx, AVS_MAIN_CORE_MASK);
    if (!NT_SUCCESS(status))
    {
        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "AVS: core reset/stall sequence failed: 0x%X\n", status));
        return status;
    }

    status = AvsCldmaPrepare(ctx);
    if (!NT_SUCCESS(status))
    {
        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "AVS: CLDMA prepare failed: 0x%X\n", status));
        return status;
    }

    status = AvsDspReleaseStallAndWaitRom(ctx, AVS_MAIN_CORE_MASK);
    if (!NT_SUCCESS(status))
    {
        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "AVS: ROM init wait failed: 0x%X\n", status));
        AvsCldmaRelease(ctx);
        return status;
    }

    status = AvsCldmaTriggerAndWaitBasefw(ctx);
    if (!NT_SUCCESS(status))
    {
        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "AVS: CLDMA basefw transfer failed: 0x%X\n", status));
    }

    // FW_STATUS==BASEFW_ENTERED and the FW_READY IPC notification are two
    // different signals; nothing may be sent before FW_READY (loader.c).
    timeout.QuadPart = -((LONGLONG)AVS_FW_INIT_TIMEOUT_MS * 10000);
    status = KeWaitForSingleObject(&ctx->FwReadyEvent, Executive, KernelMode, FALSE, &timeout);

    AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "AVS: wait for FW_READY notification -- status=0x%X, loaded=%d\n",
        status, ctx->DspFirmwareLoaded));

    AvsCldmaRelease(ctx);

    if (status != STATUS_SUCCESS || !ctx->DspFirmwareLoaded)
    {
        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "AVS: FW_READY notification never arrived within %u ms\n",
            AVS_FW_INIT_TIMEOUT_MS));
        return STATUS_DEVICE_NOT_READY;
    }
    return STATUS_SUCCESS;
}

// DSP boot + ported core, CoreLock held.
static NTSTATUS
AvsDspAndCoreUp(
    _In_ PAVS_DEVICE_CONTEXT ctx
    )
{
    NTSTATUS status = AvsDspBoot(ctx);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    status = AvsPortInit(ctx);
    AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, NT_SUCCESS(status) ? DPFLTR_INFO_LEVEL : DPFLTR_ERROR_LEVEL,
        "AVS: AvsPortInit -> 0x%X\n", status));
    return status;
}

// ----------------------------------------------------------- device API

NTSTATUS
AvsDeviceCreate(
    _In_ PDEVICE_OBJECT Fdo,
    _In_ PDEVICE_OBJECT Pdo,
    _Out_ PAVS_DEVICE* Out
    )
{
    PAVS_DEVICE_CONTEXT ctx;

    *Out = NULL;
    ctx = (PAVS_DEVICE_CONTEXT)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*ctx), AVS_CTX_POOL_TAG);
    if (ctx == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ctx->Fdo = Fdo;
    ctx->Pdo = Pdo;
    ctx->PowerState = PowerDeviceD0;
    KeInitializeMutex(&ctx->CoreLock, 0);
    InitializeListHead(&ctx->StreamList);
    AvsJackInit(ctx);
    AvsIpcInitInfra(ctx);
    if (ctx->IpcRxBuf == NULL)
    {
        ExFreePoolWithTag(ctx, AVS_CTX_POOL_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    *Out = ctx;
    return STATUS_SUCCESS;
}

NTSTATUS
AvsDeviceStart(
    _In_ PAVS_DEVICE Dev
    )
{
    PAVS_DEVICE_CONTEXT ctx = Dev;
    NTSTATUS status;

    if (ctx->HwStarted)
    {
        return STATUS_SUCCESS;
    }

    status = AvsQueryAdspInterface(ctx);
    if (!NT_SUCCESS(status))
    {
        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "AVS: query GUID_ADSP_BUS_INTERFACE failed: 0x%X\n", status));
        return status;
    }
    ctx->AdspInterfaceValid = TRUE;

    RtlZeroMemory(&ctx->Nhlt, sizeof(ctx->Nhlt));
    status = ctx->AdspInterface.GetResources(
        ctx->AdspInterface.Context,
        &ctx->HdaBar,
        &ctx->DspBar,
        &ctx->Ppcap,
        &ctx->Nhlt,
        &ctx->PciConfig);
    if (!NT_SUCCESS(status) || ctx->DspBar.Base.Base == NULL)
    {
        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "AVS: GetResources failed: 0x%X (DspBar %p)\n", status, ctx->DspBar.Base.Base));
        AvsDeviceStop(ctx);
        return NT_SUCCESS(status) ? STATUS_DEVICE_CONFIGURATION_ERROR : status;
    }

    AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "AVS: DspBar mapped at %p (len 0x%lx), NHLT %p (0x%llx bytes)\n",
        ctx->DspBar.Base.baseptr, ctx->DspBar.Len,
        ctx->Nhlt.nhlt, ctx->Nhlt.nhltSz));

    AvsNhltDumpEndpoints(ctx);

    AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "AVS: FW_STATUS before any register write = 0x%08X\n",
        READ_REGISTER_ULONG((PULONG)(ctx->DspBar.Base.baseptr + AVS_FW_REG_STATUS))));

    if (ctx->AdspInterface.RegisterInterrupt != NULL)
    {
        status = ctx->AdspInterface.RegisterInterrupt(
            ctx->AdspInterface.Context, AvsAdspInterruptCallback, ctx);
        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
            "AVS: RegisterInterrupt returned 0x%X\n", status));
        ctx->InterruptRegistered = NT_SUCCESS(status);
    }

    // Settings used on hot paths (stream start/stop), read once here.
    ctx->IpcDumpPayloads = (AvsConfigReadUlong("IpcDumpPayloads", 0) != 0);
    ctx->CfgDebugStreams = (AvsConfigReadUlong("DebugStreams", 0) != 0);
    ctx->CfgHostStreamFrags = AvsConfigReadUlong("HostStreamFrags", 2);
    if (ctx->CfgHostStreamFrags < 2 || ctx->CfgHostStreamFrags > 32)
    {
        ctx->CfgHostStreamFrags = 2;
    }
    ctx->CfgCsAudioStart = AvsConfigReadUlong("CSAudioEndpointStart", 1);
    ctx->CfgCsAudioStop = AvsConfigReadUlong("CSAudioEndpointStop", 2);
    ctx->HwStarted = TRUE;

    // Announce ourselves to CoolStar codec drivers (avs_csaudio.c).
    AvsCsAudioRegisterDsp(ctx);

    KeWaitForSingleObject(&ctx->CoreLock, Executive, KernelMode, FALSE, NULL);
    status = AvsDspAndCoreUp(ctx);
    if (NT_SUCCESS(status))
    {
        AvsPortDiscoverEndpoints(ctx);
        AvsPortBootBeep(ctx);
    }
    KeReleaseMutex(&ctx->CoreLock, FALSE);

    // Headphone jack state from da7219.sys (needs the endpoint list).
    if (NT_SUCCESS(status))
    {
        AvsJackStart(ctx);
    }

    // A DSP that didn't come up still leaves the device started: no
    // endpoints get registered, but the log stays readable and the
    // device can be disabled/enabled to retry.
    return STATUS_SUCCESS;
}

VOID
AvsDeviceStop(
    _In_ PAVS_DEVICE Dev
    )
{
    PAVS_DEVICE_CONTEXT ctx = Dev;

    AvsJackStop(ctx);

    KeWaitForSingleObject(&ctx->CoreLock, Executive, KernelMode, FALSE, NULL);
    AvsPortSuspendStreams(ctx);
    AvsPortShutdown(ctx);
    // Leave the DSP powered off (upstream avs_pci_remove), so the next
    // start (re-enable, driver update) boots it from scratch.
    if (ctx->HwStarted)
    {
        AvsDspShutdown(ctx, AVS_ALL_CORES_MASK);
    }
    KeReleaseMutex(&ctx->CoreLock, FALSE);

    if (ctx->InterruptRegistered && ctx->AdspInterface.UnregisterInterrupt != NULL)
    {
        NTSTATUS st = ctx->AdspInterface.UnregisterInterrupt(ctx->AdspInterface.Context);
        UNREFERENCED_PARAMETER(st);
    }
    ctx->InterruptRegistered = FALSE;

    if (ctx->AdspInterfaceValid && ctx->AdspInterface.InterfaceDereference != NULL)
    {
        ctx->AdspInterface.InterfaceDereference(ctx->AdspInterface.Context);
    }
    ctx->AdspInterfaceValid = FALSE;
    ctx->HwStarted = FALSE;
    ctx->DspFirmwareLoaded = FALSE;

    AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "AVS: device stopped\n"));
}

VOID
AvsDeviceDestroy(
    _In_ PAVS_DEVICE Dev
    )
{
    PAVS_DEVICE_CONTEXT ctx = Dev;

    if (ctx == NULL)
    {
        return;
    }
    if (ctx->HwStarted)
    {
        AvsDeviceStop(ctx);
    }
    if (ctx->IpcRxBuf != NULL)
    {
        ExFreePoolWithTag(ctx->IpcRxBuf, 'xRvA');
        ctx->IpcRxBuf = NULL;
    }
    ExFreePoolWithTag(ctx, AVS_CTX_POOL_TAG);
}

// Called by IAdapterPowerManagement::PowerChangeState (PASSIVE_LEVEL).
// Leaving D0: tear down streams' DSP side and the core (DSP state is lost
// when the controller powers down). Back to D0: full DSP boot + core,
// then reopen/restart the streams Windows still has open.
// UNTESTED on hardware (sleep/resume, idle D3).
VOID
AvsDeviceSetPower(
    _In_ PAVS_DEVICE Dev,
    _In_ DEVICE_POWER_STATE NewState
    )
{
    PAVS_DEVICE_CONTEXT ctx = Dev;
    DEVICE_POWER_STATE old = ctx->PowerState;

    if (NewState == old)
    {
        return;
    }

    AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "AVS: power D%d -> D%d\n", (int)old - 1, (int)NewState - 1));

    KeWaitForSingleObject(&ctx->CoreLock, Executive, KernelMode, FALSE, NULL);

    if (NewState == PowerDeviceD0)
    {
        ctx->PowerState = PowerDeviceD0;
        if (ctx->HwStarted && NT_SUCCESS(AvsDspAndCoreUp(ctx)))
        {
            AvsPortResumeStreams(ctx);
        }
    }
    else if (old == PowerDeviceD0)
    {
        AvsPortSuspendStreams(ctx);
        AvsPortShutdown(ctx);
        if (ctx->HwStarted)
        {
            AvsDspShutdown(ctx, AVS_ALL_CORES_MASK);   // upstream suspend path
        }
        ctx->DspFirmwareLoaded = FALSE;
        ctx->PowerState = NewState;
    }
    else
    {
        ctx->PowerState = NewState;
    }

    KeReleaseMutex(&ctx->CoreLock, FALSE);
}

BOOLEAN
AvsDeviceGetEndpoint(
    _In_ PAVS_DEVICE Dev,
    _In_ ULONG Index,
    _Out_ PAVS_EP_INFO Info
    )
{
    PAVS_DEVICE_CONTEXT ctx = Dev;

    RtlZeroMemory(Info, sizeof(*Info));
    if (Index >= AVS_MAX_ENDPOINTS || !ctx->Endpoints[Index].Valid)
    {
        return FALSE;
    }
    *Info = ctx->Endpoints[Index].Info;
    return TRUE;
}
