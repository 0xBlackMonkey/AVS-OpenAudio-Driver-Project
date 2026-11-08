// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 AVS OpenAudio Driver Project contributors
//
// avs_regs.h -- Skylake/Kaby Lake cAVS DSP registers (BAR4) and the HDA
// bits the DSP bring-up touches.
//
// Offsets and bit masks are hardware facts, taken from the Linux AVS
// driver (sound/soc/intel/avs/registers.h, skl.c, cldma.c) and written
// here with Windows types. All of them are exercised on real hardware
// (Asus C434, PCI 8086:9D71): DSP boot, HIPC IPC, CLDMA firmware load.

#pragma once

// ---- General DSP registers (generic across AVS generations) -------------
#define AVS_ADSP_GEN_BASE      0x00

#define AVS_ADSP_REG_ADSPCS    (AVS_ADSP_GEN_BASE + 0x04)
#define AVS_ADSP_REG_ADSPIC    (AVS_ADSP_GEN_BASE + 0x08)
#define AVS_ADSP_REG_ADSPIS    (AVS_ADSP_GEN_BASE + 0x0C)

#define AVS_ADSPIC_IPC         (1u << 0)
#define AVS_ADSPIC_CLDMA       (1u << 1)
#define AVS_ADSPIS_IPC         (1u << 0)
#define AVS_ADSPIS_CLDMA       (1u << 1)

// Core power/reset control macros (cm = core mask)
#define AVS_ADSPCS_CRST_MASK(cm)   ((ULONG)(cm))
#define AVS_ADSPCS_CSTALL_MASK(cm) ((ULONG)(cm) << 8)
#define AVS_ADSPCS_SPA_MASK(cm)    ((ULONG)(cm) << 16)
#define AVS_ADSPCS_CPA_MASK(cm)    ((ULONG)(cm) << 24)
// Skylake/Kaby Lake cAVS: 2 DSP cores (HW_CONFIG dsp_cores=2 on the C434);
// all of them go down on shutdown, like GENMASK(dsp_cores - 1, 0) upstream.
#define AVS_ALL_CORES_MASK         0x3u

#define AVS_ADSPCS_INTERVAL_US     500
#define AVS_ADSPCS_TIMEOUT_US      50000  // 50ms -- matches current upstream dsp.c,
                                            // not the older 10ms value from registers.h

// Skylake-Y (Sunrise Point-LP) has exactly one DSP core -- confirmed
// by core_init_mask=1 for skl_desc in the reference Linux driver.
#define AVS_MAIN_CORE_MASK         (1u << 0)

// ---- PCI config-space power/clock gating (BAR-independent) --------------
#define AZX_PCIREG_PGCTL           0x44
#define AZX_PCIREG_CGCTL           0x48
#define AZX_PGCTL_LSRMD_MASK       (1u << 4)
#define AZX_CGCTL_MISCBDCGE_MASK   (1u << 6)

// ---- HDA Processing Pipe Capability (PPCTL) ------------------------------
// Global-enable gate that must be set before the DSP power domain
// (ADSPCS SPA/CPA) responds -- confirmed from upstream core.c calling
// snd_hdac_ext_bus_ppcap_enable()/ppcap_int_enable() immediately before
// avs_dsp_first_boot_firmware(). sklhdaudbus hands us the ppcap pointer
// via GetResources but does not toggle this bit itself -- that is the
// AVS-specific driver's job.
#define AZX_REG_PP_PPCTL           0x04
#define AZX_PPCTL_GPROCEN          (1u << 30)
#define AZX_PPCTL_PIE              (1u << 31)

// ---- Firmware status (SRAM window, not BAR4-direct MMIO) -----------------
// Confirmed from upstream registers.h + loader.c: the ROM/firmware status
// lives at a fixed SRAM offset, not among the small BAR4 registers we've
// used so far. This is what avs_cldma_load_basefw() polls BEFORE and
// AFTER the actual CLDMA transfer -- a step our earlier attempt was
// missing entirely, which explains why LPIB never advanced: we never
// waited for the on-die ROM to report itself ready to receive code.
#define SKL_ADSP_SRAM_BASE_OFFSET  0x8000
#define AVS_FW_REG_STATUS          (SKL_ADSP_SRAM_BASE_OFFSET + 0x0)
#define AVS_FW_REG_ERROR           (SKL_ADSP_SRAM_BASE_OFFSET + 0x4)

// ---- SRAM window layout (Skylake) ----------------------------------------
// Confirmed from upstream registers.h. Three fixed-size windows starting
// at SKL_ADSP_SRAM_BASE_OFFSET, each SKL_ADSP_SRAM_WINDOW_SIZE apart:
//   window 0 (AVS_FW_REGS_WINDOW / AVS_UPLINK_WINDOW): first 4K holds the
//     FW_STATUS/FW_ERROR registers (AVS_FW_REG_STATUS/_ERROR above); the
//     remaining 4K of the SAME window is the uplink mailbox -- where the
//     DSP places the payload of whatever it just sent us (reply or
//     notification), readable right after we see HIPCT_BUSY/a completed
//     reply.
//   window 1 (AVS_DOWNLINK_WINDOW): host->DSP payload mailbox -- where we
//     must place request payloads (e.g. LARGE_CONFIG_SET) before sending.
//   window 2 (AVS_DEBUG_WINDOW): firmware log/debug window, unused so far.
#define SKL_ADSP_SRAM_WINDOW_SIZE  0x2000
#define AVS_WINDOW_CHUNK_SIZE      0x1000   // == AVS_FW_REGS_SIZE == AVS_MAILBOX_SIZE
#define AVS_MAILBOX_SIZE           AVS_WINDOW_CHUNK_SIZE

#define AVS_FW_REGS_WINDOW         0
#define AVS_UPLINK_WINDOW          AVS_FW_REGS_WINDOW
#define AVS_DOWNLINK_WINDOW        1
#define AVS_DEBUG_WINDOW           2

#define AVS_SRAM_WINDOW_OFFSET(idx) \
    (SKL_ADSP_SRAM_BASE_OFFSET + (SKL_ADSP_SRAM_WINDOW_SIZE * (idx)))

// avs_uplink_addr(adev) == avs_sram_addr(adev, AVS_UPLINK_WINDOW) + AVS_FW_REGS_SIZE
#define AVS_UPLINK_MAILBOX_OFFSET \
    (AVS_SRAM_WINDOW_OFFSET(AVS_UPLINK_WINDOW) + AVS_WINDOW_CHUNK_SIZE)
// avs_downlink_addr(adev) == avs_sram_addr(adev, AVS_DOWNLINK_WINDOW)
#define AVS_DOWNLINK_MAILBOX_OFFSET \
    AVS_SRAM_WINDOW_OFFSET(AVS_DOWNLINK_WINDOW)

#define AVS_ROM_INIT_DONE          0x1
#define AVS_ROM_STS_MASK           0xFFu
#define SKL_ROM_BASEFW_ENTERED     0xF

#define AVS_ROM_INIT_POLLING_US    5
#define SKL_ROM_INIT_TIMEOUT_US    1000000   // 1s
#define AVS_FW_INIT_POLLING_US     500
#define AVS_FW_INIT_TIMEOUT_US     3000000   // 3s
#define AVS_FW_INIT_TIMEOUT_MS     3000      // same value in ms -- confirmed
                                               // from upstream loader.c's
                                               // AVS_FW_INIT_TIMEOUT_MS, used
                                               // for the fw_ready completion
                                               // wait (a separate handshake
                                               // from FW_STATUS polling).

// ---- CLDMA (Code Loader DMA) registers -----------------------------------
// Layout as in upstream avs/cldma.c (and the older skylake/skl-sst-cldma.h,
// sof/intel/hda.h): the loader stream sits at HDA_ADSP_LOADER_BASE 0x80.

#define FW_CL_STREAM_NUMBER        0x1
#define HDA_ADSP_LOADER_BASE       0x80

#define SKL_ADSP_REG_CL_SD_CTL       (HDA_ADSP_LOADER_BASE + 0x00)
#define SKL_ADSP_REG_CL_SD_STS       (HDA_ADSP_LOADER_BASE + 0x03)
#define SKL_ADSP_REG_CL_SD_LPIB      (HDA_ADSP_LOADER_BASE + 0x04)
#define SKL_ADSP_REG_CL_SD_CBL       (HDA_ADSP_LOADER_BASE + 0x08)
#define SKL_ADSP_REG_CL_SD_LVI       (HDA_ADSP_LOADER_BASE + 0x0C)
#define SKL_ADSP_REG_CL_SD_BDLPL     (HDA_ADSP_LOADER_BASE + 0x18)
#define SKL_ADSP_REG_CL_SD_BDLPU     (HDA_ADSP_LOADER_BASE + 0x1C)

#define SKL_ADSP_REG_CL_SPBFIFO           (HDA_ADSP_LOADER_BASE + 0x20)
#define SKL_ADSP_REG_CL_SPBFIFO_SPBFCCTL  (SKL_ADSP_REG_CL_SPBFIFO + 0x4)
#define SKL_ADSP_REG_CL_SPBFIFO_SPIB      (SKL_ADSP_REG_CL_SPBFIFO + 0x8)

// CL_SD_CTL bit layout (32-bit register)
#define CL_SD_CTL_SRST_MASK        (1u << 0)
#define CL_SD_CTL_RUN_MASK         (1u << 1)
#define CL_SD_CTL_IOCE_MASK        (1u << 2)
#define CL_SD_CTL_FEIE_MASK        (1u << 3)
#define CL_SD_CTL_DEIE_MASK        (1u << 4)
#define CL_SD_CTL_STRM_SHIFT       20
#define CL_SD_CTL_STRM_MASK        (0xFu << CL_SD_CTL_STRM_SHIFT)
#define CL_SD_CTL_STRM(x)          (((ULONG)(x) << CL_SD_CTL_STRM_SHIFT) & CL_SD_CTL_STRM_MASK)

// CL_SD_STS (byte register, offset +0x03) -- completion/error bits share
// the same bit positions as their _IE enable counterparts in SD_CTL.
#define CL_SD_STS_FIFOE_MASK       (1u << 3)
#define CL_SD_STS_DESE_MASK        (1u << 4)
#define CL_SD_STS_BCIS_MASK        (1u << 2)  // buffer-completion interrupt status

// BDL base address registers -- lower requires 128-byte alignment (bits 0-6
// reserved/protect); we always allocate page-aligned buffers so this is
// satisfied trivially.
#define CL_SD_BDLPLBA(addr)        ((ULONG)((ULONG_PTR)(addr) & 0xFFFFFF80u))
#define CL_SD_BDLPUBA(addr)        ((ULONG)(((ULONGLONG)(addr) >> 32) & 0xFFFFFFFFu))

#define CL_SPBFIFO_SPBFCCTL_SPIBE_MASK (1u << 0)

#define SKL_ADSPIC_CL_DMA          (1u << 1)  // same bit as AVS_ADSPIC_CLDMA above
#define SKL_ADSPIS_CL_DMA          (1u << 1)

// ---- Skylake-generation HIPC registers (host <-> DSP command/response) --
// From upstream sound/soc/intel/avs/registers.h (SKL_ADSP_* set).

#define SKL_ADSP_IPC_BASE          0x40

#define SKL_ADSP_REG_HIPCT         (SKL_ADSP_IPC_BASE + 0x00)  // DSP -> Host response
#define SKL_ADSP_REG_HIPCTE        (SKL_ADSP_IPC_BASE + 0x04)
#define SKL_ADSP_REG_HIPCI         (SKL_ADSP_IPC_BASE + 0x08)  // Host -> DSP request
#define SKL_ADSP_REG_HIPCIE        (SKL_ADSP_IPC_BASE + 0x0C)
#define SKL_ADSP_REG_HIPCCTL       (SKL_ADSP_IPC_BASE + 0x10)

#define SKL_ADSP_HIPCI_BUSY        (1u << 31)
#define SKL_ADSP_HIPCIE_DONE       (1u << 30)
#define SKL_ADSP_HIPCT_BUSY        (1u << 31)

#define AVS_ADSP_HIPCCTL_BUSY      (1u << 0)
#define AVS_ADSP_HIPCCTL_DONE      (1u << 1)

// Handshake (Linux avs_hipc_spec for SKL), as implemented in avs_ipc.c /
// the ISR in avs_boot.c:
//  host -> DSP: payload into the downlink mailbox, extension into HIPCIE,
//               primary | HIPCI_BUSY into HIPCI; DSP sets HIPCIE_DONE.
//  DSP -> host: ADSPIS_IPC; HIPCT has HIPCT_BUSY + reply/notification,
//               HIPCTE the extension; reply payload read from the uplink
//               mailbox, then HIPCT written back to ack (and HIPCIE_DONE
//               written back to clear it).

// ---- IPC message-layer constants (from upstream messages.h) -------------
// Bitfield layouts confirmed against sound/soc/intel/avs/messages.h,
// union avs_module_msg / avs_reply_msg / avs_notify_msg. All bitfields
// below are packed LSB-first within the 32-bit primary/extension word,
// matching MSVC's x64 bitfield allocation order.

// enum avs_msg_target / avs_msg_direction
#define AVS_FW_GEN_MSG              0u
#define AVS_MOD_MSG                 1u
#define AVS_MSG_REQUEST             0u
#define AVS_MSG_REPLY               1u

// enum avs_global_msg_type (subset)
#define AVS_GLB_NOTIFICATION        27u

// enum avs_module_msg_type (subset)
#define AVS_MOD_INIT_INSTANCE       0u
#define AVS_MOD_LARGE_CONFIG_GET    3u
#define AVS_MOD_BIND                5u

// enum avs_global_msg_type (subset)
#define AVS_GLB_SET_PIPELINE_STATE  19u

// enum avs_pipeline_state
#define AVS_PPL_STATE_INVALID       0u
#define AVS_PPL_STATE_UNINITIALIZED 1u
#define AVS_PPL_STATE_RESET         2u
#define AVS_PPL_STATE_PAUSED        3u
#define AVS_PPL_STATE_RUNNING       4u

// enum avs_notify_msg_type (subset) -- confirmed from upstream messages.h
#define AVS_NOTIFY_PHRASE_DETECTED   4u
#define AVS_NOTIFY_RESOURCE_EVENT    5u
#define AVS_NOTIFY_LOG_BUFFER_STATUS 6u
#define AVS_NOTIFY_FW_READY          8u

// avs_ipc_get_fw_config() always targets the base firmware module
// (module_id=0, instance_id=0) -- confirmed from AVS_BASEFW_MOD_ID/
// AVS_BASEFW_INST_ID in messages.h.
#define AVS_BASEFW_MOD_ID           0u
#define AVS_BASEFW_INST_ID          0u

// enum avs_basefw_runtime_param (subset) -- this is the large_param_id
// for a LARGE_CONFIG_GET request targeting the base firmware module.
// NOT to be confused with AVS_FW_CFG_FW_VERSION below, which is a TLV
// *entry type inside the returned payload*, not a request parameter.
#define AVS_BASEFW_FIRMWARE_CONFIG  7u
#define AVS_BASEFW_MODULES_INFO     9u

// enum avs_fw_cfg_params (subset) -- TLV type tag found inside the
// FIRMWARE_CONFIG payload once it's been received and parsed.
#define AVS_FW_CFG_FW_VERSION       0u
