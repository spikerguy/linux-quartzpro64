// SPDX-License-Identifier: GPL-2.0-only
/*
 * Rockchip OTP Driver
 *
 * Copyright (c) 2018 Rockchip Electronics Co. Ltd.
 * Author: Finley Xiao <finley.xiao@rock-chips.com>
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/nvmem-provider.h>
#include <linux/reset.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

/* OTP Register Offsets */
#define OTPC_SBPI_CTRL			0x0020
#define OTPC_SBPI_CMD_VALID_PRE		0x0024
#define OTPC_SBPI_CS_VALID_PRE		0x0028
#define OTPC_SBPI_STATUS		0x002C
#define OTPC_USER_CTRL			0x0100
#define OTPC_USER_ADDR			0x0104
#define OTPC_USER_ENABLE		0x0108
#define OTPC_USER_QP			0x0120
#define OTPC_USER_Q			0x0124
#define OTPC_INT_STATUS			0x0304
#define OTPC_SBPI_CMD0_OFFSET		0x1000
#define OTPC_SBPI_CMD1_OFFSET		0x1004

/* OTP Register bits and masks */
#define OTPC_USER_ADDR_MASK		GENMASK(31, 16)
#define OTPC_USE_USER			BIT(0)
#define OTPC_USE_USER_MASK		GENMASK(16, 16)
#define OTPC_USER_FSM_ENABLE		BIT(0)
#define OTPC_USER_FSM_ENABLE_MASK	GENMASK(16, 16)
#define OTPC_SBPI_DONE			BIT(1)
#define OTPC_USER_DONE			BIT(2)

#define SBPI_DAP_ADDR			0x02
#define SBPI_DAP_ADDR_SHIFT		8
#define SBPI_DAP_ADDR_MASK		GENMASK(31, 24)
#define SBPI_CMD_VALID_MASK		GENMASK(31, 16)
#define SBPI_DAP_CMD_WRF		0xC0
#define SBPI_DAP_REG_ECC		0x3A
#define SBPI_ECC_ENABLE			0x00
#define SBPI_ECC_DISABLE		0x09
#define SBPI_ENABLE			BIT(0)
#define SBPI_ENABLE_MASK		GENMASK(16, 16)

#define OTPC_TIMEOUT			10000

#define RK3568_NBYTES			2

#define RK3588_OTPC_AUTO_CTRL		0x004
#define RK3588_OTPC_AUTO_EN		0x008
#define RK3588_OTPC_INT_ST		0x084
#define RK3588_OTPC_DOUT0		0x020
#define RK3588_NO_SECURE_OFFSET		0x300
#define RK3588_NBYTES			4
#define RK3588_BURST_NUM		1
#define RK3588_BURST_SHIFT		8
#define RK3588_ADDR_SHIFT		16
#define RK3588_AUTO_EN			BIT(0)
#define RK3588_RD_DONE			BIT(1)

struct rockchip_otp {
	struct device *dev;
	void __iomem *base;
	struct clk_bulk_data	*clks;
	int num_clks;
	struct reset_control *rst;
	struct nvmem_config *config;
	const struct rockchip_data *data;
	struct mutex lock;
};

struct rockchip_data {
	int size;
	const char * const *clocks;
	int num_clks;
	nvmem_reg_read_t reg_read;
	int (*init)(struct rockchip_otp *otp);
};

static int rockchip_otp_reset(struct rockchip_otp *otp)
{
	int ret;

	ret = reset_control_assert(otp->rst);
	if (ret) {
		dev_err(otp->dev, "failed to assert otp phy %d\n", ret);
		return ret;
	}

	udelay(2);

	ret = reset_control_deassert(otp->rst);
	if (ret) {
		dev_err(otp->dev, "failed to deassert otp phy %d\n", ret);
		return ret;
	}

	return 0;
}

static int px30_otp_wait_status(struct rockchip_otp *otp, u32 flag)
{
	u32 status = 0;
	int ret;

	ret = readl_poll_timeout_atomic(otp->base + OTPC_INT_STATUS, status,
					(status & flag), 1, OTPC_TIMEOUT);
	if (ret)
		return ret;

	/* clean int status */
	writel(flag, otp->base + OTPC_INT_STATUS);

	return 0;
}

static int px30_otp_ecc_enable(struct rockchip_otp *otp, bool enable)
{
	int ret = 0;

	writel(SBPI_DAP_ADDR_MASK | (SBPI_DAP_ADDR << SBPI_DAP_ADDR_SHIFT),
	       otp->base + OTPC_SBPI_CTRL);

	writel(SBPI_CMD_VALID_MASK | 0x1, otp->base + OTPC_SBPI_CMD_VALID_PRE);
	writel(SBPI_DAP_CMD_WRF | SBPI_DAP_REG_ECC,
	       otp->base + OTPC_SBPI_CMD0_OFFSET);
	if (enable)
		writel(SBPI_ECC_ENABLE, otp->base + OTPC_SBPI_CMD1_OFFSET);
	else
		writel(SBPI_ECC_DISABLE, otp->base + OTPC_SBPI_CMD1_OFFSET);

	writel(SBPI_ENABLE_MASK | SBPI_ENABLE, otp->base + OTPC_SBPI_CTRL);

	ret = px30_otp_wait_status(otp, OTPC_SBPI_DONE);
	if (ret < 0)
		dev_err(otp->dev, "timeout during ecc_enable\n");

	return ret;
}

static int px30_otp_read(void *context, unsigned int offset,
			 void *val, size_t bytes)
{
	struct rockchip_otp *otp = context;
	u8 *buf = val;
	int ret = 0;

	ret = clk_bulk_prepare_enable(otp->num_clks, otp->clks);
	if (ret < 0) {
		dev_err(otp->dev, "failed to prepare/enable clks\n");
		return ret;
	}

	ret = rockchip_otp_reset(otp);
	if (ret) {
		dev_err(otp->dev, "failed to reset otp phy\n");
		goto disable_clks;
	}

	ret = px30_otp_ecc_enable(otp, false);
	if (ret < 0) {
		dev_err(otp->dev, "rockchip_otp_ecc_enable err\n");
		goto disable_clks;
	}

	writel(OTPC_USE_USER | OTPC_USE_USER_MASK, otp->base + OTPC_USER_CTRL);
	udelay(5);
	while (bytes--) {
		writel(offset++ | OTPC_USER_ADDR_MASK,
		       otp->base + OTPC_USER_ADDR);
		writel(OTPC_USER_FSM_ENABLE | OTPC_USER_FSM_ENABLE_MASK,
		       otp->base + OTPC_USER_ENABLE);
		ret = px30_otp_wait_status(otp, OTPC_USER_DONE);
		if (ret < 0) {
			dev_err(otp->dev, "timeout during read setup\n");
			goto read_end;
		}
		*buf++ = readb(otp->base + OTPC_USER_Q);
	}

read_end:
	writel(0x0 | OTPC_USE_USER_MASK, otp->base + OTPC_USER_CTRL);
disable_clks:
	clk_bulk_disable_unprepare(otp->num_clks, otp->clks);

	return ret;
}

static int rk3568_otp_read(void *context, unsigned int offset,
			   void *val, size_t bytes)
{
	struct rockchip_otp *otp = context;
	unsigned int addr_start, addr_end, addr_offset, addr_len;
	unsigned int otp_qp;
	u32 out_value;
	u8 *buf;
	int ret = 0, i = 0;

	addr_start = rounddown(offset, RK3568_NBYTES) / RK3568_NBYTES;
	addr_end = roundup(offset + bytes, RK3568_NBYTES) / RK3568_NBYTES;
	addr_offset = offset % RK3568_NBYTES;
	addr_len = addr_end - addr_start;

	buf = kzalloc(array3_size(addr_len, RK3568_NBYTES, sizeof(*buf)),
		      GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = clk_bulk_prepare_enable(otp->num_clks, otp->clks);
	if (ret < 0) {
		dev_err(otp->dev, "failed to prepare/enable clks\n");
		goto out;
	}

	ret = rockchip_otp_reset(otp);
	if (ret) {
		dev_err(otp->dev, "failed to reset otp phy\n");
		goto disable_clks;
	}

	ret = px30_otp_ecc_enable(otp, true);
	if (ret < 0) {
		dev_err(otp->dev, "rockchip_otp_ecc_enable err\n");
		goto disable_clks;
	}

	writel(OTPC_USE_USER | OTPC_USE_USER_MASK, otp->base + OTPC_USER_CTRL);
	udelay(5);
	while (addr_len--) {
		writel(addr_start++ | OTPC_USER_ADDR_MASK,
		       otp->base + OTPC_USER_ADDR);
		writel(OTPC_USER_FSM_ENABLE | OTPC_USER_FSM_ENABLE_MASK,
		       otp->base + OTPC_USER_ENABLE);
		ret = px30_otp_wait_status(otp, OTPC_USER_DONE);
		if (ret < 0) {
			dev_err(otp->dev, "timeout during read setup\n");
			goto read_end;
		}
		otp_qp = readl(otp->base + OTPC_USER_QP);
		if (((otp_qp & 0xc0) == 0xc0) || (otp_qp & 0x20)) {
			ret = -EIO;
			dev_err(otp->dev, "ecc check error during read setup\n");
			goto read_end;
		}
		out_value = readl(otp->base + OTPC_USER_Q);
		memcpy(&buf[i], &out_value, RK3568_NBYTES);
		i += RK3568_NBYTES;
	}

	memcpy(val, buf + addr_offset, bytes);

read_end:
	writel(0x0 | OTPC_USE_USER_MASK, otp->base + OTPC_USER_CTRL);
disable_clks:
	clk_bulk_disable_unprepare(otp->num_clks, otp->clks);
out:
	kfree(buf);

	return ret;
}

static int rk3588_otp_wait_status(struct rockchip_otp *otp, u32 flag)
{
	u32 status = 0;
	int ret;

	ret = readl_poll_timeout_atomic(otp->base + RK3588_OTPC_INT_ST, status,
					(status & flag), 1, OTPC_TIMEOUT);
	if (ret)
		return ret;

	/* clean int status */
	writel(flag, otp->base + RK3588_OTPC_INT_ST);

	return 0;
}

static int rk3588_otp_read(void *context, unsigned int offset,
			   void *val, size_t bytes)
{
	struct rockchip_otp *otp = context;
	unsigned int addr_start, addr_end, addr_offset, addr_len;
	int ret = 0, i = 0;
	u32 out_value;
	u8 *buf;

	if (offset >= otp->data->size)
		return -ENOMEM;
	if (offset + bytes > otp->data->size)
		bytes = otp->data->size - offset;

	addr_start = rounddown(offset, RK3588_NBYTES) / RK3588_NBYTES;
	addr_end = roundup(offset + bytes, RK3588_NBYTES) / RK3588_NBYTES;
	addr_offset = offset % RK3588_NBYTES;
	addr_len = addr_end - addr_start;
	addr_start += RK3588_NO_SECURE_OFFSET;

	buf = kzalloc(array3_size(addr_len, RK3588_NBYTES, sizeof(*buf)),
		      GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = clk_bulk_prepare_enable(otp->num_clks, otp->clks);
	if (ret < 0) {
		dev_err(otp->dev, "failed to prepare/enable clks\n");
		goto out;
	}

	while (addr_len--) {
		writel((addr_start << RK3588_ADDR_SHIFT) |
		       (RK3588_BURST_NUM << RK3588_BURST_SHIFT),
		       otp->base + RK3588_OTPC_AUTO_CTRL);
		writel(RK3588_AUTO_EN, otp->base + RK3588_OTPC_AUTO_EN);
		ret = rk3588_otp_wait_status(otp, RK3588_RD_DONE);
		if (ret < 0) {
			dev_err(otp->dev, "timeout during read setup\n");
			goto read_end;
		}

		out_value = readl(otp->base + RK3588_OTPC_DOUT0);
		memcpy(&buf[i], &out_value, RK3588_NBYTES);
		i += RK3588_NBYTES;
		addr_start++;
	}

	memcpy(val, buf + addr_offset, bytes);

read_end:
	clk_bulk_disable_unprepare(otp->num_clks, otp->clks);
out:
	kfree(buf);

	return ret;
}

static int rockchip_otp_read(void *context, unsigned int offset,
			     void *val, size_t bytes)
{
	struct rockchip_otp *otp = context;
	int ret = -EINVAL;

	mutex_lock(&otp->lock);
	if (otp->data && otp->data->reg_read)
		ret = otp->data->reg_read(context, offset, val, bytes);
	mutex_unlock(&otp->lock);

	return ret;
}

static struct nvmem_config otp_config = {
	.name = "rockchip-otp",
	.owner = THIS_MODULE,
	.read_only = true,
	.stride = 1,
	.word_size = 1,
	.reg_read = rockchip_otp_read,
};

static const char * const px30_otp_clocks[] = {
	"otp", "apb_pclk", "phy",
};

static const struct rockchip_data px30_data = {
	.size = 0x40,
	.clocks = px30_otp_clocks,
	.num_clks = ARRAY_SIZE(px30_otp_clocks),
	.reg_read = px30_otp_read,
};

static const char * const rk3568_otp_clocks[] = {
	"usr", "sbpi", "apb", "phy",
};

static const struct rockchip_data rk3568_data = {
	.size = 0x80,
	.clocks = rk3568_otp_clocks,
	.num_clks = ARRAY_SIZE(rk3568_otp_clocks),
	.reg_read = rk3568_otp_read,
};

static const char * const rk3588_otp_clocks[] = {
	"otpc", "apb", "arb", "phy",
};

static const struct rockchip_data rk3588_data = {
	.size = 0x400,
	.clocks = rk3588_otp_clocks,
	.num_clks = ARRAY_SIZE(rk3588_otp_clocks),
	.reg_read = rk3588_otp_read,
};

static const struct of_device_id rockchip_otp_match[] = {
	{
		.compatible = "rockchip,px30-otp",
		.data = (void *)&px30_data,
	},
	{
		.compatible = "rockchip,rk3308-otp",
		.data = (void *)&px30_data,
	},
	{
		.compatible = "rockchip,rk3568-otp",
		.data = (void *)&rk3568_data,
	},
	{
		.compatible = "rockchip,rk3588-otp",
		.data = (void *)&rk3588_data,
	},
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, rockchip_otp_match);

static int rockchip_otp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rockchip_otp *otp;
	const struct rockchip_data *data;
	struct nvmem_device *nvmem;
	int ret, i;

	data = of_device_get_match_data(dev);
	if (!data) {
		dev_err(dev, "failed to get match data\n");
		return -EINVAL;
	}

	otp = devm_kzalloc(&pdev->dev, sizeof(struct rockchip_otp),
			   GFP_KERNEL);
	if (!otp)
		return -ENOMEM;

	mutex_init(&otp->lock);
	otp->data = data;
	otp->dev = dev;
	otp->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(otp->base))
		return PTR_ERR(otp->base);

	otp->num_clks = data->num_clks;
	otp->clks = devm_kcalloc(dev, otp->num_clks,
				     sizeof(*otp->clks), GFP_KERNEL);
	if (!otp->clks)
		return -ENOMEM;

	for (i = 0; i < otp->num_clks; ++i)
		otp->clks[i].id = data->clocks[i];

	ret = devm_clk_bulk_get(dev, otp->num_clks, otp->clks);
	if (ret)
		return ret;

	otp->rst = devm_reset_control_array_get_optional_exclusive(dev);
	if (IS_ERR(otp->rst))
		return PTR_ERR(otp->rst);

	otp->config = &otp_config;
	otp->config->size = data->size;
	otp->config->priv = otp;
	otp->config->dev = dev;

	if (data->init) {
		ret = data->init(otp);
		if (ret)
			return ret;
	}

	nvmem = devm_nvmem_register(dev, otp->config);

	return PTR_ERR_OR_ZERO(nvmem);
}

static struct platform_driver rockchip_otp_driver = {
	.probe = rockchip_otp_probe,
	.driver = {
		.name = "rockchip-otp",
		.of_match_table = rockchip_otp_match,
	},
};

module_platform_driver(rockchip_otp_driver);
MODULE_DESCRIPTION("Rockchip OTP driver");
MODULE_LICENSE("GPL v2");
