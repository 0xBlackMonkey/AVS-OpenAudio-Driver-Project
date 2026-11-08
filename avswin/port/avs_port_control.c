// SPDX-License-Identifier: GPL-2.0-only
//
// avs_port_control.c -- stand-ins for sound/soc/intel/avs/control.c.
//
// Upstream these are ALSA kcontrol get/put/info callbacks (alsamixer).
// topology.c only stores their addresses in its io_ops table; on Windows
// volume comes from the glue (later: the WaveRT/KS volume node) through
// avs_port_tplg_set_volume() in avs_tplg_loader.c, which updates the same
// avs_control_data values that path.c reads when it creates PEAKVOL.

#include "avs.h"
#include "control.h"

int avs_control_volume_get(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *uctl)
{
	(void)kctl; (void)uctl;
	return -ENOTSUPP;
}

int avs_control_volume_put(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *uctl)
{
	(void)kctl; (void)uctl;
	return -ENOTSUPP;
}

int avs_control_volume_info(struct snd_kcontrol *kctl, struct snd_ctl_elem_info *uinfo)
{
	(void)kctl; (void)uinfo;
	return -ENOTSUPP;
}

int avs_control_mute_get(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *uctl)
{
	(void)kctl; (void)uctl;
	return -ENOTSUPP;
}

int avs_control_mute_put(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *uctl)
{
	(void)kctl; (void)uctl;
	return -ENOTSUPP;
}

int avs_control_mute_info(struct snd_kcontrol *kctl, struct snd_ctl_elem_info *uinfo)
{
	(void)kctl; (void)uinfo;
	return -ENOTSUPP;
}
