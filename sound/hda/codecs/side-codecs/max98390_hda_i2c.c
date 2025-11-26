// SPDX-License-Identifier: GPL-2.0-only
//
// max98390_hda_i2c.c -- I2C wrapper for MAX98390 HDA side codec driver
//

#include <linux/acpi.h>
#include <linux/i2c.h>
#include <linux/module.h>

#include "max98390_hda.h"

static int max98390_hda_i2c_probe(struct i2c_client *clt)
{
	return max98390_hda_probe(&clt->dev, clt->name, clt->addr, clt->irq);
}

static void max98390_hda_i2c_remove(struct i2c_client *clt)
{
	max98390_hda_remove(&clt->dev);
}

static const struct i2c_device_id max98390_hda_i2c_id[] = {
	{ "max98390-hda", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, max98390_hda_i2c_id);

static const struct acpi_device_id max98390_hda_acpi_match[] = {
	{ "MAX98390", 0 },
	{ }
};
MODULE_DEVICE_TABLE(acpi, max98390_hda_acpi_match);

static struct i2c_driver max98390_hda_i2c_driver = {
	.driver = {
		.name = "max98390-hda",
		.acpi_match_table = max98390_hda_acpi_match,
		.pm = &max98390_hda_pm_ops,
	},
	.probe = max98390_hda_i2c_probe,
	.remove = max98390_hda_i2c_remove,
	.id_table = max98390_hda_i2c_id,
};
module_i2c_driver(max98390_hda_i2c_driver);

MODULE_DESCRIPTION("HD-audio MAX98390 I2C side codec driver");
MODULE_AUTHOR("Lyapsus");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("SND_HDA_SCODEC_MAX98390");
