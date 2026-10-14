/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __QCOM_CLK_DVFS_H__
#define __QCOM_CLK_DVFS_H__

#include <linux/clk-provider.h>
#include <linux/types.h>

struct device;
struct clk_regmap;

struct qcom_clk_dvfs_rate {
	unsigned long max_rate;
	unsigned int level;
};

struct qcom_clk_dvfs_data {
	const unsigned int *pd_indices;
	size_t num_pd_indices;
	const struct qcom_clk_dvfs_rate *rates;
	size_t num_rates;
};

int qcom_clk_dvfs_resource_acquire(struct clk_hw *hw,
				 const struct clk_resource_transition *transition,
				 void **cookie);
int qcom_clk_dvfs_resource_release(struct clk_hw *hw,
				 const struct clk_resource_transition *transition,
				 void *cookie,
				 enum clk_resource_change_result result);
int qcom_clk_dvfs_prepare(struct clk_hw *hw);
void qcom_clk_dvfs_unprepare(struct clk_hw *hw);

int qcom_clk_dvfs_register(struct device *dev,
			   size_t num_rail_voters,
			   struct clk_regmap **clks,
			   size_t num_clks);
struct device *qcom_clk_dvfs_get_rail_voter(struct device *dev,
					     unsigned int pd_index);

#endif
