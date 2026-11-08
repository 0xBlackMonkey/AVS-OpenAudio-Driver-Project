// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 AVS OpenAudio Driver Project contributors
//
// avs_dsp_boot.c -- DSP core power/reset sequencing.
//
// Corrected sequence (from upstream loader.c's avs_cldma_load_basefw):
//   1. PGCTL/CGCTL PCI config-space fixup
//   2. PPCTL global-enable (GPROCEN/PIE)
//   3. Power on core (SPA), wait for CPA
//   4. Deassert CRST -- core out of reset, still stalled
//   5. <-- CLDMA must be reset+programmed HERE, while core is still
//         stalled -- done by the caller (avs_cldma.c) between steps
//         5 and 6, not something this file does internally.
//   6. Deassert CSTALL -- core starts executing its on-die ROM
//   7. Poll SRAM firmware-status register for ROM_INIT_DONE
//
// This file owns steps 1-4 (AvsDspCoreResetStall) and 6-7
// (AvsDspReleaseStallAndWaitRom), with CLDMA setup deliberately left
// to happen in between, orchestrated by AvsDspBoot() in avs_boot.c.

#include "driver.h"

static NTSTATUS
AvsPollAdspcs(
    _In_ PAVS_DEVICE_CONTEXT ctx,
    _In_ ULONG ExpectedMask,
    _In_ ULONG ExpectedValue,
    _In_ ULONG TimeoutUs
    )
{
    ULONG elapsed = 0;

    for (;;)
    {
        ULONG adspcs = READ_REGISTER_ULONG(
            (PULONG)(ctx->DspBar.Base.baseptr + AVS_ADSP_REG_ADSPCS));

        // A 0xFFFFFFFF read means the register space isn't actually
        // live yet (link down / not clocked), not real content -- an
        // all-ones value trivially satisfies almost any low-bit mask
        // check, producing a false-positive "done" on garbage. Treat
        // it as "not ready yet" and keep polling, matching upstream's
        // universal UINT_MAX guard (see apl.c/cnl.c/skl.c/mtl.c
        // dsp_interrupt: "if (adspis == UINT_MAX) return").
        if (adspcs != 0xFFFFFFFFu && (adspcs & ExpectedMask) == ExpectedValue)
        {
            return STATUS_SUCCESS;
        }

        if (elapsed >= TimeoutUs)
        {
            AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                "AVS: ADSPCS poll timeout (last=0x%08X, want mask=0x%08X val=0x%08X)\n",
                adspcs, ExpectedMask, ExpectedValue));
            return STATUS_TIMEOUT;
        }

        KeStallExecutionProcessor(AVS_ADSPCS_INTERVAL_US);
        elapsed += AVS_ADSPCS_INTERVAL_US;
    }
}

// Upstream avs_dsp_core_disable() (dsp.c) for the SKL register set
// (skl_dsp_core_stall/reset/power): stall, reset, drop power -- no error
// checks, "to allow for complete DSP shutdown". Preceded by
// int_control(false) as in avs_pci_remove()/suspend.
VOID
AvsDspShutdown(
    _In_ PAVS_DEVICE_CONTEXT ctx,
    ULONG coreMask
    )
{
    if (ctx->DspBar.Base.Base == NULL)
    {
        return;
    }

    PULONG adspcsReg = (PULONG)(ctx->DspBar.Base.baseptr + AVS_ADSP_REG_ADSPCS);
    ULONG before = READ_REGISTER_ULONG(adspcsReg);
    ULONG val;

    // IPC interrupt gates off (avs_dsp_interrupt_control(adev, false)).
    {
        PULONG adspicReg = (PULONG)(ctx->DspBar.Base.baseptr + AVS_ADSP_REG_ADSPIC);
        PULONG hipcctlReg = (PULONG)(ctx->DspBar.Base.baseptr + SKL_ADSP_REG_HIPCCTL);

        WRITE_REGISTER_ULONG(hipcctlReg,
            READ_REGISTER_ULONG(hipcctlReg) & ~(AVS_ADSP_HIPCCTL_BUSY | AVS_ADSP_HIPCCTL_DONE));
        WRITE_REGISTER_ULONG(adspicReg, READ_REGISTER_ULONG(adspicReg) & ~AVS_ADSPIC_IPC);
    }

    val = READ_REGISTER_ULONG(adspcsReg) | AVS_ADSPCS_CSTALL_MASK(coreMask);
    WRITE_REGISTER_ULONG(adspcsReg, val);
    (VOID)AvsPollAdspcs(ctx, AVS_ADSPCS_CSTALL_MASK(coreMask), AVS_ADSPCS_CSTALL_MASK(coreMask),
        AVS_ADSPCS_TIMEOUT_US);

    val = READ_REGISTER_ULONG(adspcsReg) | AVS_ADSPCS_CRST_MASK(coreMask);
    WRITE_REGISTER_ULONG(adspcsReg, val);
    (VOID)AvsPollAdspcs(ctx, AVS_ADSPCS_CRST_MASK(coreMask), AVS_ADSPCS_CRST_MASK(coreMask),
        AVS_ADSPCS_TIMEOUT_US);

    val = READ_REGISTER_ULONG(adspcsReg) & ~AVS_ADSPCS_SPA_MASK(coreMask);
    WRITE_REGISTER_ULONG(adspcsReg, val);
    (VOID)AvsPollAdspcs(ctx, AVS_ADSPCS_CPA_MASK(coreMask), 0, AVS_ADSPCS_TIMEOUT_US);

    ctx->DspFirmwareLoaded = FALSE;

    AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "AVS: DSP cores 0x%X shut down, ADSPCS 0x%08X -> 0x%08X\n",
        coreMask, before, READ_REGISTER_ULONG(adspcsReg)));
}

NTSTATUS
AvsDspCoreResetStall(
    _In_ PAVS_DEVICE_CONTEXT ctx,
    ULONG coreMask
    )
{
    if (ctx->DspBar.Base.Base == NULL)
    {
        return STATUS_DEVICE_NOT_READY;
    }

    PULONG adspcsReg = (PULONG)(ctx->DspBar.Base.baseptr + AVS_ADSP_REG_ADSPCS);
    NTSTATUS status;
    ULONG val;

    // Step 0: release link-stereo-reset and enable clock gating via PCI
    // config space -- required before ADSPCS SPA/CPA responds at all.
    if (ctx->PciConfig.GetBusData && ctx->PciConfig.SetBusData)
    {
        UCHAR pgctl = 0;
        UCHAR cgctl = 0;

        ctx->PciConfig.GetBusData(ctx->PciConfig.Context, PCI_WHICHSPACE_CONFIG,
            &pgctl, AZX_PCIREG_PGCTL, sizeof(pgctl));
        pgctl &= (UCHAR)(~AZX_PGCTL_LSRMD_MASK & 0xFFu);
        ctx->PciConfig.SetBusData(ctx->PciConfig.Context, PCI_WHICHSPACE_CONFIG,
            &pgctl, AZX_PCIREG_PGCTL, sizeof(pgctl));

        ctx->PciConfig.GetBusData(ctx->PciConfig.Context, PCI_WHICHSPACE_CONFIG,
            &cgctl, AZX_PCIREG_CGCTL, sizeof(cgctl));
        cgctl |= (UCHAR)AZX_CGCTL_MISCBDCGE_MASK;
        ctx->PciConfig.SetBusData(ctx->PciConfig.Context, PCI_WHICHSPACE_CONFIG,
            &cgctl, AZX_PCIREG_CGCTL, sizeof(cgctl));

        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
            "AVS: PGCTL=0x%02X CGCTL=0x%02X after clock/reset-gating fixup\n",
            pgctl, cgctl));
    }
    else
    {
        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "AVS: PciConfig interface not available -- skipping PGCTL/CGCTL step\n"));
    }

    // Step 0.5: enable the Processing Pipe global gate (GPROCEN + PIE).
    if (ctx->Ppcap != NULL)
    {
        PUCHAR ppcap = (PUCHAR)ctx->Ppcap;
        ULONG ppctl = READ_REGISTER_ULONG((PULONG)(ppcap + AZX_REG_PP_PPCTL));
        ppctl |= AZX_PPCTL_GPROCEN | AZX_PPCTL_PIE;
        WRITE_REGISTER_ULONG((PULONG)(ppcap + AZX_REG_PP_PPCTL), ppctl);

        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
            "AVS: PPCTL=0x%08X after GPROCEN/PIE enable\n",
            READ_REGISTER_ULONG((PULONG)(ppcap + AZX_REG_PP_PPCTL))));
    }
    else
    {
        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "AVS: Ppcap not available -- skipping GPROCEN/PIE step\n"));
    }

    // Step 0.9: a DSP still powered from a previous session (driver
    // restart: disable/enable, update) must be power-cycled first, else
    // its stale FW_STATUS (0xF) satisfies the ROM/basefw polls below while
    // the ROM hasn't actually re-run -- and FW_READY never comes.
    val = READ_REGISTER_ULONG(adspcsReg);
    if (val != 0xFFFFFFFFu && (val & AVS_ADSPCS_CPA_MASK(coreMask)) != 0)
    {
        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
            "AVS: DSP still powered (ADSPCS=0x%08X, FW_STATUS=0x%08X) -- power-cycling first\n",
            val, READ_REGISTER_ULONG((PULONG)(ctx->DspBar.Base.baseptr + AVS_FW_REG_STATUS))));
        AvsDspShutdown(ctx, AVS_ALL_CORES_MASK);
    }

    // Step 1: request power for this core mask (set SPA).
    val = READ_REGISTER_ULONG(adspcsReg);
    val |= AVS_ADSPCS_SPA_MASK(coreMask);
    WRITE_REGISTER_ULONG(adspcsReg, val);

    {
        ULONG afterWrite = READ_REGISTER_ULONG(adspcsReg);
        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
            "AVS: wrote SPA (mask 0x%X), immediate readback ADSPCS=0x%08X (SPA bit %s)\n",
            coreMask, afterWrite,
            (afterWrite & AVS_ADSPCS_SPA_MASK(coreMask)) ? "SET" : "NOT set"));
    }

    // Step 2: wait for CPA to reflect SPA -- core power domain is live.
    status = AvsPollAdspcs(
        ctx,
        AVS_ADSPCS_CPA_MASK(coreMask),
        AVS_ADSPCS_CPA_MASK(coreMask),
        AVS_ADSPCS_TIMEOUT_US);
    if (!NT_SUCCESS(status))
    {
        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "AVS: core power-up failed for mask 0x%X\n", coreMask));
        return status;
    }

    // Step 3+4: hold core in stall and reset before releasing reset.
    val = READ_REGISTER_ULONG(adspcsReg);
    val |= AVS_ADSPCS_CSTALL_MASK(coreMask);
    val |= AVS_ADSPCS_CRST_MASK(coreMask);
    WRITE_REGISTER_ULONG(adspcsReg, val);

    // Step 4.5: the core is powered, in reset and stalled -- clear the
    // SRAM status word so only what the ROM writes from now on counts
    // (belt and braces next to Step 0.9; the ROM rewrites it anyway).
    WRITE_REGISTER_ULONG((PULONG)(ctx->DspBar.Base.baseptr + AVS_FW_REG_STATUS), 0);

    // Step 5: deassert CRST -- core exits reset (still stalled). CSTALL
    // is intentionally left asserted here -- CLDMA must be reset and
    // programmed by the caller while the core is still stalled, before
    // AvsDspReleaseStallAndWaitRom() is called.
    val = READ_REGISTER_ULONG(adspcsReg);
    val &= ~AVS_ADSPCS_CRST_MASK(coreMask);
    WRITE_REGISTER_ULONG(adspcsReg, val);

    status = AvsPollAdspcs(
        ctx,
        AVS_ADSPCS_CRST_MASK(coreMask),
        0,
        AVS_ADSPCS_TIMEOUT_US);
    if (!NT_SUCCESS(status))
    {
        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "AVS: core failed to leave reset for mask 0x%X\n", coreMask));
        return status;
    }

    AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "AVS: core mask 0x%X out of reset (still stalled), ADSPCS=0x%08X -- CLDMA can now be programmed\n",
        coreMask, READ_REGISTER_ULONG(adspcsReg)));

    return STATUS_SUCCESS;
}

NTSTATUS
AvsDspReleaseStallAndWaitRom(
    _In_ PAVS_DEVICE_CONTEXT ctx,
    ULONG coreMask
    )
{
    if (ctx->DspBar.Base.Base == NULL)
    {
        return STATUS_DEVICE_NOT_READY;
    }

    PULONG adspcsReg = (PULONG)(ctx->DspBar.Base.baseptr + AVS_ADSP_REG_ADSPCS);
    NTSTATUS status;
    ULONG val;

    // Step 6: deassert CSTALL -- core starts executing its on-die ROM,
    // which itself polls for CLDMA completion before jumping to the
    // firmware image. CLDMA must already be reset+programmed (but not
    // triggered) by this point.
    val = READ_REGISTER_ULONG(adspcsReg);
    val &= ~AVS_ADSPCS_CSTALL_MASK(coreMask);
    WRITE_REGISTER_ULONG(adspcsReg, val);

    status = AvsPollAdspcs(
        ctx,
        AVS_ADSPCS_CSTALL_MASK(coreMask),
        0,
        AVS_ADSPCS_TIMEOUT_US);
    if (!NT_SUCCESS(status))
    {
        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "AVS: core failed to leave stall for mask 0x%X\n", coreMask));
        return status;
    }

    AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "AVS: core mask 0x%X fully running (ROM), ADSPCS=0x%08X -- waiting for ROM_INIT_DONE\n",
        coreMask, READ_REGISTER_ULONG(adspcsReg)));

    // Enable the IPC interrupt gates now -- confirmed from upstream
    // avs_cldma_load_basefw() (loader.c): int_control(true) is called
    // right here, AFTER stall(false)/CSTALL-deassert, BEFORE polling
    // ROM_INIT_DONE. Enabling them earlier (before the power/clock-gating
    // sequence above) does not stick on a cold boot.
    //
    // Two gates must be opened, not one -- confirmed from upstream
    // avs_dsp_interrupt_control() (ipc.c), whose comment states this
    // explicitly ("No particular bit setting order. All of these are
    // required to have a functional SW <-> FW communication."):
    //   1. ADSPIC.IPC       -- top-level "let the IPC source wake ADSPIS"
    //   2. HIPCCTL.BUSY|DONE -- the HIPC block's own local enable, which
    //      gates whether the DSP ever actually asserts ADSPIS_IPC in
    //      the first place.
    {
        PULONG adspicReg = (PULONG)(ctx->DspBar.Base.baseptr + AVS_ADSP_REG_ADSPIC);
        ULONG adspic = READ_REGISTER_ULONG(adspicReg);
        adspic |= AVS_ADSPIC_IPC;
        WRITE_REGISTER_ULONG(adspicReg, adspic);

        PULONG hipcctlReg = (PULONG)(ctx->DspBar.Base.baseptr + SKL_ADSP_REG_HIPCCTL);
        ULONG hipcctl = READ_REGISTER_ULONG(hipcctlReg);
        hipcctl |= AVS_ADSP_HIPCCTL_BUSY | AVS_ADSP_HIPCCTL_DONE;
        WRITE_REGISTER_ULONG(hipcctlReg, hipcctl);

        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
            "AVS: ADSPIC=0x%08X HIPCCTL=0x%08X after enabling IPC interrupt gates (post-CSTALL)\n",
            READ_REGISTER_ULONG(adspicReg), READ_REGISTER_ULONG(hipcctlReg)));
    }

    // Step 7: poll the SRAM-based firmware status register (NOT ADSPCS)
    // for the ROM's own init-done flag -- confirmed from upstream
    // loader.c: this happens BEFORE the actual CLDMA transfer is
    // allowed to start.
    {
        ULONG elapsed = 0;
        PULONG fwStatusReg = (PULONG)(ctx->DspBar.Base.baseptr + AVS_FW_REG_STATUS);

        for (;;)
        {
            ULONG fwStatus = READ_REGISTER_ULONG(fwStatusReg);

            // Same UINT_MAX guard as AvsPollAdspcs above -- this is the
            // exact false-positive observed on a genuinely cold boot:
            // FW_STATUS momentarily read 0xFFFFFFFF (register space
            // not yet live) during this transition, which trivially
            // satisfies "(fwStatus & 0x1) == 0x1" and caused this poll
            // to report ROM_INIT_DONE on garbage rather than real data.
            if (fwStatus != 0xFFFFFFFFu && (fwStatus & AVS_ROM_INIT_DONE) == AVS_ROM_INIT_DONE)
            {
                AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                    "AVS: ROM_INIT_DONE, FW_STATUS=0x%08X\n", fwStatus));
                return STATUS_SUCCESS;
            }

            if (elapsed >= SKL_ROM_INIT_TIMEOUT_US)
            {
                ULONG fwError = READ_REGISTER_ULONG(
                    (PULONG)(ctx->DspBar.Base.baseptr + AVS_FW_REG_ERROR));
                AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                    "AVS: ROM init timeout, FW_STATUS=0x%08X, FW_ERROR=0x%08X\n",
                    fwStatus, fwError));
                return STATUS_TIMEOUT;
            }

            KeStallExecutionProcessor(AVS_ROM_INIT_POLLING_US);
            elapsed += AVS_ROM_INIT_POLLING_US;
        }
    }
}
