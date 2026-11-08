// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 AVS OpenAudio Driver Project contributors
//
// avs_ipc.c -- generic HIPC "send request, wait for reply" transport.
//
// Built on the HIPC register facts confirmed earlier in this project
// (SKL_ADSP_REG_HIPCI/HIPCIE/HIPCT/HIPCTE offsets and busy/done bits)
// and on the real ISR entry point confirmed via sklhdaudbus's
// hda_interrupt(), which calls our registered dspInterruptCallback
// unconditionally on every interrupt for this PCI device.
//
// IRQL note: our callback runs synchronously inside sklhdaudbus's own
// ISR, at that interrupt's IRQL -- which may be true device IRQL, not
// just DISPATCH_LEVEL. KeSetEvent requires IRQL <= DISPATCH_LEVEL, so
// completion is deferred through a raw KDPC (KeInsertQueueDpc is safe
// from any IRQL) rather than signalling the event directly from the
// callback.

#include "driver.h"

// Generic hex dump -- 16 bytes per line, offset-prefixed. Uses only a
// stack buffer and DbgPrintEx, so it is safe at any IRQL (incl. our ISR).
// Byte order is raw memory order (little-endian dwords appear byte-
// reversed), which is what Linux trace events print too -- so lines can
// be diffed directly against an ftrace payload dump.
VOID
AvsDumpHex(
    _In_ PCSTR Label,
    _In_reads_bytes_(Size) const UCHAR* Buf,
    _In_ ULONG Size
    )
{
    static const char hex[] = "0123456789ABCDEF";
    CHAR line[16 * 3 + 1];

    AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "AVS: %s -- %lu bytes:\n", Label, Size));

    if (Buf == NULL)
    {
        return;
    }

    for (ULONG off = 0; off < Size; off += 16)
    {
        ULONG n = Size - off;
        if (n > 16)
        {
            n = 16;
        }

        ULONG p = 0;
        for (ULONG i = 0; i < n; i++)
        {
            UCHAR b = Buf[off + i];
            line[p++] = hex[b >> 4];
            line[p++] = hex[b & 0xF];
            line[p++] = ' ';
        }
        line[p] = '\0';

        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
            "AVS:   %04lX: %s\n", off, line));
    }
}

static VOID
AvsIpcDpcRoutine(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2
    )
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    PAVS_DEVICE_CONTEXT ctx = (PAVS_DEVICE_CONTEXT)DeferredContext;
    if (ctx == NULL)
    {
        return;
    }

    KeSetEvent(&ctx->IpcDoneEvent, IO_NO_INCREMENT, FALSE);
}

// Deferred completion for the FW_READY notification -- same IRQL
// constraint as AvsIpcDpcRoutine above (ISR context can't call
// KeSetEvent directly), kept as its own event/DPC pair rather than
// reusing IpcDoneEvent so a boot-sequence FW_READY wait can never be
// mistaken for -- or race with -- an in-flight command reply.
static VOID
AvsFwReadyDpcRoutine(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2
    )
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    PAVS_DEVICE_CONTEXT ctx = (PAVS_DEVICE_CONTEXT)DeferredContext;
    if (ctx == NULL)
    {
        return;
    }

    KeSetEvent(&ctx->FwReadyEvent, IO_NO_INCREMENT, FALSE);
}

VOID
AvsIpcInitInfra(
    _In_ PAVS_DEVICE_CONTEXT ctx
    )
{
    if (ctx->IpcInfraInitialized)
    {
        return;
    }

    KeInitializeDpc(&ctx->IpcDpc, AvsIpcDpcRoutine, ctx);
    KeInitializeEvent(&ctx->IpcDoneEvent, NotificationEvent, FALSE);
    ctx->IpcReplyHipct = 0;
    ctx->IpcReplyHipcte = 0;

    KeInitializeDpc(&ctx->FwReadyDpc, AvsFwReadyDpcRoutine, ctx);
    KeInitializeEvent(&ctx->FwReadyEvent, NotificationEvent, FALSE);

    if (ctx->IpcRxBuf == NULL)
    {
        ctx->IpcRxBuf = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, AVS_MAILBOX_SIZE, 'xRvA');
    }
    ctx->IpcRxExpect = 0;
    ctx->IpcRxSize = 0;

    ctx->IpcInfraInitialized = TRUE;
}

// Called from AvsAdspInterruptCallback (avs_boot.c) when the FW_READY
// notification is observed. Runs at the ISR's IRQL -- must stay
// IRQL-safe (no KeSetEvent here), same as AvsIpcOnReplyFromIsr below.
VOID
AvsIpcOnFwReadyFromIsr(
    _In_ PAVS_DEVICE_CONTEXT ctx
    )
{
    if (!ctx->IpcInfraInitialized)
    {
        return;
    }

    KeInsertQueueDpc(&ctx->FwReadyDpc, NULL, NULL);
}

// Called from AvsAdspInterruptCallback (avs_boot.c) when a non-notification
// message (i.e. a reply to something we sent) is observed at HIPCT.
// Runs at the ISR's IRQL -- must stay IRQL-safe (no KeSetEvent here).
VOID
AvsIpcOnReplyFromIsr(
    _In_ PAVS_DEVICE_CONTEXT ctx,
    _In_ ULONG Hipct,
    _In_ ULONG Hipcte
    )
{
    if (!ctx->IpcInfraInitialized)
    {
        return;
    }

    // Copy the reply payload out of the uplink mailbox now, before the
    // caller (our ISR) acks HIPCT -- same order as upstream
    // avs_dsp_receive_rx(). Only for successful replies; LARGE_CONFIG_GET
    // replies carry their own size in ext.large_config.data_off_size
    // (bits 0-19), everything else gets what the sender said to expect.
    // READ_REGISTER_BUFFER_UCHAR is safe at device IRQL.
    {
        ULONG status = Hipct & 0xFFFFFFu;
        ULONG msgTarget = (Hipct >> 30) & 1u;
        ULONG msgType = (Hipct >> 24) & 0x1Fu;
        ULONG size = 0;

        if (status == 0 && ctx->IpcRxBuf != NULL)
        {
            size = (msgTarget == 1 && msgType == 3 /* LARGE_CONFIG_GET */)
                ? (Hipcte & 0xFFFFFu) : ctx->IpcRxExpect;
            if (size > AVS_MAILBOX_SIZE)
            {
                size = AVS_MAILBOX_SIZE;
            }
            if (size != 0)
            {
                AvsMailboxReadUplink(ctx, ctx->IpcRxBuf, size);
            }
        }
        ctx->IpcRxSize = size;
    }

    ctx->IpcReplyHipct = Hipct;
    ctx->IpcReplyHipcte = Hipcte;
    KeInsertQueueDpc(&ctx->IpcDpc, NULL, NULL);
}

static NTSTATUS
AvsIpcSendCore(
    _In_ PAVS_DEVICE_CONTEXT ctx,
    _In_ ULONG Primary,
    _In_ ULONG Extension,
    _In_reads_bytes_opt_(PayloadSize) PUCHAR Payload,
    _In_ ULONG PayloadSize,
    _In_ ULONG RxExpect,
    _In_ ULONG TimeoutMs,
    _Out_ PULONG ReplyPrimary,
    _Out_ PULONG ReplyExtension
    )
{
    if (ctx->DspBar.Base.Base == NULL)
    {
        return STATUS_DEVICE_NOT_READY;
    }
    if (!ctx->IpcInfraInitialized)
    {
        AvsIpcInitInfra(ctx);
    }

    PUCHAR dspBar = ctx->DspBar.Base.baseptr;

    // PASSIVE_LEVEL only (KeWaitForSingleObject below); callers hold
    // ctx->CoreLock, so messages never overlap.

    ULONG hipci = READ_REGISTER_ULONG((PULONG)(dspBar + SKL_ADSP_REG_HIPCI));
    if (hipci & SKL_ADSP_HIPCI_BUSY)
    {
        // Left over from a message the firmware never acknowledged.
        // This is the host-side busy bit, so clearing it is safe.
        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "AVS: IPC send -- HIPCI was already busy (0x%08X), forcing clear and proceeding\n",
            hipci));
        WRITE_REGISTER_ULONG((PULONG)(dspBar + SKL_ADSP_REG_HIPCI), 0);
    }

    KeClearEvent(&ctx->IpcDoneEvent);
    ctx->IpcReplyHipct = 0;
    ctx->IpcReplyHipcte = 0;
    ctx->IpcRxExpect = RxExpect;
    ctx->IpcRxSize = 0;

    if (ctx->IpcDumpPayloads)
    {
        ULONG preHipcie = READ_REGISTER_ULONG((PULONG)(dspBar + SKL_ADSP_REG_HIPCIE));
        ULONG preHipct = READ_REGISTER_ULONG((PULONG)(dspBar + SKL_ADSP_REG_HIPCT));
        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
            "AVS: IPC send primary=0x%08X ext=0x%08X payload=%lu (HIPCIE=0x%08X HIPCT=0x%08X before)\n",
            Primary, Extension, PayloadSize, preHipcie, preHipct));
    }

    // Payload (if any) must land in the downlink mailbox BEFORE we
    // trigger the send -- the DSP reads it as part of processing the
    // just-arrived HIPCI/HIPCIE header, so it must already be there.
    if (Payload != NULL && PayloadSize > 0)
    {
        if (ctx->IpcDumpPayloads)
        {
            AvsDumpHex("IPC send payload", Payload, PayloadSize);
        }

        NTSTATUS mbStatus = AvsMailboxWriteDownlink(ctx, Payload, PayloadSize);
        if (!NT_SUCCESS(mbStatus))
        {
            AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                "AVS: IPC send -- downlink mailbox write failed: 0x%X\n", mbStatus));
            return mbStatus;
        }
    }

    WRITE_REGISTER_ULONG((PULONG)(dspBar + SKL_ADSP_REG_HIPCIE), Extension);
    WRITE_REGISTER_ULONG((PULONG)(dspBar + SKL_ADSP_REG_HIPCI), Primary | SKL_ADSP_HIPCI_BUSY);

    LARGE_INTEGER timeout;
    timeout.QuadPart = -((LONGLONG)TimeoutMs * 10000); // relative, 100ns units

    NTSTATUS waitStatus = KeWaitForSingleObject(
        &ctx->IpcDoneEvent, Executive, KernelMode, FALSE, &timeout);

    // Always clear our own busy bit -- if the DSP never answered, this
    // at least leaves the transport in a known state for next time.
    WRITE_REGISTER_ULONG((PULONG)(dspBar + SKL_ADSP_REG_HIPCI), 0);

    if (waitStatus != STATUS_SUCCESS)
    {
        // Full post-mortem register dump -- captures whatever state
        // the transport was actually left in, so a repeat test isn't
        // needed just to see these values.
        ULONG adspis = READ_REGISTER_ULONG((PULONG)(dspBar + AVS_ADSP_REG_ADSPIS));
        ULONG hipct = READ_REGISTER_ULONG((PULONG)(dspBar + SKL_ADSP_REG_HIPCT));
        ULONG hipcte = READ_REGISTER_ULONG((PULONG)(dspBar + SKL_ADSP_REG_HIPCTE));
        ULONG hipcie = READ_REGISTER_ULONG((PULONG)(dspBar + SKL_ADSP_REG_HIPCIE));
        ULONG hipcctl = READ_REGISTER_ULONG((PULONG)(dspBar + SKL_ADSP_REG_HIPCCTL));
        ULONG fwStatus = READ_REGISTER_ULONG((PULONG)(dspBar + AVS_FW_REG_STATUS));
        ULONG fwError = READ_REGISTER_ULONG((PULONG)(dspBar + AVS_FW_REG_ERROR));

        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "AVS: IPC send -- no reply within %lu ms (wait status 0x%X)\n",
            TimeoutMs, waitStatus));
        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "AVS: IPC send -- post-mortem: ADSPIS=0x%08X HIPCT=0x%08X HIPCTE=0x%08X "
            "HIPCIE=0x%08X HIPCCTL=0x%08X FW_STATUS=0x%08X FW_ERROR=0x%08X\n",
            adspis, hipct, hipcte, hipcie, hipcctl, fwStatus, fwError));
        return STATUS_TIMEOUT;
    }

    *ReplyPrimary = ctx->IpcReplyHipct;
    *ReplyExtension = ctx->IpcReplyHipcte;

    if (ctx->IpcDumpPayloads)
    {
        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
            "AVS: IPC reply HIPCT=0x%08X HIPCTE=0x%08X\n", *ReplyPrimary, *ReplyExtension));
    }

    return STATUS_SUCCESS;
}


NTSTATUS
AvsIpcSendWithPayloadAndWait(
    _In_ PAVS_DEVICE_CONTEXT ctx,
    _In_ ULONG Primary,
    _In_ ULONG Extension,
    _In_reads_bytes_(PayloadSize) PUCHAR Payload,
    _In_ ULONG PayloadSize,
    _In_ ULONG TimeoutMs,
    _Out_ PULONG ReplyPrimary,
    _Out_ PULONG ReplyExtension
    )
{
    return AvsIpcSendCore(ctx, Primary, Extension, Payload, PayloadSize, 0,
        TimeoutMs, ReplyPrimary, ReplyExtension);
}

NTSTATUS
AvsIpcSendRaw(
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
    _Out_ PULONG ReplyExtension
    )
{
    if (RxSize != NULL)
    {
        *RxSize = 0;
    }

    NTSTATUS status = AvsIpcSendCore(ctx, Primary, Extension, (PUCHAR)Payload, PayloadSize,
        RxExpect, TimeoutMs, ReplyPrimary, ReplyExtension);
    if (status != STATUS_SUCCESS)
    {
        return status;
    }

    ULONG size = ctx->IpcRxSize;
    if (size > AVS_MAILBOX_SIZE)
    {
        size = AVS_MAILBOX_SIZE;
    }
    if (RxBuf != NULL && size != 0 && ctx->IpcRxBuf != NULL)
    {
        RtlCopyMemory(RxBuf, ctx->IpcRxBuf, size);
    }
    if (RxSize != NULL)
    {
        *RxSize = size;
    }
    return STATUS_SUCCESS;
}
