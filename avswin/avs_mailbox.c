// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 AVS OpenAudio Driver Project contributors
//
// avs_mailbox.c -- SRAM uplink/downlink mailbox payload access.
//
// The IPC transport (avs_ipc.c) only carries the 8-byte HIPC header
// (primary + extension words) -- the actual payload of a reply or
// notification (e.g. the FIRMWARE_CONFIG TLV blob) lives in a
// separate SRAM window, read via plain MMIO after the header
// handshake completes. Offsets confirmed from upstream registers.h
// (avs_uplink_addr()/avs_downlink_addr() macros) -- see avs_regs.h
// for the derivation.

#include "driver.h"

// Reads up to Size bytes from the DSP->host uplink mailbox into
// Buffer. Size is silently capped at AVS_MAILBOX_SIZE (the window is
// only that big -- reading past it would just pull in the downlink
// window's content). Must be called after a reply/notification has
// been observed (HIPCT_BUSY seen) and BEFORE that HIPCT is
// acknowledged/cleared, matching upstream's avs_dsp_receive_rx(),
// which reads the payload before writing back to clear HIPCT_BUSY.
//
// Uses READ_REGISTER_BUFFER_UCHAR rather than RtlCopyMemory/memcpy:
// this SRAM window is device (uncached, MMIO-mapped) memory, and the
// WDK register-buffer intrinsics are the documented-safe way to bulk
// copy from that kind of mapping.
NTSTATUS
AvsMailboxReadUplink(
    _In_ PAVS_DEVICE_CONTEXT ctx,
    _Out_writes_bytes_(Size) PUCHAR Buffer,
    _In_ ULONG Size
    )
{
    if (ctx == NULL || ctx->DspBar.Base.Base == NULL || Buffer == NULL)
    {
        return STATUS_DEVICE_NOT_READY;
    }

    ULONG readSize = (Size > AVS_MAILBOX_SIZE) ? AVS_MAILBOX_SIZE : Size;
    PUCHAR mailbox = ctx->DspBar.Base.baseptr + AVS_UPLINK_MAILBOX_OFFSET;

    READ_REGISTER_BUFFER_UCHAR(mailbox, Buffer, readSize);

    return STATUS_SUCCESS;
}

// Writes up to Size bytes of Buffer into the host->DSP downlink
// mailbox. Must be called BEFORE triggering the IPC send (writing
// HIPCI with the BUSY bit) that references this payload -- e.g.
// INIT_INSTANCE's module-specific config blob. Size silently capped
// at AVS_MAILBOX_SIZE, same reasoning as the uplink read.
NTSTATUS
AvsMailboxWriteDownlink(
    _In_ PAVS_DEVICE_CONTEXT ctx,
    _In_reads_bytes_(Size) PUCHAR Buffer,
    _In_ ULONG Size
    )
{
    if (ctx == NULL || ctx->DspBar.Base.Base == NULL || Buffer == NULL)
    {
        return STATUS_DEVICE_NOT_READY;
    }

    ULONG writeSize = (Size > AVS_MAILBOX_SIZE) ? AVS_MAILBOX_SIZE : Size;
    PUCHAR mailbox = ctx->DspBar.Base.baseptr + AVS_DOWNLINK_MAILBOX_OFFSET;

    WRITE_REGISTER_BUFFER_UCHAR(mailbox, Buffer, writeSize);

    return STATUS_SUCCESS;
}
