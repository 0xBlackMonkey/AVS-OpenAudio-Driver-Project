// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 AVS OpenAudio Driver Project contributors
//
// avs_cldma.c -- Code Loader DMA: transfers dsp_basefw.bin into DSP SRAM.
//
// Corrected two-phase flow (from upstream loader.c's
// avs_cldma_load_basefw, which we now have the actual source of):
//
//   AvsCldmaPrepare()             -- called while the DSP core is still
//                                    CSTALL-asserted (out of reset, not
//                                    yet running). Loads the firmware
//                                    file, strips its ext-manifest
//                                    header, builds the BDL, resets the
//                                    CLDMA stream, and programs the
//                                    controller registers -- but does
//                                    NOT set SPIB, does NOT enable
//                                    ADSPIC_CLDMA, and does NOT set RUN.
//
//   [ caller then releases CSTALL and waits for ROM_INIT_DONE ]
//
//   AvsCldmaTriggerAndWaitBasefw() -- called only after ROM_INIT_DONE.
//                                    Writes SPIB (the whole payload
//                                    size, single-shot), enables
//                                    ADSPIC_CLDMA, sets RUN, then polls
//                                    the SRAM firmware-status register
//                                    (not SD_STS/LPIB) for
//                                    SKL_ROM_BASEFW_ENTERED.
//
// Register facts and control flow reimplemented independently against
// our own avs_regs.h; no GPL source reproduced.

#include "driver.h"

#define AVS_FW_PATH AVS_DATA_DIR L"dsp_basefw.bin"

typedef struct _CLDMA_BDL_ENTRY
{
    ULONG AddrLow;
    ULONG AddrHigh;
    ULONG Length;
    ULONG Ioc;
} CLDMA_BDL_ENTRY, *PCLDMA_BDL_ENTRY;

// ---- Firmware file loading ------------------------------------------------

static NTSTATUS
AvsReadFirmwareFile(
    _Out_ PVOID *Buffer,
    _Out_ PULONG Size
    )
{
    NTSTATUS status;
    UNICODE_STRING fileName;
    OBJECT_ATTRIBUTES objAttr;
    IO_STATUS_BLOCK ioStatus;
    HANDLE fileHandle = NULL;
    FILE_STANDARD_INFORMATION fileInfo;
    PVOID buffer = NULL;
    PHYSICAL_ADDRESS lowest, highest, boundary;

    *Buffer = NULL;
    *Size = 0;

    RtlInitUnicodeString(&fileName, AVS_FW_PATH);
    InitializeObjectAttributes(&objAttr, &fileName,
        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);

    status = ZwCreateFile(&fileHandle, GENERIC_READ, &objAttr, &ioStatus,
        NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OPEN,
        FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
    if (!NT_SUCCESS(status))
    {
        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "AVS: ZwCreateFile(%wZ) failed: 0x%X -- expected firmware at this path\n",
            &fileName, status));
        return status;
    }

    status = ZwQueryInformationFile(fileHandle, &ioStatus, &fileInfo,
        sizeof(fileInfo), FileStandardInformation);
    if (!NT_SUCCESS(status) || fileInfo.EndOfFile.QuadPart <= 0 ||
        fileInfo.EndOfFile.QuadPart > 16 * 1024 * 1024)
    {
        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "AVS: firmware file size query failed or out of sane range: 0x%X\n", status));
        ZwClose(fileHandle);
        return NT_SUCCESS(status) ? STATUS_INVALID_IMAGE_FORMAT : status;
    }

    {
        ULONG rawSize = (ULONG)fileInfo.EndOfFile.QuadPart;
        ULONG allocSize = (rawSize + (PAGE_SIZE - 1)) & ~((ULONG)PAGE_SIZE - 1);

        lowest.QuadPart = 0;
        highest.QuadPart = MAXLONGLONG;
        boundary.QuadPart = 0;

        buffer = MmAllocateContiguousMemorySpecifyCache(
            allocSize, lowest, highest, boundary, MmCached);
        if (buffer == NULL)
        {
            ZwClose(fileHandle);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlZeroMemory(buffer, allocSize);

        LARGE_INTEGER offset;
        offset.QuadPart = 0;
        status = ZwReadFile(fileHandle, NULL, NULL, NULL, &ioStatus,
            buffer, rawSize, &offset, NULL);
        ZwClose(fileHandle);

        if (!NT_SUCCESS(status) || ioStatus.Information != rawSize)
        {
            AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                "AVS: ZwReadFile firmware failed: 0x%X (read %zu of %lu)\n",
                status, ioStatus.Information, rawSize));
            MmFreeContiguousMemory(buffer);
            return NT_SUCCESS(status) ? STATUS_UNEXPECTED_IO_ERROR : status;
        }

        *Buffer = buffer;
        *Size = rawSize;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
AvsStripExtManifest(
    _In_ PVOID RawBuffer,
    _In_ ULONG RawSize,
    _Out_ PVOID *Payload,
    _Out_ PULONG PayloadSize
    )
{
    PUCHAR raw = (PUCHAR)RawBuffer;

    if (RawSize < 8 || raw[0] != '$' || raw[1] != 'A' || raw[2] != 'E' || raw[3] != '1')
    {
        *Payload = RawBuffer;
        *PayloadSize = RawSize;
        return STATUS_SUCCESS;
    }

    ULONG hdrLen = *(PULONG)(raw + 4);
    if (hdrLen == 0 || hdrLen >= RawSize)
    {
        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "AVS: ext-manifest header length 0x%X looks invalid for file size 0x%X\n",
            hdrLen, RawSize));
        return STATUS_INVALID_IMAGE_FORMAT;
    }

    *Payload = raw + hdrLen;
    *PayloadSize = RawSize - hdrLen;

    AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "AVS: stripped $AE1 ext-manifest, header=0x%X bytes, payload=0x%X bytes\n",
        hdrLen, *PayloadSize));

    return STATUS_SUCCESS;
}

// ---- CLDMA stream reset (SRST assert/deassert handshake) ------------------

static NTSTATUS
AvsCldmaResetStream(
    _In_ PUCHAR DspBar
    )
{
    ULONG ctl;
    ULONG elapsed;

    // Stop first: clear RUN and int-enables.
    ctl = READ_REGISTER_ULONG((PULONG)(DspBar + SKL_ADSP_REG_CL_SD_CTL));
    ctl &= ~(CL_SD_CTL_RUN_MASK | CL_SD_CTL_IOCE_MASK | CL_SD_CTL_FEIE_MASK | CL_SD_CTL_DEIE_MASK);
    WRITE_REGISTER_ULONG((PULONG)(DspBar + SKL_ADSP_REG_CL_SD_CTL), ctl);

    elapsed = 0;
    for (;;)
    {
        ctl = READ_REGISTER_ULONG((PULONG)(DspBar + SKL_ADSP_REG_CL_SD_CTL));
        if (!(ctl & CL_SD_CTL_RUN_MASK)) break;
        if (elapsed >= AVS_ADSPCS_TIMEOUT_US) return STATUS_TIMEOUT;
        KeStallExecutionProcessor(3);
        elapsed += 3;
    }

    // Assert SRST.
    ctl |= CL_SD_CTL_SRST_MASK;
    WRITE_REGISTER_ULONG((PULONG)(DspBar + SKL_ADSP_REG_CL_SD_CTL), ctl);

    elapsed = 0;
    for (;;)
    {
        ctl = READ_REGISTER_ULONG((PULONG)(DspBar + SKL_ADSP_REG_CL_SD_CTL));
        if (ctl & CL_SD_CTL_SRST_MASK) break;
        if (elapsed >= AVS_ADSPCS_TIMEOUT_US) return STATUS_TIMEOUT;
        KeStallExecutionProcessor(3);
        elapsed += 3;
    }

    // Deassert SRST.
    ctl &= ~CL_SD_CTL_SRST_MASK;
    WRITE_REGISTER_ULONG((PULONG)(DspBar + SKL_ADSP_REG_CL_SD_CTL), ctl);

    elapsed = 0;
    for (;;)
    {
        ctl = READ_REGISTER_ULONG((PULONG)(DspBar + SKL_ADSP_REG_CL_SD_CTL));
        if (!(ctl & CL_SD_CTL_SRST_MASK)) break;
        if (elapsed >= AVS_ADSPCS_TIMEOUT_US) return STATUS_TIMEOUT;
        KeStallExecutionProcessor(3);
        elapsed += 3;
    }

    WRITE_REGISTER_ULONG((PULONG)(DspBar + SKL_ADSP_REG_CL_SD_BDLPL), 0);
    WRITE_REGISTER_ULONG((PULONG)(DspBar + SKL_ADSP_REG_CL_SD_BDLPU), 0);
    WRITE_REGISTER_ULONG((PULONG)(DspBar + SKL_ADSP_REG_CL_SD_CBL), 0);
    WRITE_REGISTER_ULONG((PULONG)(DspBar + SKL_ADSP_REG_CL_SD_LVI), 0);

    return STATUS_SUCCESS;
}

// ---- Phase 1: prepare (core still stalled) --------------------------------

NTSTATUS
AvsCldmaPrepare(
    _In_ PAVS_DEVICE_CONTEXT ctx
    )
{
    NTSTATUS status;
    PVOID rawBuffer = NULL;
    ULONG rawSize = 0;
    PVOID payload;
    ULONG payloadSize;
    PVOID dataBuf = NULL;
    PVOID bdlBuf = NULL;
    PHYSICAL_ADDRESS dataPhys, bdlPhys, lowest, highest, boundary;
    PUCHAR dspBar;

    if (ctx->DspBar.Base.Base == NULL)
    {
        return STATUS_DEVICE_NOT_READY;
    }
    dspBar = ctx->DspBar.Base.baseptr;

    status = AvsReadFirmwareFile(&rawBuffer, &rawSize);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    status = AvsStripExtManifest(rawBuffer, rawSize, &payload, &payloadSize);
    if (!NT_SUCCESS(status))
    {
        MmFreeContiguousMemory(rawBuffer);
        return status;
    }

    {
        ULONG allocSize = (payloadSize + (PAGE_SIZE - 1)) & ~((ULONG)PAGE_SIZE - 1);

        lowest.QuadPart = 0;
        highest.QuadPart = MAXLONGLONG;
        boundary.QuadPart = 0;

        dataBuf = MmAllocateContiguousMemorySpecifyCache(
            allocSize, lowest, highest, boundary, MmCached);
        if (dataBuf == NULL)
        {
            MmFreeContiguousMemory(rawBuffer);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlCopyMemory(dataBuf, payload, payloadSize);
        dataPhys = MmGetPhysicalAddress(dataBuf);
    }

    MmFreeContiguousMemory(rawBuffer);
    rawBuffer = NULL;

    bdlBuf = MmAllocateContiguousMemorySpecifyCache(
        PAGE_SIZE, lowest, highest, boundary, MmCached);
    if (bdlBuf == NULL)
    {
        MmFreeContiguousMemory(dataBuf);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(bdlBuf, PAGE_SIZE);
    bdlPhys = MmGetPhysicalAddress(bdlBuf);

    {
        PCLDMA_BDL_ENTRY bdl = (PCLDMA_BDL_ENTRY)bdlBuf;
        bdl[0].AddrLow  = (ULONG)(dataPhys.QuadPart & 0xFFFFFFFFu);
        bdl[0].AddrHigh = (ULONG)((ULONGLONG)dataPhys.QuadPart >> 32);
        bdl[0].Length   = payloadSize;
        bdl[0].Ioc      = 1;
    }

    status = AvsCldmaResetStream(dspBar);
    if (!NT_SUCCESS(status))
    {
        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "AVS: CLDMA stream reset failed: 0x%X\n", status));
        MmFreeContiguousMemory(dataBuf);
        MmFreeContiguousMemory(bdlBuf);
        return status;
    }

    WRITE_REGISTER_ULONG((PULONG)(dspBar + SKL_ADSP_REG_CL_SD_BDLPL),
        CL_SD_BDLPLBA(bdlPhys.QuadPart));
    WRITE_REGISTER_ULONG((PULONG)(dspBar + SKL_ADSP_REG_CL_SD_BDLPU),
        CL_SD_BDLPUBA(bdlPhys.QuadPart));
    WRITE_REGISTER_ULONG((PULONG)(dspBar + SKL_ADSP_REG_CL_SD_CBL), payloadSize);
    WRITE_REGISTER_ULONG((PULONG)(dspBar + SKL_ADSP_REG_CL_SD_LVI), 0); // 1 entry

    {
        ULONG ctl = READ_REGISTER_ULONG((PULONG)(dspBar + SKL_ADSP_REG_CL_SD_CTL));
        ctl = (ctl & ~CL_SD_CTL_STRM_MASK) | CL_SD_CTL_STRM(FW_CL_STREAM_NUMBER);
        WRITE_REGISTER_ULONG((PULONG)(dspBar + SKL_ADSP_REG_CL_SD_CTL), ctl);
    }

    // Enable SPIB gate (upstream just writes literal 1 -- SPBFCTL's
    // only meaningful bit is SPIBE at bit0, register otherwise reads
    // back 0 from reset, so this is equivalent to our masked-OR form).
    WRITE_REGISTER_ULONG((PULONG)(dspBar + SKL_ADSP_REG_CL_SPBFIFO_SPBFCCTL), 1);
    WRITE_REGISTER_ULONG((PULONG)(dspBar + SKL_ADSP_REG_CL_SPBFIFO_SPIB), 0);

    ctx->FwDataBuf = dataBuf;
    ctx->FwDataPhys = dataPhys;
    ctx->FwBdlBuf = bdlBuf;
    ctx->FwPayloadSize = payloadSize;

    AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "AVS: CLDMA prepared (not triggered), payload=0x%X bytes at phys 0x%llX, BDL at phys 0x%llX\n",
        payloadSize, dataPhys.QuadPart, bdlPhys.QuadPart));

    return STATUS_SUCCESS;
}

// ---- Phase 2: trigger + wait (only after ROM_INIT_DONE) -------------------

NTSTATUS
AvsCldmaTriggerAndWaitBasefw(
    _In_ PAVS_DEVICE_CONTEXT ctx
    )
{
    PUCHAR dspBar;
    ULONG ctl;
    NTSTATUS status;

    if (ctx->DspBar.Base.Base == NULL || ctx->FwDataBuf == NULL || ctx->FwBdlBuf == NULL)
    {
        return STATUS_DEVICE_NOT_READY;
    }
    dspBar = ctx->DspBar.Base.baseptr;

    // Diagnostic: log FW_STATUS BEFORE we touch anything here. If this
    // already reads 0xF (SKL_ROM_BASEFW_ENTERED), the "success" this
    // function reports later may be a false positive -- a stale value
    // left over from a prior session where the DSP never actually lost
    // power (e.g. across our own repeated Device Manager disable/
    // enable cycles, which don't necessarily force a real PCI D3->D0
    // transition) rather than genuine evidence our own transfer worked.
    {
        ULONG preStatus = READ_REGISTER_ULONG((PULONG)(dspBar + AVS_FW_REG_STATUS));
        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
            "AVS: FW_STATUS BEFORE trigger = 0x%08X (0xF here would mean stale/leftover state)\n",
            preStatus));
    }

    // Single-shot: the whole payload is already valid in the buffer,
    // so SPIB can point at the full size up front (mirrors what
    // hda_cldma_fill would do incrementally for a large image).
    WRITE_REGISTER_ULONG((PULONG)(dspBar + SKL_ADSP_REG_CL_SPBFIFO_SPIB), ctx->FwPayloadSize);

    // Enable the DSP-wide CLDMA interrupt gate.
    {
        ULONG adspic = READ_REGISTER_ULONG((PULONG)(dspBar + AVS_ADSP_REG_ADSPIC));
        adspic |= AVS_ADSPIC_CLDMA;
        WRITE_REGISTER_ULONG((PULONG)(dspBar + AVS_ADSP_REG_ADSPIC), adspic);
    }

    // Trigger: enable stream int-sources + RUN together, then confirm.
    ctl = READ_REGISTER_ULONG((PULONG)(dspBar + SKL_ADSP_REG_CL_SD_CTL));
    ctl |= CL_SD_CTL_IOCE_MASK | CL_SD_CTL_FEIE_MASK | CL_SD_CTL_DEIE_MASK | CL_SD_CTL_RUN_MASK;
    WRITE_REGISTER_ULONG((PULONG)(dspBar + SKL_ADSP_REG_CL_SD_CTL), ctl);
    KeStallExecutionProcessor(5);

    {
        ULONG runTimeout = 900;
        while (runTimeout > 0)
        {
            ctl = READ_REGISTER_ULONG((PULONG)(dspBar + SKL_ADSP_REG_CL_SD_CTL));
            if (ctl & CL_SD_CTL_RUN_MASK) break;
            KeStallExecutionProcessor(3);
            runTimeout -= 3;
        }
        if (!(ctl & CL_SD_CTL_RUN_MASK))
        {
            AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                "AVS: CLDMA RUN bit never took effect\n"));
            status = STATUS_DEVICE_DATA_ERROR;
            goto stop_and_cleanup;
        }
    }

    AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "AVS: CLDMA triggered, payload=0x%X bytes -- waiting for BASEFW_ENTERED\n",
        ctx->FwPayloadSize));

    // Poll the SRAM firmware-status register (NOT SD_STS/LPIB) for the
    // ROM having entered the just-transferred base firmware.
    {
        ULONG elapsed = 0;
        PULONG fwStatusReg = (PULONG)(dspBar + AVS_FW_REG_STATUS);

        for (;;)
        {
            ULONG fwStatus = READ_REGISTER_ULONG(fwStatusReg);
            if ((fwStatus & AVS_ROM_STS_MASK) == SKL_ROM_BASEFW_ENTERED)
            {
                status = STATUS_SUCCESS;
                AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                    "AVS: firmware transfer complete, FW_STATUS=0x%08X\n", fwStatus));
                break;
            }
            if (elapsed >= AVS_FW_INIT_TIMEOUT_US)
            {
                ULONG fwError = READ_REGISTER_ULONG((PULONG)(dspBar + AVS_FW_REG_ERROR));
                AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                    "AVS: basefw entry timeout, FW_STATUS=0x%08X, FW_ERROR=0x%08X\n",
                    fwStatus, fwError));
                status = STATUS_TIMEOUT;
                break;
            }

            KeStallExecutionProcessor(AVS_FW_INIT_POLLING_US);
            elapsed += AVS_FW_INIT_POLLING_US;
        }
    }

stop_and_cleanup:
    ctl = READ_REGISTER_ULONG((PULONG)(dspBar + SKL_ADSP_REG_CL_SD_CTL));
    ctl &= ~CL_SD_CTL_RUN_MASK;
    WRITE_REGISTER_ULONG((PULONG)(dspBar + SKL_ADSP_REG_CL_SD_CTL), ctl);

    {
        ULONG adspic = READ_REGISTER_ULONG((PULONG)(dspBar + AVS_ADSP_REG_ADSPIC));
        adspic &= ~AVS_ADSPIC_CLDMA;
        WRITE_REGISTER_ULONG((PULONG)(dspBar + AVS_ADSP_REG_ADSPIC), adspic);
    }

    // Buffers are freed by AvsCldmaRelease() once the boot sequence is
    // over (the DSP runs from its own SRAM after BASEFW_ENTERED).

    return status;
}

// Frees the basefw image + BDL. Safe to call more than once; called after
// every boot attempt so a DSP re-boot (resume from D3) doesn't leak.
VOID
AvsCldmaRelease(
    _In_ PAVS_DEVICE_CONTEXT ctx
    )
{
    // Same sizes/cache type as allocated in AvsCldmaPrepare().
    if (ctx->FwDataBuf != NULL)
    {
        ULONG allocSize = (ctx->FwPayloadSize + (PAGE_SIZE - 1)) & ~((ULONG)PAGE_SIZE - 1);
        MmFreeContiguousMemorySpecifyCache(ctx->FwDataBuf, allocSize, MmCached);
        ctx->FwDataBuf = NULL;
    }
    if (ctx->FwBdlBuf != NULL)
    {
        MmFreeContiguousMemorySpecifyCache(ctx->FwBdlBuf, PAGE_SIZE, MmCached);
        ctx->FwBdlBuf = NULL;
    }
    ctx->FwPayloadSize = 0;
}
