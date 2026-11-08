// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 AVS OpenAudio Driver Project contributors
//
// avs_hdalink.c -- HD-Audio Multiple Links (ML) capability discovery
// and per-link power sequencing, operating on BAR0 (the HDA
// controller's own register space, informational-only until now).
//
// sklhdaudbus discovers the ML capability pointer for its own
// purposes (an unrelated graphics-power PnP heuristic -- confirmed
// by reading fdo.cpp's only use of fdoCtx->mlcap) but never powers
// up any individual SSP/I2S link itself. That link power-up is a
// prerequisite the DSP firmware enforces before it will attach a
// gateway to that link (a COPIER module targeting
// AVS_DMA_I2S_LINK_OUTPUT/_INPUT) -- confirmed empirically on this
// hardware: the link-copier's INIT_INSTANCE failed with
// MOD_NOT_INITIALIZED (status=104) while the otherwise-identical
// host-copier (which needs no external link) succeeded.
//
// Capability-list walk mirrors sklhdaudbus's own working code
// (fdo.cpp) and upstream's snd_hdac_bus_parse_capabilities()
// (sound/hda/hdac_controller.c). Per-link register layout and the
// SPA/CPA power handshake (same conceptual pattern as ADSPCS's own
// SPA/CPA, just a different register/domain) are confirmed against
// include/sound/hda_register.h and sound/pci/hda/hda_intel.c's
// intel_ml_lctl_set_power().

#include "driver.h"

#define AZX_REG_LLCH                0x14u

#define AZX_CAP_HDR_ID_OFF          16
#define AZX_CAP_HDR_ID_MASK         (0xFFFu << AZX_CAP_HDR_ID_OFF)
#define AZX_CAP_HDR_NXT_PTR_MASK    0xFFFFu

#define AZX_ML_CAP_ID               0x2u

#define AZX_ML_BASE                 0x40u
#define AZX_ML_INTERVAL             0x40u

#define AZX_REG_ML_LCTL             0x04u
#define AZX_ML_LCTL_CPA             (1u << 23)
#define AZX_ML_LCTL_SPA             (1u << 16)

#define AVS_HDA_LINK_POWER_TIMEOUT_US   50000  // 50ms, matches AVS_ADSPCS_TIMEOUT_US convention
#define AVS_HDA_LINK_POWER_POLL_US      500

// Walks BAR0's linked capability list looking for the Multiple Links
// (ML) capability. Returns a pointer to it, or NULL if not found
// (guard-limited to 32 entries -- a real capability list is always
// short, so hitting the guard means something is wrong, not that we
// need to search further).
static PUCHAR
AvsHdaFindMlcap(
    _In_ PAVS_DEVICE_CONTEXT ctx
    )
{
    if (ctx->HdaBar.Base.Base == NULL)
    {
        return NULL;
    }

    PUCHAR bar0 = ctx->HdaBar.Base.baseptr;
    USHORT offset = READ_REGISTER_USHORT((PUSHORT)(bar0 + AZX_REG_LLCH));

    for (ULONG guard = 0; guard < 32; guard++)
    {
        ULONG curCap = READ_REGISTER_ULONG((PULONG)(bar0 + offset));

        if (curCap == 0xFFFFFFFFu)
        {
            break;
        }

        ULONG capId = (curCap & AZX_CAP_HDR_ID_MASK) >> AZX_CAP_HDR_ID_OFF;

        if (capId == AZX_ML_CAP_ID)
        {
            return bar0 + offset;
        }

        ULONG nextPtr = curCap & AZX_CAP_HDR_NXT_PTR_MASK;
        if (nextPtr == 0)
        {
            break;
        }
        offset = (USHORT)nextPtr;
    }

    return NULL;
}

// Powers up a single SSP/I2S link (by its multi-link index, matching
// NHLT's virtual_bus_id) and waits for CPA to confirm -- same
// write-then-poll pattern already used for ADSPCS's SPA/CPA, just a
// different register/domain (BAR0 multi-link, not the DSP core).
NTSTATUS
AvsHdaLinkPowerUp(
    _In_ PAVS_DEVICE_CONTEXT ctx,
    _In_ ULONG LinkIndex
    )
{
    PUCHAR mlcap = AvsHdaFindMlcap(ctx);
    if (mlcap == NULL)
    {
        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "AVS: HDA link power-up -- ML capability not found in BAR0\n"));
        return STATUS_NOT_FOUND;
    }

    PULONG lctlReg = (PULONG)(mlcap + AZX_ML_BASE +
        (LinkIndex * AZX_ML_INTERVAL) + AZX_REG_ML_LCTL);

    ULONG val = READ_REGISTER_ULONG(lctlReg);
    val |= AZX_ML_LCTL_SPA;
    WRITE_REGISTER_ULONG(lctlReg, val);

    ULONG elapsed = 0;
    for (;;)
    {
        ULONG cur = READ_REGISTER_ULONG(lctlReg);
        if (cur & AZX_ML_LCTL_CPA)
        {
            AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                "AVS: HDA link %lu powered up, LCTL=0x%08X\n", LinkIndex, cur));
            return STATUS_SUCCESS;
        }

        if (elapsed >= AVS_HDA_LINK_POWER_TIMEOUT_US)
        {
            AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                "AVS: HDA link %lu power-up timeout, LCTL=0x%08X\n", LinkIndex, cur));
            return STATUS_TIMEOUT;
        }

        KeStallExecutionProcessor(AVS_HDA_LINK_POWER_POLL_US);
        elapsed += AVS_HDA_LINK_POWER_POLL_US;
    }
}
