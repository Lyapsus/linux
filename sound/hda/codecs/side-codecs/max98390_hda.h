/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * HD-audio side codec glue for Maxim MAX98390 smart amplifiers
 */

#ifndef __MAX98390_HDA_H__
#define __MAX98390_HDA_H__

#include <linux/device.h>

int max98390_hda_probe(struct device *dev, const char *device_name,
			 int id, int irq);
void max98390_hda_remove(struct device *dev);

extern const struct dev_pm_ops max98390_hda_pm_ops;

#endif /* __MAX98390_HDA_H__ */
