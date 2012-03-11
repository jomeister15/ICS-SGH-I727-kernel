/* Copyright (c) 2011, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/elf.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/platform_device.h>

#include <mach/msm_iomap.h>
#include <mach/msm_xo.h>

#include "peripheral-loader.h"
#include "scm-pas.h"

#define PROXY_VOTE_TIMEOUT		10000

#define RIVA_PMU_A2XB_CFG		0xB8
#define RIVA_PMU_A2XB_CFG_EN		BIT(0)

#define RIVA_PMU_CFG			0x28
#define RIVA_PMU_CFG_WARM_BOOT		BIT(0)
#define RIVA_PMU_CFG_IRIS_XO_MODE	0x6
#define RIVA_PMU_CFG_IRIS_XO_MODE_48	(3 << 1)

#define RIVA_PMU_OVRD_VAL		0x30
#define RIVA_PMU_OVRD_VAL_CCPU_RESET	BIT(0)
#define RIVA_PMU_OVRD_VAL_CCPU_CLK	BIT(1)

#define RIVA_PMU_CCPU_CTL		0x9C
#define RIVA_PMU_CCPU_CTL_HIGH_IVT	BIT(0)
#define RIVA_PMU_CCPU_CTL_REMAP_EN	BIT(2)

#define RIVA_PMU_CCPU_BOOT_REMAP_ADDR	0xA0

#define RIVA_PLL_MODE			(MSM_CLK_CTL_BASE + 0x31A0)
#define PLL_MODE_OUTCTRL		BIT(0)
#define PLL_MODE_BYPASSNL		BIT(1)
#define PLL_MODE_RESET_N		BIT(2)
#define PLL_MODE_REF_XO_SEL		0x30
#define PLL_MODE_REF_XO_SEL_CXO		(2 << 4)
#define PLL_MODE_REF_XO_SEL_RF		(3 << 4)
#define RIVA_PLL_L_VAL			(MSM_CLK_CTL_BASE + 0x31A4)
#define RIVA_PLL_M_VAL			(MSM_CLK_CTL_BASE + 0x31A8)
#define RIVA_PLL_N_VAL			(MSM_CLK_CTL_BASE + 0x31Ac)
#define RIVA_PLL_CONFIG			(MSM_CLK_CTL_BASE + 0x31B4)
#define RIVA_PLL_STATUS			(MSM_CLK_CTL_BASE + 0x31B8)

#define RIVA_PMU_ROOT_CLK_SEL		0xC8
#define RIVA_PMU_ROOT_CLK_SEL_3		BIT(2)

#define RIVA_PMU_CLK_ROOT3			0x78
#define RIVA_PMU_CLK_ROOT3_ENA			BIT(0)
#define RIVA_PMU_CLK_ROOT3_SRC0_DIV		0x3C
#define RIVA_PMU_CLK_ROOT3_SRC0_DIV_2		(1 << 2)
#define RIVA_PMU_CLK_ROOT3_SRC0_SEL		0x1C0
#define RIVA_PMU_CLK_ROOT3_SRC0_SEL_RIVA	(1 << 6)
#define RIVA_PMU_CLK_ROOT3_SRC1_DIV		0x1E00
#define RIVA_PMU_CLK_ROOT3_SRC1_DIV_2		(1 << 9)
#define RIVA_PMU_CLK_ROOT3_SRC1_SEL		0xE000
#define RIVA_PMU_CLK_ROOT3_SRC1_SEL_RIVA	(1 << 13)

struct riva_data {
	void __iomem *base;
	unsigned long start_addr;
	struct msm_xo_voter *xo;
	struct timer_list xo_timer;
};

static void pil_riva_make_xo_proxy_votes(struct device *dev)
{
	struct riva_data *drv = dev_get_drvdata(dev);

	msm_xo_mode_vote(drv->xo, MSM_XO_MODE_ON);
	mod_timer(&drv->xo_timer, jiffies+msecs_to_jiffies(PROXY_VOTE_TIMEOUT));
}

static void pil_riva_remove_xo_proxy_votes(unsigned long data)
{
	struct riva_data *drv = (struct riva_data *)data;

	msm_xo_mode_vote(drv->xo, MSM_XO_MODE_OFF);
}

static void pil_riva_remove_xo_proxy_votes_now(struct device *dev)
{
	struct riva_data *drv = dev_get_drvdata(dev);

	if (del_timer(&drv->xo_timer))
		pil_riva_remove_xo_proxy_votes((unsigned long)drv);
}

static bool cxo_is_needed(struct riva_data *drv)
{
	u32 reg = readl_relaxed(drv->base + RIVA_PMU_CFG);
	return (reg & RIVA_PMU_CFG_IRIS_XO_MODE)
		!= RIVA_PMU_CFG_IRIS_XO_MODE_48;
}

static int nop_verify_blob(struct pil_desc *pil, u32 phy_addr, size_t size)
{
	return 0;
}

static int pil_riva_init_image(struct pil_desc *pil, const u8 *metadata,
		size_t size)
{
	const struct elf32_hdr *ehdr = (struct elf32_hdr *)metadata;
	struct riva_data *drv = dev_get_drvdata(pil->dev);
	drv->start_addr = ehdr->e_entry;
	return 0;
}

static int pil_riva_reset(struct pil_desc *pil)
{
	u32 reg, sel;
	bool use_cxo;
	struct riva_data *drv = dev_get_drvdata(pil->dev);
	void __iomem *base = drv->base;
	unsigned long start_addr = drv->start_addr;

	/* Enable A2XB bridge */
	reg = readl_relaxed(base + RIVA_PMU_A2XB_CFG);
	reg |= RIVA_PMU_A2XB_CFG_EN;
	writel_relaxed(reg, base + RIVA_PMU_A2XB_CFG);

	/* Proxy-vote for CXO if it's needed */
	use_cxo = cxo_is_needed(drv);
	if (use_cxo)
		pil_riva_make_xo_proxy_votes(pil->dev);

	/* Program PLL 13 to 960 MHz */
	reg = readl_relaxed(RIVA_PLL_MODE);
	reg &= ~(PLL_MODE_BYPASSNL | PLL_MODE_OUTCTRL | PLL_MODE_RESET_N);
	writel_relaxed(reg, RIVA_PLL_MODE);

	if (use_cxo)
		writel_relaxed(0x40000C00 | 50, RIVA_PLL_L_VAL);
	else
		writel_relaxed(0x40000C00 | 40, RIVA_PLL_L_VAL);
	writel_relaxed(0, RIVA_PLL_M_VAL);
	writel_relaxed(1, RIVA_PLL_N_VAL);
	writel_relaxed(0x01495227, RIVA_PLL_CONFIG);

	reg = readl_relaxed(RIVA_PLL_MODE);
	reg &= ~(PLL_MODE_REF_XO_SEL);
	reg |= use_cxo ? PLL_MODE_REF_XO_SEL_CXO : PLL_MODE_REF_XO_SEL_RF;
	writel_relaxed(reg, RIVA_PLL_MODE);

	/* Enable PLL 13 */
	reg |= PLL_MODE_BYPASSNL;
	writel_relaxed(reg, RIVA_PLL_MODE);

	/*
	 * H/W requires a 5us delay between disabling the bypass and
	 * de-asserting the reset. Delay 10us just to be safe.
	 */
	mb();
	usleep_range(10, 20);

	reg |= PLL_MODE_RESET_N;
	writel_relaxed(reg, RIVA_PLL_MODE);
	reg |= PLL_MODE_OUTCTRL;
	writel_relaxed(reg, RIVA_PLL_MODE);

	/* Wait for PLL to settle */
	mb();
	usleep_range(50, 100);

	/* Configure cCPU for 240 MHz */
	sel = readl_relaxed(base + RIVA_PMU_ROOT_CLK_SEL);
	reg = readl_relaxed(base + RIVA_PMU_CLK_ROOT3);
	if (sel & RIVA_PMU_ROOT_CLK_SEL_3) {
		reg &= ~(RIVA_PMU_CLK_ROOT3_SRC0_SEL |
			 RIVA_PMU_CLK_ROOT3_SRC0_DIV);
		reg |= RIVA_PMU_CLK_ROOT3_SRC0_SEL_RIVA |
		       RIVA_PMU_CLK_ROOT3_SRC0_DIV_2;
	} else {
		reg &= ~(RIVA_PMU_CLK_ROOT3_SRC1_SEL |
			 RIVA_PMU_CLK_ROOT3_SRC1_DIV);
		reg |= RIVA_PMU_CLK_ROOT3_SRC1_SEL_RIVA |
		       RIVA_PMU_CLK_ROOT3_SRC1_DIV_2;
	}
	writel_relaxed(reg, base + RIVA_PMU_CLK_ROOT3);
	reg |= RIVA_PMU_CLK_ROOT3_ENA;
	writel_relaxed(reg, base + RIVA_PMU_CLK_ROOT3);
	reg = readl_relaxed(base + RIVA_PMU_ROOT_CLK_SEL);
	reg ^= RIVA_PMU_ROOT_CLK_SEL_3;
	writel_relaxed(reg, base + RIVA_PMU_ROOT_CLK_SEL);

	/* Use the high vector table */
	reg = readl_relaxed(base + RIVA_PMU_CCPU_CTL);
	reg |= RIVA_PMU_CCPU_CTL_HIGH_IVT | RIVA_PMU_CCPU_CTL_REMAP_EN;
	writel_relaxed(reg, base + RIVA_PMU_CCPU_CTL);

	/* Set base memory address */
	writel_relaxed(start_addr >> 16, base + RIVA_PMU_CCPU_BOOT_REMAP_ADDR);

	/* Clear warmboot bit indicating this is a cold boot */
	reg = readl_relaxed(base + RIVA_PMU_CFG);
	reg &= ~(RIVA_PMU_CFG_WARM_BOOT);
	writel_relaxed(reg, base + RIVA_PMU_CFG);

	/* Enable the cCPU clock */
	reg = readl_relaxed(base + RIVA_PMU_OVRD_VAL);
	reg |= RIVA_PMU_OVRD_VAL_CCPU_CLK;
	writel_relaxed(reg, base + RIVA_PMU_OVRD_VAL);

	/* Take cCPU out of reset */
	reg |= RIVA_PMU_OVRD_VAL_CCPU_RESET;
	writel_relaxed(reg, base + RIVA_PMU_OVRD_VAL);

	return 0;
}

static int pil_riva_shutdown(struct pil_desc *pil)
{
	struct riva_data *drv = dev_get_drvdata(pil->dev);
	u32 reg;

	reg = readl_relaxed(drv->base + RIVA_PMU_OVRD_VAL);
	reg &= ~(RIVA_PMU_OVRD_VAL_CCPU_RESET | RIVA_PMU_OVRD_VAL_CCPU_CLK);
	writel_relaxed(reg, drv->base + RIVA_PMU_OVRD_VAL);

	pil_riva_remove_xo_proxy_votes_now(pil->dev);

	return 0;
}

static struct pil_reset_ops pil_riva_ops = {
	.init_image = pil_riva_init_image,
	.verify_blob = nop_verify_blob,
	.auth_and_reset = pil_riva_reset,
	.shutdown = pil_riva_shutdown,
};

static int pil_riva_init_image_trusted(struct pil_desc *pil,
		const u8 *metadata, size_t size)
{
	return pas_init_image(PAS_RIVA, metadata, size);
}

static int pil_riva_reset_trusted(struct pil_desc *pil)
{
	struct riva_data *drv = dev_get_drvdata(pil->dev);

	/* Proxy-vote for CXO if it's needed */
	if (cxo_is_needed(drv))
		pil_riva_make_xo_proxy_votes(pil->dev);

	return pas_auth_and_reset(PAS_RIVA);
}

static int pil_riva_shutdown_trusted(struct pil_desc *pil)
{
	int ret = pas_shutdown(PAS_RIVA);

	pil_riva_remove_xo_proxy_votes_now(pil->dev);

	return ret;
}

static struct pil_reset_ops pil_riva_ops_trusted = {
	.init_image = pil_riva_init_image_trusted,
	.verify_blob = nop_verify_blob,
	.auth_and_reset = pil_riva_reset_trusted,
	.shutdown = pil_riva_shutdown_trusted,
};

static int __devinit pil_riva_probe(struct platform_device *pdev)
{
	struct riva_data *drv;
	struct resource *res;
	struct pil_desc *desc;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;

	drv = devm_kzalloc(&pdev->dev, sizeof(*drv), GFP_KERNEL);
	if (!drv)
		return -ENOMEM;
	platform_set_drvdata(pdev, drv);

	drv->base = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	if (!drv->base)
		return -ENOMEM;

	desc = devm_kzalloc(&pdev->dev, sizeof(*desc), GFP_KERNEL);
	if (!desc)
		return -ENOMEM;

	desc->name = "wcnss";
	desc->dev = &pdev->dev;

	if (pas_supported(PAS_RIVA) > 0) {
		desc->ops = &pil_riva_ops_trusted;
		dev_info(&pdev->dev, "using secure boot\n");
	} else {
		desc->ops = &pil_riva_ops;
		dev_info(&pdev->dev, "using non-secure boot\n");
	}

	setup_timer(&drv->xo_timer, pil_riva_remove_xo_proxy_votes,
		    (unsigned long)drv);
	drv->xo = msm_xo_get(MSM_XO_CXO, desc->name);
	if (IS_ERR(drv->xo))
		return PTR_ERR(drv->xo);

	return msm_pil_register(desc);
}

static int __devexit pil_riva_remove(struct platform_device *pdev)
{
	return 0;
}

static struct platform_driver pil_riva_driver = {
	.probe = pil_riva_probe,
	.remove = __devexit_p(pil_riva_remove),
	.driver = {
		.name = "pil_riva",
		.owner = THIS_MODULE,
	},
};

static int __init pil_riva_init(void)
{
	return platform_driver_register(&pil_riva_driver);
}
module_init(pil_riva_init);

static void __exit pil_riva_exit(void)
{
	platform_driver_unregister(&pil_riva_driver);
}
module_exit(pil_riva_exit);

MODULE_DESCRIPTION("Support for booting RIVA (WCNSS) processors");
MODULE_LICENSE("GPL v2");
