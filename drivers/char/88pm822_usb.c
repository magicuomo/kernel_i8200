/*
 * 88pm822 VBus driver for Marvell USB
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/mfd/88pm822.h>
#include <linux/delay.h>
#include <linux/platform_data/mv_usb.h>

#if defined(CONFIG_SM5502_MUIC)
#include <mach/sm5502-muic.h>
extern int extert_muic_read_reg(u8 reg, u8 * data);
#endif
#if defined(CONFIG_MFD_RT8973)
extern int rt8973_check_usb_status();
#endif

struct pm822_usb_info {
	struct pm822_chip	*chip;
	struct pm822_subchip	*subchip;
	int			irq;
	int			vbus_gpio;
	int			id_gpadc;
};

/* bit definitions of PM822_GPADC_MEAS_EN2 : 0x02 */
#define PM822_MEAS_GP0_EN			(1 << 2)
#define PM822_MEAS_GP1_EN			(1 << 3)
#define PM822_MEAS_GP2_EN			(1 << 4)
#define PM822_MEAS_GP3_EN			(1 << 5)

/* bit definitions of PM800_GPADC_MISC_GPFSM_EN : 0x06*/
#define PM822_GPADC_MISC_GPFSM_EN	(1 << 0)

static struct pm822_usb_info *vbus_info;

static int pm822_read_usb_val(unsigned int *level)
{
	int ret;
	unsigned int val;
#if defined(CONFIG_SM5502_MUIC)
	u8 data;
#endif

	ret = regmap_read(vbus_info->chip->regmap, PM822_STATUS1, &val);
	if (ret)
		return ret;
#if defined(CONFIG_SM5502_MUIC)
	extert_muic_read_reg(REG_DEV_T2, &data);
	pr_info("%s: val=0x%x, data=0x%x\n", __func__, val, data);

	if (data & DEV_JIG_ALL)
		*level = VBUS_LOW;
	else {
		if (val & PM822_CHG_STS1)
			*level = VBUS_HIGH;
		else
			*level = VBUS_LOW;
	}
#elif defined(CONFIG_MFD_RT8973)
	if(rt8973_check_usb_status())
		*level = VBUS_HIGH;
	else
		*level = VBUS_LOW;
#else
	if (val & PM822_CHG_STS1)
		*level = VBUS_HIGH;
	else
		*level = VBUS_LOW;
#endif
	pr_info("pm822_read_usb_val VBUS = %d\n", *level);
	return 0;
}

static int pm822_read_id_val(unsigned int *level)
{
	int ret, data;
	unsigned int val;
	unsigned int meas1, meas2, upp_th, low_th;

	switch (vbus_info->id_gpadc) {
	case PM822_GPADC0:
		meas1 = PM822_GPADC0_MEAS1;
		meas2 = PM822_GPADC0_MEAS2;
		low_th = PM822_GPADC0_LOW_TH;
		upp_th = PM822_GPADC0_UPP_TH;
		break;
	case PM822_GPADC1:
		meas1 = PM822_GPADC1_MEAS1;
		meas2 = PM822_GPADC1_MEAS2;
		low_th = PM822_GPADC1_LOW_TH;
		upp_th = PM822_GPADC1_UPP_TH;
		break;
	case PM822_GPADC2:
		meas1 = PM822_GPADC2_MEAS1;
		meas2 = PM822_GPADC2_MEAS2;
		low_th = PM822_GPADC2_LOW_TH;
		upp_th = PM822_GPADC2_UPP_TH;
		break;
	case PM822_GPADC3:
		meas1 = PM822_GPADC3_MEAS1;
		meas2 = PM822_GPADC3_MEAS2;
		low_th = PM822_GPADC3_LOW_TH;
		upp_th = PM822_GPADC3_UPP_TH;
		break;
	default:
		return -ENODEV;
	}

	ret = regmap_read(vbus_info->subchip->regmap_gpadc, meas1, &val);
	data = val << 4;
	if (ret)
		return ret;

	ret = regmap_read(vbus_info->subchip->regmap_gpadc, meas2, &val);
	data |= val & 0x0F;
	if (ret)
		return ret;
	if (data > 0x10) {
		regmap_write(vbus_info->subchip->regmap_gpadc, low_th, 0x10);
		if (ret)
			return ret;

		regmap_write(vbus_info->subchip->regmap_gpadc, upp_th, 0xff);
		if (ret)
			return ret;

		*level = 1;
	} else {
		regmap_write(vbus_info->subchip->regmap_gpadc, low_th, 0);
		if (ret)
			return ret;

		regmap_write(vbus_info->subchip->regmap_gpadc, upp_th, 0x10);
		if (ret)
			return ret;

		*level = 0;
	}

	return 0;
};

int pm822_init_id(void)
{
	int ret;
	unsigned int en;

	switch (vbus_info->id_gpadc) {
	case PM822_GPADC0:
		en = PM822_MEAS_GP0_EN;
		break;
	case PM822_GPADC1:
		en = PM822_MEAS_GP1_EN;
		break;
	case PM822_GPADC2:
		en = PM822_MEAS_GP2_EN;
		break;
	case PM822_GPADC3:
		en = PM822_MEAS_GP3_EN;
		break;
	default:
		return -ENODEV;
	}


	ret = regmap_update_bits(vbus_info->subchip->regmap_gpadc,
					PM822_GPADC_MEAS_EN2, en, en);
	if (ret)
		return ret;

	ret = regmap_update_bits(vbus_info->subchip->regmap_gpadc,
		PM822_GPADC_MISC_CONFIG2, PM822_GPADC_MISC_GPFSM_EN,
		PM822_GPADC_MISC_GPFSM_EN);
	if (ret)
		return ret;

	return 0;
}

static int pm822_set_vbus(unsigned int vbus)
{
	int ret;
	unsigned int data = 0, mask, reg = 0;

	switch (vbus_info->vbus_gpio) {
	case PM822_NO_GPIO:
		/* OTG5V not supported - Do nothing */
		return 0;

	case PM822_GPIO0:
		/* OTG5V Enable/Disable is connected to GPIO_0 */
		mask = PM822_GPIO0_GPIO_MODE(0x01) | PM822_GPIO0_VAL;
		reg = PM822_GPIO0_CTRL;
		break;

	case PM822_GPIO1:
		/* OTG5V Enable/Disable is connected to GPIO_1 */
		mask = PM822_GPIO1_GPIO_MODE(0x01) | PM822_GPIO1_VAL;
		reg = PM822_GPIO1_CTRL;
		break;

	case PM822_GPIO2:
		/* OTG5V Enable/Disable is connected to GPIO_2 */
		mask = PM822_GPIO2_GPIO_MODE(0x01) | PM822_GPIO2_VAL;
		reg = PM822_GPIO2_CTRL;
		break;

	case PM822_GPIO3:
		/* OTG5V Enable/Disable is connected to GPIO_3 */
		mask = PM822_GPIO3_GPIO_MODE(0x01) | PM822_GPIO3_VAL;
		reg = PM822_GPIO3_CTRL;
		break;

	default:
		return -ENODEV;
	}

	if (vbus == VBUS_HIGH)
		data = mask;

	ret = regmap_update_bits(vbus_info->chip->regmap, reg, mask, data);
	if (ret)
		return ret;

	mdelay(20);

	ret = pm822_read_usb_val(&data);
	if (ret)
		return ret;

	if (ret != vbus)
		pr_info("vbus set failed %x\n", vbus);
	else
		pr_info("vbus set done %x\n", vbus);

	return 0;
}

static irqreturn_t vbus_irq(int irq, void *dev)
{
	pxa_usb_notify(PXA_USB_DEV_OTG, EVENT_VBUS, 0);

	return IRQ_HANDLED;
}


static int __devinit pm822_usb_probe(struct platform_device *pdev)
{
	struct pm822_chip *chip = dev_get_drvdata(pdev->dev.parent);
	struct pm822_platform_data *pm822_pdata;
	struct pm822_usb_info *usb;
	int ret;

	if (pdev->dev.parent->platform_data) {
		pm822_pdata = pdev->dev.parent->platform_data;
	} else {
		pr_debug("Invalid pm822 platform data!\n");
		return -EINVAL;
	}

	usb = kzalloc(sizeof(struct pm822_usb_info), GFP_KERNEL);
	if (!usb)
		return -ENOMEM;

	usb->chip = chip;
	usb->subchip = chip->subchip;

	if (pm822_pdata->usb) {
		usb->vbus_gpio = pm822_pdata->usb->vbus_gpio;
		usb->id_gpadc = pm822_pdata->usb->id_gpadc;
	}

	usb->irq = platform_get_irq(pdev, 0);
	if (usb->irq < 0) {
		dev_err(&pdev->dev, "failed to get vbus irq\n");
		ret = -ENXIO;
		goto out;
	}

	usb->irq += chip->irq_base;

	ret = request_threaded_irq(usb->irq, NULL,
			vbus_irq, IRQF_ONESHOT, "88pm800-usb-vbus", usb);
	if (ret) {
		dev_info(&pdev->dev,
			"Can not request irq for VBUS, "
			"disable clock gating\n");
	}

	vbus_info = usb;
	platform_set_drvdata(pdev, usb);
	device_init_wakeup(&pdev->dev, 1);

	pxa_usb_set_extern_call(PXA_USB_DEV_OTG, vbus, set_vbus,
				pm822_set_vbus);
	pxa_usb_set_extern_call(PXA_USB_DEV_OTG, vbus, get_vbus,
				pm822_read_usb_val);

	pxa_usb_set_extern_call(PXA_USB_DEV_OTG, idpin, get_idpin,
				pm822_read_id_val);
	pxa_usb_set_extern_call(PXA_USB_DEV_OTG, idpin, init,
				pm822_init_id);
	return 0;

out:
	kfree(usb);
	return ret;
}

static int __devexit pm822_usb_remove(struct platform_device *pdev)
{
	struct pm822_usb_info *usb = platform_get_drvdata(pdev);

	if (usb) {
		platform_set_drvdata(pdev, NULL);
		kfree(usb);
	}

	return 0;
}

#ifdef CONFIG_PM
static int pm822_usb_suspend(struct device *dev)
{
	return pm822_dev_suspend(dev);
}

static int pm822_usb_resume(struct device *dev)
{
	return pm822_dev_resume(dev);
}

static const struct dev_pm_ops pm822_usb_pm_ops = {
	.suspend	= pm822_usb_suspend,
	.resume		= pm822_usb_resume,
};
#endif

static struct platform_driver pm822_usb_driver = {
	.driver		= {
		.name	= "88pm822-usb",
		.owner	= THIS_MODULE,
#ifdef CONFIG_PM
		.pm	= &pm822_usb_pm_ops,
#endif
	},
	.probe		= pm822_usb_probe,
	.remove		= __devexit_p(pm822_usb_remove),
};

static int __init pm822_usb_init(void)
{
	return platform_driver_register(&pm822_usb_driver);
}
module_init(pm822_usb_init);

static void __exit pm822_usb_exit(void)
{
	platform_driver_unregister(&pm822_usb_driver);
}
module_exit(pm822_usb_exit);

MODULE_DESCRIPTION("VBUS driver for Marvell Semiconductor 88PM822");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:88pm822-usb");
