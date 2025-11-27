// SPDX-License-Identifier: GPL-2.0-only
//
// max98390_hda.c -- MAX98390 HDA side codec driver
//

#include <linux/acpi.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/dmi.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <sound/hda_codec.h>

#include "hda_component.h"
#include "max98390_hda.h"
#include "../generic.h"
#include "../../soc/codecs/max98390.h"

#define MAX98390_HDA_I2C_BASE_ADDR	0x38
#define MAX98390_HDA_MAX_AMPS		4

#define MAX98390_ACPI_PROP_DEV_INDEX	"maxim,dev-index"
#define MAX98390_ACPI_PROP_SPK_POS	"maxim,speaker-position"
#define MAX98390_ACPI_PROP_SPK_ID	"maxim,speaker-id"

#if IS_ENABLED(CONFIG_DMI)
static const struct dmi_system_id max98390_dsm_dmi_table[] = {
	{
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "SAMSUNG ELECTRONICS"),
			DMI_MATCH(DMI_PRODUCT_NAME, "960QGK"),
		},
		.driver_data = (void *)"dsm_param_samsung_galaxybook4.bin",
	},
	{
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "SAMSUNG ELECTRONICS"),
			DMI_MATCH(DMI_PRODUCT_NAME, "940XGK"),
		},
		.driver_data = (void *)"dsm_param_samsung_galaxybook4.bin",
	},
	{}
};
#endif

struct max98390_hda {
	struct device *dev;
	struct regmap *regmap;
	struct gpio_desc *reset_gpio;
	struct acpi_device *adev;
	struct hda_codec *codec;
	int index;
	int channel;
	bool playing;
	bool suspended;
};

static const struct regmap_config max98390_hda_regmap_i2c = {
	.reg_bits = 16,
	.val_bits = 8,
	.max_register = MAX98390_R24FF_REV_ID,
};

static void max98390_hda_hw_reset(struct max98390_hda *ctx)
{
	if (!ctx->reset_gpio)
		return;

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(1000, 2000);
}

static int max98390_hda_program_slots(struct max98390_hda *ctx, u32 v_slot, u32 i_slot)
{
	int ret;

	ret = regmap_write(ctx->regmap, MAX98390_PCM_CH_SRC_2,
			((i_slot & 0xf) << 4) | (v_slot & 0xf));
	if (ret)
		return ret;

	if (v_slot < 8) {
		ret = regmap_update_bits(ctx->regmap, MAX98390_PCM_TX_HIZ_CTRL_A,
					 BIT(v_slot), 0);
		if (ret)
			return ret;
		ret = regmap_update_bits(ctx->regmap, MAX98390_PCM_TX_EN_A,
					 BIT(v_slot), BIT(v_slot));
	} else {
		ret = regmap_update_bits(ctx->regmap, MAX98390_PCM_TX_HIZ_CTRL_B,
					 BIT(v_slot - 8), 0);
		if (ret)
			return ret;
		ret = regmap_update_bits(ctx->regmap, MAX98390_PCM_TX_EN_B,
					 BIT(v_slot - 8), BIT(v_slot - 8));
	}
	if (ret)
		return ret;

	if (i_slot < 8) {
		ret = regmap_update_bits(ctx->regmap, MAX98390_PCM_TX_HIZ_CTRL_A,
					 BIT(i_slot), 0);
		if (ret)
			return ret;
		ret = regmap_update_bits(ctx->regmap, MAX98390_PCM_TX_EN_A,
					 BIT(i_slot), BIT(i_slot));
	} else {
		ret = regmap_update_bits(ctx->regmap, MAX98390_PCM_TX_HIZ_CTRL_B,
					 BIT(i_slot - 8), 0);
		if (ret)
			return ret;
		ret = regmap_update_bits(ctx->regmap, MAX98390_PCM_TX_EN_B,
					 BIT(i_slot - 8), BIT(i_slot - 8));
	}

	return ret;
}

static int max98390_hda_init(struct max98390_hda *ctx)
{
	int ret;

	regmap_write(ctx->regmap, MAX98390_SOFTWARE_RESET, 0x01);
	msleep(20);

	ret = regmap_write(ctx->regmap, MAX98390_CLK_MON, 0x6f);
	if (ret)
		return ret;
	ret = regmap_write(ctx->regmap, MAX98390_DAT_MON, 0x00);
	if (ret)
		return ret;
	ret = regmap_write(ctx->regmap, MAX98390_PWR_GATE_CTL, 0x00);
	if (ret)
		return ret;

	/*
	 * Enable RX channels 0,1 (0x03) - matches Linux ALSA slot usage.
	 * Windows uses 0x78 (Ch 3-6) but Windows Realtek driver likely sends
	 * on different TDM slots. Linux HDA sends on Slots 0,1.
	 */
	ret = regmap_write(ctx->regmap, MAX98390_PCM_RX_EN_A, 0x03);
	if (ret)
		return ret;
	ret = regmap_write(ctx->regmap, MAX98390_ENV_TRACK_VOUT_HEADROOM, 0x0e);
	if (ret)
		return ret;
	ret = regmap_write(ctx->regmap, MAX98390_BOOST_BYPASS1, 0x46);
	if (ret)
		return ret;
	ret = regmap_write(ctx->regmap, MAX98390_FET_SCALING3, 0x03);
	if (ret)
		return ret;
	/*
	 * EXACT Windows register values from netstate2 analysis:
	 *
	 * 0x2024 PCM_MODE_CFG = 0xF8 (Format 7, 32-bit)
	 * 0x2027 PCM_SR_SETUP = 0x01 (sample rate config)
	 * 0x2021 PCM_CH_SRC_1 = NOT SET (uses chip default)
	 */
	ret = regmap_write(ctx->regmap, MAX98390_PCM_MODE_CFG, 0xF8);
	if (ret)
		return ret;

	ret = regmap_write(ctx->regmap, MAX98390_PCM_SR_SETUP, 0x01);
	if (ret)
		return ret;

	/* Windows doesn't set PCM_CH_SRC_1 - use chip default (0x00) */
	/* But testing showed swap needed, so try inverted mapping */
	ret = regmap_write(ctx->regmap, MAX98390_PCM_CH_SRC_1,
			   (ctx->index & 1) ^ 1);
	if (ret)
		return ret;

	ret = max98390_hda_program_slots(ctx, 0, 1);
	if (ret)
		return ret;

	/* Ensure amp is disabled until playback starts */
	regmap_update_bits(ctx->regmap, MAX98390_R203A_AMP_EN,
			 MAX98390_AMP_EN_MASK, 0);
	regmap_update_bits(ctx->regmap, MAX98390_R23FF_GLOBAL_EN,
			 MAX98390_GLOBAL_EN_MASK, 0);

	return 0;
}

static int max98390_hda_start(struct max98390_hda *ctx)
{
	int ret;

	ret = regmap_update_bits(ctx->regmap, MAX98390_R23FF_GLOBAL_EN,
				 MAX98390_GLOBAL_EN_MASK, MAX98390_GLOBAL_EN_MASK);
	if (ret)
		return ret;

	return regmap_update_bits(ctx->regmap, MAX98390_R203A_AMP_EN,
				       MAX98390_AMP_EN_MASK, MAX98390_AMP_EN_MASK);
}

static void max98390_hda_stop(struct max98390_hda *ctx)
{
	regmap_update_bits(ctx->regmap, MAX98390_R203A_AMP_EN,
			 MAX98390_AMP_EN_MASK, 0);
	regmap_update_bits(ctx->regmap, MAX98390_R23FF_GLOBAL_EN,
			 MAX98390_GLOBAL_EN_MASK, 0);
}

static void max98390_hda_playback_hook(struct device *dev, int action)
{
	struct max98390_hda *ctx = dev_get_drvdata(dev);

	switch (action) {
	case HDA_GEN_PCM_ACT_OPEN:
		pm_runtime_get_sync(dev);
		ctx->playing = true;
		break;
	case HDA_GEN_PCM_ACT_PREPARE:
		max98390_hda_start(ctx);
		break;
	case HDA_GEN_PCM_ACT_CLEANUP:
		max98390_hda_stop(ctx);
		break;
	case HDA_GEN_PCM_ACT_CLOSE:
		max98390_hda_stop(ctx);
		ctx->playing = false;
		pm_runtime_mark_last_busy(dev);
		pm_runtime_put_autosuspend(dev);
		break;
	default:
		break;
	}
}

static int max98390_hda_bind(struct device *dev, struct device *master,
			      void *master_data)
{
	struct max98390_hda *ctx = dev_get_drvdata(dev);
	struct hda_component_parent *parent = master_data;
	struct hda_component *comp;

	comp = hda_component_from_index(parent, ctx->index);
	if (!comp)
		return -EINVAL;
	if (comp->dev)
		return -EBUSY;

	comp->dev = dev;
	ctx->codec = parent->codec;
	strscpy(comp->name, dev_name(dev), sizeof(comp->name));
	comp->playback_hook = max98390_hda_playback_hook;

	return 0;
}

static void max98390_hda_unbind(struct device *dev, struct device *master,
				void *master_data)
{
	struct max98390_hda *ctx = dev_get_drvdata(dev);
	struct hda_component_parent *parent = master_data;
	struct hda_component *comp;

	comp = hda_component_from_index(parent, ctx->index);
	if (comp && comp->dev == dev)
		memset(comp, 0, sizeof(*comp));
	ctx->codec = NULL;
}

static const struct component_ops max98390_hda_comp_ops = {
	.bind = max98390_hda_bind,
	.unbind = max98390_hda_unbind,
};

static int max98390_hda_acpi_probe(struct max98390_hda *ctx)
{
	struct i2c_client *client = to_i2c_client(ctx->dev);
	u32 value;
	u64 uid;
	int ret;

	ctx->adev = ACPI_COMPANION(ctx->dev);

	/* Samsung Galaxy Book 4 Pro/360 uses non-contiguous I2C addresses: 0x38, 0x39, 0x3c, 0x3d */
	switch (client->addr) {
	case 0x38:
		ctx->index = 0;
		break;
	case 0x39:
		ctx->index = 1;
		break;
	case 0x3c:
		ctx->index = 2;
		break;
	case 0x3d:
		ctx->index = 3;
		break;
	default:
		dev_warn(ctx->dev, "Unknown I2C address 0x%02x, defaulting to index 0\n", client->addr);
		ctx->index = 0;
		break;
	}

	ctx->channel = ctx->index;

	if (!ctx->adev)
		return 0;

	if (!device_property_read_u32(ctx->dev, MAX98390_ACPI_PROP_DEV_INDEX,
					    &value) && value < MAX98390_HDA_MAX_AMPS)
		ctx->index = value;
	else if (!acpi_dev_uid_to_integer(ctx->adev, &uid) &&
		 uid < MAX98390_HDA_MAX_AMPS)
		ctx->index = uid;

	if (!device_property_read_u32(ctx->dev, MAX98390_ACPI_PROP_SPK_POS, &value) &&
	    value < MAX98390_HDA_MAX_AMPS)
		ctx->channel = value;

	ret = device_property_read_u32(ctx->dev, MAX98390_ACPI_PROP_SPK_ID, &value);
	if (!ret)
		dev_dbg(ctx->dev, "Speaker ID %u\n", value);

	return 0;
}

int max98390_hda_probe(struct device *dev, const char *device_name, int id, int irq)
{
	struct max98390_hda *ctx;
	struct i2c_client *client = to_i2c_client(dev);
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->dev = dev;
	dev_set_drvdata(dev, ctx);

	ctx->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio))
		return PTR_ERR(ctx->reset_gpio);

	ctx->regmap = devm_regmap_init_i2c(client, &max98390_hda_regmap_i2c);
	if (IS_ERR(ctx->regmap))
		return PTR_ERR(ctx->regmap);

	ret = max98390_hda_acpi_probe(ctx);
	if (ret)
		return ret;

	max98390_hda_hw_reset(ctx);
	ret = max98390_hda_init(ctx);
	if (ret)
		return ret;

#if IS_ENABLED(CONFIG_DMI)
	{
		const struct dmi_system_id *dmi_id;

		dmi_id = dmi_first_match(max98390_dsm_dmi_table);
		if (dmi_id)
			dev_info(dev, "Loading DSM parameters from %s\n",
				 (const char *)dmi_id->driver_data);
		ret = max98390_load_dsm_fw(dev, ctx->regmap,
				       dmi_id ? dmi_id->driver_data : NULL);
	}
#else
	ret = max98390_load_dsm_fw(dev, ctx->regmap, NULL);
#endif
	if (ret)
		dev_warn(dev, "DSM firmware load failed: %d\n", ret);

	pm_runtime_set_autosuspend_delay(dev, 3000);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	ret = component_add(dev, &max98390_hda_comp_ops);
	if (ret) {
		pm_runtime_disable(dev);
		return ret;
	}

	dev_info(dev, "MAX98390 HDA amp index %d channel %d initialised\n",
		 ctx->index, ctx->channel);

	return 0;
}
EXPORT_SYMBOL_NS_GPL(max98390_hda_probe, "SND_HDA_SCODEC_MAX98390");

void max98390_hda_remove(struct device *dev)
{
	struct max98390_hda *ctx = dev_get_drvdata(dev);

	component_del(dev, &max98390_hda_comp_ops);
	pm_runtime_disable(dev);
	max98390_hda_stop(ctx);
}
EXPORT_SYMBOL_NS_GPL(max98390_hda_remove, "SND_HDA_SCODEC_MAX98390");

static int max98390_hda_runtime_suspend(struct device *dev)
{
	struct max98390_hda *ctx = dev_get_drvdata(dev);

	ctx->suspended = true;
	if (ctx->playing)
		max98390_hda_stop(ctx);

	return 0;
}

static int max98390_hda_runtime_resume(struct device *dev)
{
	struct max98390_hda *ctx = dev_get_drvdata(dev);

	ctx->suspended = false;
	return 0;
}

const struct dev_pm_ops max98390_hda_pm_ops = {
	.runtime_suspend = max98390_hda_runtime_suspend,
	.runtime_resume = max98390_hda_runtime_resume,
};
EXPORT_SYMBOL_NS_GPL(max98390_hda_pm_ops, "SND_HDA_SCODEC_MAX98390");

MODULE_IMPORT_NS("SND_SOC_MAX98390");
MODULE_DESCRIPTION("HDA MAX98390 driver");
MODULE_AUTHOR("Lyapsus");
MODULE_LICENSE("GPL");
