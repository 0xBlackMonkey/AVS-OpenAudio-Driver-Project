/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * avs_port_bridge.h -- the ONLY header shared between the two halves of the
 * driver:
 *
 *   Linux side   (port/ *.c: ported AVS core, includes linux_compat.h)
 *   Windows side (avs_boot.c, avs_port_win.c, *.cpp: includes ntddk/portcls)
 *
 * Plain C types only, so neither side's headers leak into the other.
 */
#ifndef AVS_PORT_BRIDGE_H
#define AVS_PORT_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== implemented by the Windows side (or the harness) */

/*
 * Send one IPC request and wait for its reply.
 *   primary/ext       : header words (BUSY bit is added by the transport)
 *   payload/size      : request payload, written to the downlink mailbox
 *   rx_expect         : payload bytes the caller expects back (non-LARGE_
 *                       CONFIG_GET replies); LARGE_CONFIG_GET replies use the
 *                       size reported in the reply extension instead
 *   rx_buf/rx_size    : reply payload copied from the uplink mailbox BEFORE
 *                       the reply is acknowledged (may be NULL)
 *   reply_primary/ext : reply header words
 * Returns 0 when a reply arrived (inspect reply_primary[23:0] for firmware
 * status), negative errno-style value on transport failure (-110 timeout).
 */
int avs_bridge_ipc_send(void *host, unsigned int primary, unsigned int ext,
			const void *payload, unsigned int payload_size,
			unsigned int rx_expect, void *rx_buf, unsigned int *rx_size,
			unsigned int *reply_primary, unsigned int *reply_ext,
			unsigned int timeout_ms);

/* ============================== implemented by the core (avs_port_core.c) */

struct avs_core;

struct avs_core_fmt {
	unsigned int rate;
	unsigned int channels;
	unsigned int bits;		/* container */
	unsigned int valid_bits;
};

/* NHLT blob as handed over by the bus driver (kept by reference). */
void avs_core_set_nhlt(const void *nhlt, unsigned long long size);

/* Queries FIRMWARE_CONFIG, HARDWARE_CONFIG and MODULES_INFO. DSP must be up. */
int avs_core_create(void *host, struct avs_core **out);
void avs_core_destroy(struct avs_core *core);

/* @i2s_link_mask: SSP port(s) this codec topology is wired to (BIT(n)=SSPn). */
int avs_core_load_tplg(struct avs_core *core, const void *data, unsigned long size,
		       unsigned int i2s_link_mask);

/*
 * Finds a playback FE path template: @fe_name, or (NULL/"") the first
 * template named "*p_fe". Writes its name and its FE stream format.
 */
int avs_core_find_playback(struct avs_core *core, const char *fe_name,
			   char *name_out, unsigned int name_len,
			   struct avs_core_fmt *fe_fmt);

/* 0..100 % of each "Volume" control's topology maximum. Applies to paths
 * created afterwards. Returns number of controls updated. */
int avs_core_set_control_percent(struct avs_core *core, const char *name_part,
				 unsigned int percent);
int avs_core_set_volume_percent(struct avs_core *core, unsigned int percent);

/*
 * One playback stream = one FE path + the BE path its pipeline binds to.
 * Several streams may be open at once as long as they use different BEs
 * (e.g. ssp0p_fe -> speakers and ssp1p_fe -> headphones).
 *
 * Linux order (pcm.c + soc-pcm.c, FE links use SND_SOC_DPCM_TRIGGER_PRE):
 *   open : hw_params: BE path create (dma_id 0), FE path create (@dma_id =
 *          HDA host stream_tag - 1) + bind; prepare: BE reset+pause, then FE
 *   run  : caller starts host DMA FIRST; then FE pause+run, BE pause+run
 *   stop : BE pause+reset, FE pause; caller then stops host DMA and calls
 *   reset: FE reset                     (STOP/PAUSE_PUSH: BE first, then FE)
 *   close: hw_free: FE unbind+free, then BE free
 * A stopped+reset stream can be run again (PAUSE_RELEASE does the same).
 */
struct avs_core_stream;

int avs_core_stream_open(struct avs_core *core, const char *fe_name, unsigned int dma_id,
			 struct avs_core_stream **out);
int avs_core_stream_run(struct avs_core *core, struct avs_core_stream *s);
int avs_core_stream_stop(struct avs_core *core, struct avs_core_stream *s);
int avs_core_stream_reset(struct avs_core *core, struct avs_core_stream *s);
void avs_core_stream_close(struct avs_core *core, struct avs_core_stream *s);

#ifdef __cplusplus
}
#endif

#endif
