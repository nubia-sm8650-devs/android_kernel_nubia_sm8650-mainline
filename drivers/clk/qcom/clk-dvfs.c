// SPDX-License-Identifier: GPL-2.0-only

#include <linux/device.h>
#include <linux/device/bus.h>
#include <linux/debugfs.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "clk-dvfs.h"
#include "clk-regmap.h"

struct qcom_clk_dvfs_vote {
	struct list_head node;
	const char *clock_name;
	unsigned int level;
	bool transition;
};

struct qcom_clk_dvfs_rail_voter {
	struct device *dev;
	struct device_link *link;
	struct notifier_block nb;
	const char *name;
	/* Serializes the vote list and rail voter state. */
	struct mutex lock;
	struct list_head votes;
	unsigned int last_successful_vote_level;
	/*
	 * Local conservative floor included in subsequent aggregation after a
	 * failed request.
	 */
	unsigned int safety_floor_level;
	bool active;
	bool must_contribute;
};

struct qcom_clk_dvfs_clock {
	struct qcom_clk_dvfs_rail_voter *rail_voters;
	const struct qcom_clk_dvfs_data *data;
	struct qcom_clk_dvfs_vote *votes;
	struct clk_regmap *rclk;
};

struct qcom_clk_dvfs_transition {
	struct qcom_clk_dvfs_vote *votes;
	size_t num_votes;
};

struct qcom_clk_dvfs {
	struct device *dev;
	struct qcom_clk_dvfs_rail_voter *rail_voters;
	struct qcom_clk_dvfs_clock *clocks;
	size_t num_rail_voters;
	size_t num_clocks;
};

static DEFINE_MUTEX(qcom_clk_dvfs_debugfs_lock);
static struct dentry *qcom_clk_dvfs_debugfs_root;

static int
qcom_clk_dvfs_find_level(const struct qcom_clk_dvfs_data *data, unsigned long rate,
			 unsigned int *level)
{
	for (size_t i = 0; i < data->num_rates; i++) {
		/* PLL-derived rates are not Hz-precise. */
		if (DIV_ROUND_CLOSEST(rate, 1000) <=
		    DIV_ROUND_CLOSEST(data->rates[i].max_rate, 1000)) {
			*level = data->rates[i].level;
			return 0;
		}
	}

	return -ERANGE;
}

static unsigned int
qcom_clk_dvfs_aggregate(struct qcom_clk_dvfs_rail_voter *rail_voter)
{
	struct qcom_clk_dvfs_vote *vote;
	unsigned int level = 0;

	list_for_each_entry(vote, &rail_voter->votes, node)
		level = max(level, vote->level);

	return max(level, rail_voter->safety_floor_level);
}

static int qcom_clk_dvfs_debugfs_show(struct seq_file *s, void *data)
{
	struct qcom_clk_dvfs *dvfs = dev_get_drvdata(s->private);

	for (size_t i = 0; i < dvfs->num_rail_voters; i++) {
		struct qcom_clk_dvfs_rail_voter *rail_voter = &dvfs->rail_voters[i];
		struct qcom_clk_dvfs_vote *vote;

		guard(mutex)(&rail_voter->lock);
		seq_printf(s, "%s (aggregate=%u)\n", rail_voter->name,
			   qcom_clk_dvfs_aggregate(rail_voter));
		seq_printf(s, "  last_successful=%u safety_floor=%u must_contribute=%u\n",
			   rail_voter->last_successful_vote_level,
			   rail_voter->safety_floor_level, rail_voter->must_contribute);
		list_for_each_entry(vote, &rail_voter->votes, node) {
			int padding;

			if (!vote->level)
				continue;

			padding = max(32 - (int)strlen(vote->clock_name), 0);
			seq_printf(s, "  %s:%*s%s level=%u\n", vote->clock_name,
				   padding, "",
				   vote->transition ? "transition" : "prepared", vote->level);
		}
	}

	return 0;
}

static int qcom_clk_dvfs_activate(struct qcom_clk_dvfs_rail_voter *rail_voter)
{
	int ret;

	if (!rail_voter->active) {
		ret = pm_runtime_resume_and_get(rail_voter->dev);
		if (ret)
			return ret;
		rail_voter->active = true;
	}

	WRITE_ONCE(rail_voter->must_contribute, true);

	return 0;
}

static int qcom_clk_dvfs_apply(struct qcom_clk_dvfs_rail_voter *rail_voter,
			       unsigned int level)
{
	int ret;

	ret = qcom_clk_dvfs_activate(rail_voter);
	if (ret)
		return ret;
	ret = dev_pm_genpd_set_performance_state(rail_voter->dev, level);
	if (ret) {
		rail_voter->safety_floor_level =
			max(rail_voter->safety_floor_level,
			    max(rail_voter->last_successful_vote_level, level));
		return ret;
	}

	rail_voter->last_successful_vote_level = level;
	rail_voter->safety_floor_level = 0;

	return 0;
}

static int qcom_clk_dvfs_update(struct qcom_clk_dvfs_rail_voter *rail_voter)
{
	unsigned int level = qcom_clk_dvfs_aggregate(rail_voter);
	int ret;

	if (level)
		return qcom_clk_dvfs_apply(rail_voter, level);

	if (!rail_voter->active)
		return 0;

	ret = dev_pm_genpd_set_performance_state(rail_voter->dev, 0);
	if (ret) {
		rail_voter->safety_floor_level =
			max(rail_voter->safety_floor_level,
			    rail_voter->last_successful_vote_level);
		return ret;
	}

	rail_voter->last_successful_vote_level = 0;
	WRITE_ONCE(rail_voter->must_contribute, false);
	rail_voter->active = false;
	ret = pm_runtime_put_sync_suspend(rail_voter->dev);
	if (ret < 0)
		dev_warn(rail_voter->dev, "failed to suspend DVFS rail_voter: %d\n",
			 ret);

	return 0;
}

static int qcom_clk_dvfs_replace_vote(struct qcom_clk_dvfs_rail_voter *rail_voter,
				 struct qcom_clk_dvfs_vote *vote,
				 unsigned int level)
{
	unsigned int old_level = vote->level;
	int ret;

	guard(mutex)(&rail_voter->lock);
	vote->level = level;
	ret = qcom_clk_dvfs_update(rail_voter);
	if (ret)
		vote->level = old_level;

	return ret;
}

static int qcom_clk_dvfs_add_vote(struct qcom_clk_dvfs_rail_voter *rail_voter,
				   struct qcom_clk_dvfs_vote *vote,
				   unsigned int level)
{
	int ret;

	guard(mutex)(&rail_voter->lock);
	vote->level = level;
	list_add_tail(&vote->node, &rail_voter->votes);
	ret = qcom_clk_dvfs_update(rail_voter);
	if (ret) {
		list_del_init(&vote->node);
		vote->level = 0;
	}

	return ret;
}

static int qcom_clk_dvfs_remove_vote(struct qcom_clk_dvfs_rail_voter *rail_voter,
				      struct qcom_clk_dvfs_vote *vote)
{
	unsigned int old_level = vote->level;
	int ret;

	guard(mutex)(&rail_voter->lock);
	list_del_init(&vote->node);
	vote->level = 0;
	ret = qcom_clk_dvfs_update(rail_voter);
	if (ret) {
		/* The transaction record is freed; retain its error owner as a floor. */
		rail_voter->safety_floor_level = max(rail_voter->safety_floor_level,
						   old_level);
		WRITE_ONCE(rail_voter->must_contribute, true);
	}

	return ret;
}

int qcom_clk_dvfs_prepare_rate(struct clk_hw *hw, unsigned long rate)
{
	struct qcom_clk_dvfs_clock *clock = to_clk_regmap(hw)->dvfs;
	unsigned int *old_levels;
	unsigned int level;
	size_t i;
	int ret;

	if (!clock)
		return 0;

	ret = qcom_clk_dvfs_find_level(clock->data, rate, &level);
	if (ret)
		return ret;

	old_levels = kcalloc(clock->data->num_pd_indices, sizeof(*old_levels),
			     GFP_NOIO);
	if (!old_levels)
		return -ENOMEM;

	for (i = 0; i < clock->data->num_pd_indices; i++) {
		unsigned int pd_index = clock->data->pd_indices[i];

		old_levels[i] = clock->votes[i].level;
		ret = qcom_clk_dvfs_replace_vote(&clock->rail_voters[pd_index],
						 &clock->votes[i], level);
		if (ret)
			goto unwind;
	}

	kfree(old_levels);

	return 0;

unwind:
	while (i--) {
		unsigned int pd_index = clock->data->pd_indices[i];

		qcom_clk_dvfs_replace_vote(&clock->rail_voters[pd_index],
					   &clock->votes[i], old_levels[i]);
	}

	kfree(old_levels);

	return ret;
}

int qcom_clk_dvfs_prepare(struct clk_hw *hw)
{
	return qcom_clk_dvfs_prepare_rate(hw, clk_hw_get_rate(hw));
}

void qcom_clk_dvfs_unprepare(struct clk_hw *hw)
{
	struct qcom_clk_dvfs_clock *clock = to_clk_regmap(hw)->dvfs;

	if (!clock)
		return;

	for (size_t i = clock->data->num_pd_indices; i-- > 0;) {
		unsigned int pd_index = clock->data->pd_indices[i];

		if (qcom_clk_dvfs_replace_vote(&clock->rail_voters[pd_index],
						  &clock->votes[i], 0))
			dev_warn(clock->rail_voters[pd_index].dev,
				 "failed to release %s DVFS requirement\n",
				 clk_hw_get_name(hw));
	}
}

int qcom_clk_dvfs_resource_acquire(struct clk_hw *hw,
				 const struct clk_resource_transition *transition,
				 void **cookie)
{
	struct qcom_clk_dvfs_clock *clock = to_clk_regmap(hw)->dvfs;
	struct qcom_clk_dvfs_transition *dvfs_transition __free(kfree) = NULL;
	struct qcom_clk_dvfs_vote *votes __free(kfree) = NULL;
	unsigned int level;
	size_t i;
	int ret;

	if (!clock) {
		*cookie = NULL;
		return 0;
	}

	dvfs_transition = kzalloc_obj(*dvfs_transition, GFP_NOIO);
	if (!dvfs_transition)
		return -ENOMEM;

	votes = kcalloc(clock->data->num_pd_indices, sizeof(*votes),
			 GFP_NOIO);
	if (!votes)
		return -ENOMEM;

	dvfs_transition->votes = votes;
	dvfs_transition->num_votes = clock->data->num_pd_indices;
	ret = qcom_clk_dvfs_find_level(clock->data, transition->new_rate, &level);
	if (ret)
		return ret;

	for (i = 0; i < dvfs_transition->num_votes; i++) {
		unsigned int pd_index = clock->data->pd_indices[i];

		dvfs_transition->votes[i].clock_name = clk_hw_get_name(hw);
		dvfs_transition->votes[i].transition = true;
		ret = qcom_clk_dvfs_add_vote(&clock->rail_voters[pd_index],
						  &dvfs_transition->votes[i], level);
		if (ret)
			goto unwind;
	}

	dvfs_transition->votes = no_free_ptr(votes);
	*cookie = no_free_ptr(dvfs_transition);

	return 0;

unwind:
	while (i--) {
		unsigned int pd_index = clock->data->pd_indices[i];

		qcom_clk_dvfs_remove_vote(&clock->rail_voters[pd_index],
					    &dvfs_transition->votes[i]);
	}

	return ret;
}

int qcom_clk_dvfs_resource_release(struct clk_hw *hw,
				 const struct clk_resource_transition *transition,
				 void *cookie,
				 enum clk_resource_change_result result)
{
	struct qcom_clk_dvfs_transition *dvfs_transition = cookie;
	struct qcom_clk_dvfs_clock *clock = to_clk_regmap(hw)->dvfs;
	int first_error = 0;
	unsigned int level;
	int ret;

	if (!clock)
		return 0;
	if (result == CLK_RESOURCE_COMPLETE) {
		ret = qcom_clk_dvfs_find_level(clock->data, transition->actual_rate,
					   &level);
		if (ret) {
			first_error = ret;
			result = CLK_RESOURCE_HW_CHANGED;
		}
	}

	for (size_t i = 0; i < dvfs_transition->num_votes; i++) {
		unsigned int pd_index = clock->data->pd_indices[i];

		if (!clock->votes[i].level)
			continue;

		if (result == CLK_RESOURCE_HW_CHANGED)
			level = max(clock->votes[i].level,
				    dvfs_transition->votes[i].level);
		else if (result != CLK_RESOURCE_COMPLETE)
			continue;

		ret = qcom_clk_dvfs_replace_vote(&clock->rail_voters[pd_index],
						 &clock->votes[i], level);
		if (ret && !first_error)
			first_error = ret;
	}

	for (size_t i = dvfs_transition->num_votes; i-- > 0;) {
		unsigned int pd_index = clock->data->pd_indices[i];

		ret = qcom_clk_dvfs_remove_vote(&clock->rail_voters[pd_index],
						  &dvfs_transition->votes[i]);
		if (ret && !first_error)
			first_error = ret;
	}

	kfree(dvfs_transition->votes);
	kfree(dvfs_transition);

	return first_error;
}

static int qcom_clk_dvfs_notifier(struct notifier_block *nb,
				  unsigned long event, void *data)
{
	struct qcom_clk_dvfs_rail_voter *rail_voter =
		container_of(nb, struct qcom_clk_dvfs_rail_voter, nb);

	if (event == GENPD_NOTIFY_PRE_OFF &&
	    READ_ONCE(rail_voter->must_contribute))
		return notifier_from_errno(-EBUSY);

	return NOTIFY_OK;
}

static void qcom_clk_dvfs_debugfs_remove(void *data)
{
	struct qcom_clk_dvfs *dvfs = data;

	guard(mutex)(&qcom_clk_dvfs_debugfs_lock);
	if (!IS_ERR_OR_NULL(qcom_clk_dvfs_debugfs_root))
		debugfs_lookup_and_remove(dev_name(dvfs->dev),
					  qcom_clk_dvfs_debugfs_root);
}

static void qcom_clk_dvfs_cleanup(void *data)
{
	struct qcom_clk_dvfs *dvfs = data;

	for (size_t i = 0; i < dvfs->num_clocks; i++)
		dvfs->clocks[i].rclk->dvfs = NULL;

	for (size_t i = dvfs->num_rail_voters; i-- > 0;) {
		struct qcom_clk_dvfs_rail_voter *rail_voter = &dvfs->rail_voters[i];

		if (rail_voter->active) {
			dev_pm_genpd_set_performance_state(rail_voter->dev, 0);
			WRITE_ONCE(rail_voter->must_contribute, false);
			pm_runtime_put_sync_suspend(rail_voter->dev);
		}
		dev_pm_genpd_remove_notifier(rail_voter->dev);
		device_link_del(rail_voter->link);
		dev_pm_domain_detach(rail_voter->dev, true);
		mutex_destroy(&rail_voter->lock);
	}
}

static int
qcom_clk_dvfs_validate_clock(size_t num_rail_voters,
			     const struct qcom_clk_dvfs_data *data)
{
	if (!data->num_pd_indices || !data->pd_indices || !data->num_rates ||
	    !data->rates)
		return -EINVAL;

	for (size_t i = 0; i < data->num_pd_indices; i++) {
		if (data->pd_indices[i] >= num_rail_voters)
			return -EINVAL;
		for (size_t j = 0; j < i; j++)
			if (data->pd_indices[j] == data->pd_indices[i])
				return -EINVAL;
	}

	for (size_t i = 0; i < data->num_rates; i++) {
		if (!data->rates[i].max_rate || !data->rates[i].level)
			return -EINVAL;
		if (i && data->rates[i - 1].max_rate >= data->rates[i].max_rate)
			return -EINVAL;
	}

	return 0;
}

struct device *qcom_clk_dvfs_get_rail_voter(struct device *dev,
					     unsigned int pd_index)
{
	struct qcom_clk_dvfs *dvfs = dev_get_drvdata(dev);

	if (!dvfs || pd_index >= dvfs->num_rail_voters)
		return ERR_PTR(-EINVAL);

	return dvfs->rail_voters[pd_index].dev;
}

int qcom_clk_dvfs_register(struct device *dev,
			   size_t num_rail_voters,
			   struct clk_regmap **clks,
			   size_t num_clks)
{
	struct qcom_clk_dvfs *dvfs;
	size_t num_dvfs_clocks = 0;
	int ret;

	if (!dev || !dev->of_node || !clks || !num_rail_voters)
		return -EINVAL;

	for (size_t i = 0; i < num_clks; i++) {
		const struct clk_init_data *init;
		bool has_dvfs_data;
		bool has_dvfs_resource_ops;

		if (!clks[i])
			continue;

		init = clks[i]->hw.init;
		has_dvfs_data = !!clks[i]->dvfs_data;
		has_dvfs_resource_ops = init && init->ops &&
			init->ops->resource_acquire == qcom_clk_dvfs_resource_acquire &&
			init->ops->resource_release == qcom_clk_dvfs_resource_release;
		if (has_dvfs_data && !has_dvfs_resource_ops) {
			dev_warn(dev, "%s has DVFS data without matching DVFS resource ops\n",
				 init ? init->name : "an uninitialized clock");
			return -EINVAL;
		}
		if (!has_dvfs_data)
			continue;
		if (qcom_clk_dvfs_validate_clock(num_rail_voters,
						  clks[i]->dvfs_data))
			return -EINVAL;
		num_dvfs_clocks++;
	}
	if (!num_dvfs_clocks)
		return -EINVAL;

	dvfs = devm_kzalloc(dev, sizeof(*dvfs), GFP_KERNEL);
	if (!dvfs)
		return -ENOMEM;
	dvfs->dev = dev;
	dvfs->rail_voters = devm_kcalloc(dev, num_rail_voters,
					  sizeof(*dvfs->rail_voters), GFP_KERNEL);
	dvfs->clocks = devm_kcalloc(dev, num_dvfs_clocks, sizeof(*dvfs->clocks),
				  GFP_KERNEL);
	if (!dvfs->rail_voters || !dvfs->clocks)
		return -ENOMEM;

	for (size_t i = 0, j = 0; i < num_clks; i++) {
		struct clk_regmap *rclk = clks[i];

		if (!rclk || !rclk->dvfs_data)
			continue;

		dvfs->clocks[j].rclk = rclk;
		j++;
	}

	for (size_t i = 0; i < num_rail_voters; i++) {
		struct qcom_clk_dvfs_rail_voter *rail_voter = &dvfs->rail_voters[i];

		mutex_init(&rail_voter->lock);
		INIT_LIST_HEAD(&rail_voter->votes);
		rail_voter->dev = genpd_dev_pm_attach_by_id_suffix(dev, i, "dvfs");
		if (IS_ERR_OR_NULL(rail_voter->dev)) {
			ret = rail_voter->dev ? PTR_ERR(rail_voter->dev) : -ENODEV;
			goto err_cleanup;
		}
		rail_voter->name = pd_to_genpd(rail_voter->dev->pm_domain)->name;

		rail_voter->link = device_link_add(dev, rail_voter->dev,
						     DL_FLAG_STATELESS);
		if (!rail_voter->link) {
			ret = -EINVAL;
			dev_pm_domain_detach(rail_voter->dev, true);
			goto err_cleanup;
		}

		rail_voter->nb.notifier_call = qcom_clk_dvfs_notifier;
		ret = dev_pm_genpd_add_notifier(rail_voter->dev, &rail_voter->nb);
		if (ret) {
			device_link_del(rail_voter->link);
			dev_pm_domain_detach(rail_voter->dev, true);
			goto err_cleanup;
		}
		dvfs->num_rail_voters++;
	}

	for (size_t i = 0; i < num_dvfs_clocks; i++) {
		struct qcom_clk_dvfs_clock *clock = &dvfs->clocks[i];
		const struct qcom_clk_dvfs_data *data = clock->rclk->dvfs_data;

		clock->votes = devm_kcalloc(dev, data->num_pd_indices,
					  sizeof(*clock->votes), GFP_KERNEL);
		if (!clock->votes) {
			ret = -ENOMEM;
			goto err_cleanup;
		}

		clock->data = data;
		clock->rail_voters = dvfs->rail_voters;
		for (size_t j = 0; j < data->num_pd_indices; j++) {
			struct qcom_clk_dvfs_rail_voter *rail_voter;
			unsigned int pd_index;

			pd_index = data->pd_indices[j];
			rail_voter = &dvfs->rail_voters[pd_index];
			clock->votes[j].clock_name = clock->rclk->hw.init->name;
			guard(mutex)(&rail_voter->lock);
			list_add_tail(&clock->votes[j].node, &rail_voter->votes);
		}

		clock->rclk->dvfs = clock;
		dvfs->num_clocks++;
	}

	dev_set_drvdata(dev, dvfs);

	ret = devm_add_action_or_reset(dev, qcom_clk_dvfs_cleanup, dvfs);
	if (ret)
		return ret;

	scoped_guard(mutex, &qcom_clk_dvfs_debugfs_lock) {
		if (!qcom_clk_dvfs_debugfs_root)
			qcom_clk_dvfs_debugfs_root = debugfs_create_dir("qcom_clk_dvfs", NULL);

		debugfs_create_devm_seqfile(dvfs->dev, dev_name(dvfs->dev),
					    qcom_clk_dvfs_debugfs_root,
					    qcom_clk_dvfs_debugfs_show);
	}

	return devm_add_action_or_reset(dev, qcom_clk_dvfs_debugfs_remove, dvfs);

err_cleanup:
	qcom_clk_dvfs_cleanup(dvfs);

	return ret;
}
