/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * soc_compat.h -- the small slice of the ALSA/ASoC object model that the
 * ported topology.c / path.c touch. Field names match the kernel so the
 * ported files compile unchanged; everything else of the real structures
 * is omitted. Objects are created by our own topology loader
 * (avs_tplg_loader.c) instead of sound/soc/soc-topology.c.
 */
#ifndef AVS_PORT_SOC_COMPAT_H
#define AVS_PORT_SOC_COMPAT_H

#include "linux_compat.h"
#include "asoc.h"

/* ---- generic bit helpers used by utils.h ---- */
static inline unsigned int hweight_long(unsigned long w)
{
	unsigned int n = 0;

	while (w) {
		n += (unsigned int)(w & 1);
		w >>= 1;
	}
	return n;
}

static inline unsigned long __ffs(unsigned long w)
{
	unsigned long n = 0;

	if (!w)
		return 0;
	while (!(w & 1)) {
		w >>= 1;
		n++;
	}
	return n;
}

/* ---- PCM direction ---- */
#define SNDRV_PCM_STREAM_PLAYBACK 0
#define SNDRV_PCM_STREAM_CAPTURE  1

/* ---- machine description (sound/soc-acpi.h) ---- */
struct snd_soc_acpi_mach_params {
	u32 i2s_link_mask;
};

struct snd_soc_acpi_mach {
	struct snd_soc_acpi_mach_params mach_params;
	void *pdata;			/* struct avs_mach_pdata * */
};

/* struct device carries the machine description ("platform data"). */
struct avs_port_device {
	struct device base;
	struct snd_soc_acpi_mach *mach;
};
static inline struct snd_soc_acpi_mach *dev_get_platdata(struct device *dev)
{
	return container_of(dev, struct avs_port_device, base)->mach;
}

/* ---- card / component ---- */
struct snd_soc_card {
	struct device *dev;
};

struct snd_soc_component {
	struct device *dev;
	struct snd_soc_card *card;
	void *priv;			/* struct avs_soc_component * */
};

static inline void *snd_soc_component_to_priv(struct snd_soc_component *comp)
{
	return comp->priv;
}

/* ---- controls ---- */
struct snd_soc_dobj {
	void *private;
};

struct soc_mixer_control {
	int min, max, platform_max;
	int reg, rreg;
	unsigned int shift, rshift;
	unsigned int sign_bit;
	unsigned int invert:1;
	unsigned int autodisable:1;
	unsigned int num_channels;
	struct snd_soc_dobj dobj;
};

struct snd_ctl_elem_id {
	char name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
};

/* AVS-PORT: uintptr_t, not unsigned long -- long is 32-bit on Windows x64 */
struct snd_kcontrol_new {
	uintptr_t private_value;
};

struct snd_kcontrol {
	struct snd_ctl_elem_id id;
	uintptr_t private_value;	/* struct soc_mixer_control * */
};

struct snd_ctl_elem_value;
struct snd_ctl_elem_info;

/* ---- DAPM ---- */
struct snd_soc_dapm_widget {
	char *name;
	void *priv;
	int num_kcontrols;
	struct snd_kcontrol **kcontrols;
	unsigned int ignore_suspend:1;
	unsigned int no_wname_in_kcontrol_name:1;
};

struct snd_soc_dapm_route {
	const char *sink;
	const char *control;
	const char *source;
};

/* ---- PCM hw params ----
 * Upstream path.c only asks four questions of snd_pcm_hw_params; the glue
 * passes this plain struct instead. */
struct snd_pcm_hw_params {
	unsigned int rate;
	unsigned int channels;
	unsigned int physical_width;	/* container bits */
	unsigned int msbits;		/* valid bits */
};
static inline unsigned int params_rate(const struct snd_pcm_hw_params *p) { return p->rate; }
static inline unsigned int params_channels(const struct snd_pcm_hw_params *p) { return p->channels; }
static inline unsigned int params_physical_width(const struct snd_pcm_hw_params *p)
{
	return p->physical_width;
}
static inline unsigned int snd_pcm_hw_params_bits(const struct snd_pcm_hw_params *p)
{
	return p->msbits;
}

struct snd_pcm_hw_constraint_list {
	unsigned int count;
	const unsigned int *list;
	unsigned int mask;
};

/* ---- DAI / links: only referenced by topology callbacks we never invoke ---- */
struct snd_soc_dai_ops { int unused; }; /* never invoked on Windows */
struct snd_soc_pcm_stream {
	u64 subformats;
};
struct snd_soc_dai_driver {
	const struct snd_soc_dai_ops *ops;
	struct snd_soc_pcm_stream capture;
	struct snd_soc_pcm_stream playback;
};
struct snd_soc_dai;
struct snd_soc_dai_link {
	unsigned int ignore_suspend:1;
	unsigned int no_pcm:1;
	unsigned int nonatomic:1;
	unsigned int dpcm_merged_format:1;
	int trigger[2];
};
#define SND_SOC_DPCM_TRIGGER_PRE 1
#define SNDRV_PCM_SUBFMTBIT_MSBITS_20  (1ULL << 1)
#define SNDRV_PCM_SUBFMTBIT_MSBITS_24  (1ULL << 2)
#define SNDRV_PCM_SUBFMTBIT_MSBITS_MAX (1ULL << 3)

/* ---- soc-topology ops table (include/sound/soc-topology.h) ---- */
struct snd_soc_tplg_kcontrol_ops {
	u32 id;
	int (*get)(struct snd_kcontrol *, struct snd_ctl_elem_value *);
	int (*put)(struct snd_kcontrol *, struct snd_ctl_elem_value *);
	int (*info)(struct snd_kcontrol *, struct snd_ctl_elem_info *);
};

struct snd_soc_tplg_ops {
	int (*control_load)(struct snd_soc_component *, int, struct snd_kcontrol_new *,
			    struct snd_soc_tplg_ctl_hdr *);
	int (*dapm_route_load)(struct snd_soc_component *, int, struct snd_soc_dapm_route *);
	int (*widget_load)(struct snd_soc_component *, int, struct snd_soc_dapm_widget *,
			   struct snd_soc_tplg_dapm_widget *);
	int (*widget_ready)(struct snd_soc_component *, int, struct snd_soc_dapm_widget *,
			    struct snd_soc_tplg_dapm_widget *);
	int (*dai_load)(struct snd_soc_component *, int, struct snd_soc_dai_driver *,
			struct snd_soc_tplg_pcm *, struct snd_soc_dai *);
	int (*link_load)(struct snd_soc_component *, int, struct snd_soc_dai_link *,
			 struct snd_soc_tplg_link_config *);
	int (*manifest)(struct snd_soc_component *, int, struct snd_soc_tplg_manifest *);
	const struct snd_soc_tplg_kcontrol_ops *io_ops;
	int io_ops_count;
};

#endif
