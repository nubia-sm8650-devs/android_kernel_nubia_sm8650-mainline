// SPDX-License-Identifier: GPL-2.0
/* Tests for resource and parent-fault recovery during clock transitions. */
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/module.h>

#include <kunit/clk.h>
#include <kunit/test.h>

struct clk_resource_test_clock {
	struct clk_hw hw;
	struct clk *clk;
	unsigned long rate;
	unsigned int acquire_calls;
	unsigned int release_calls;
	unsigned int set_rate_calls;
	int release_error;
	int parent_error;
	u8 parent_index;
};

#define to_clk_resource_test(_hw) \
	container_of(_hw, struct clk_resource_test_clock, hw)

static unsigned long clk_resource_test_recalc(struct clk_hw *hw,
					     unsigned long parent_rate)
{
	return to_clk_resource_test(hw)->rate;
}

static unsigned long clk_resource_test_pass_recalc(struct clk_hw *hw,
						  unsigned long parent_rate)
{
	return parent_rate;
}

static int clk_resource_test_determine(struct clk_hw *hw,
				       struct clk_rate_request *req)
{
	return 0;
}

static int clk_resource_test_pass_determine(struct clk_hw *hw,
					    struct clk_rate_request *req)
{
	struct clk_hw *parent = clk_hw_get_parent(hw);
	struct clk_rate_request parent_req;
	int ret;

	clk_hw_forward_rate_request(hw, req, parent, &parent_req, req->rate);
	ret = __clk_determine_rate(parent, &parent_req);
	if (ret)
		return ret;

	req->rate = parent_req.rate;
	req->best_parent_rate = parent_req.rate;
	req->best_parent_hw = parent;

	return 0;
}

static int clk_resource_test_set_rate(struct clk_hw *hw, unsigned long rate,
				      unsigned long parent_rate)
{
	struct clk_resource_test_clock *clock = to_clk_resource_test(hw);

	clock->rate = rate;
	clock->set_rate_calls++;

	return 0;
}

static int
clk_resource_test_acquire(struct clk_hw *hw,
			  const struct clk_resource_transition *transition,
			  void **cookie)
{
	to_clk_resource_test(hw)->acquire_calls++;
	*cookie = NULL;

	return 0;
}

static int
clk_resource_test_release(struct clk_hw *hw,
			  const struct clk_resource_transition *transition,
			  void *cookie, enum clk_resource_change_result result)
{
	struct clk_resource_test_clock *clock = to_clk_resource_test(hw);

	clock->release_calls++;

	return clock->release_error;
}

static u8 clk_resource_test_get_parent(struct clk_hw *hw)
{
	return to_clk_resource_test(hw)->parent_index;
}

static int clk_resource_test_set_parent(struct clk_hw *hw, u8 index)
{
	struct clk_resource_test_clock *clock = to_clk_resource_test(hw);

	if (clock->parent_error)
		return clock->parent_error;
	clock->parent_index = index;

	return 0;
}

static const struct clk_ops clk_resource_test_rate_ops = {
	.recalc_rate = clk_resource_test_recalc,
	.determine_rate = clk_resource_test_determine,
	.set_rate = clk_resource_test_set_rate,
	.resource_acquire = clk_resource_test_acquire,
	.resource_release = clk_resource_test_release,
};

static const struct clk_ops clk_resource_test_pass_ops = {
	.recalc_rate = clk_resource_test_pass_recalc,
	.determine_rate = clk_resource_test_pass_determine,
};

static const struct clk_ops clk_resource_test_mux_ops = {
	.determine_rate = __clk_mux_determine_rate,
	.recalc_rate = clk_resource_test_pass_recalc,
	.get_parent = clk_resource_test_get_parent,
	.set_parent = clk_resource_test_set_parent,
};

static struct clk_resource_test_clock *
clk_resource_test_register(struct kunit *test, const char *name,
			   const struct clk_ops *ops,
			   const struct clk_hw **parents, u8 num_parents,
			   unsigned long flags)
{
	struct clk_init_data init = {
		.name = name,
		.ops = ops,
		.parent_hws = parents,
		.num_parents = num_parents,
		.flags = flags,
	};
	struct clk_resource_test_clock *clock;
	int ret;

	clock = kunit_kzalloc(test, sizeof(*clock), GFP_KERNEL);
	if (!clock)
		return ERR_PTR(-ENOMEM);

	clock->rate = 100;
	clock->hw.init = &init;
	ret = clk_hw_register_kunit(test, NULL, &clock->hw);
	if (ret)
		return ERR_PTR(ret);

	clock->clk = clk_hw_get_clk_kunit(test, &clock->hw, NULL);
	if (IS_ERR(clock->clk))
		return ERR_CAST(clock->clk);

	return clock;
}

static void clk_resource_retry_reconciles_ancestor_test(struct kunit *test)
{
	struct clk_resource_test_clock *root, *middle, *leaf;
	const struct clk_hw *parents[1];

	root = clk_resource_test_register(test, "resource-root",
					  &clk_resource_test_rate_ops, NULL, 0, 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, root);
	parents[0] = &root->hw;
	middle = clk_resource_test_register(test, "resource-middle",
					    &clk_resource_test_pass_ops, parents, 1,
					    CLK_SET_RATE_PARENT);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, middle);
	parents[0] = &middle->hw;
	leaf = clk_resource_test_register(test, "resource-leaf",
					  &clk_resource_test_pass_ops, parents, 1,
					  CLK_SET_RATE_PARENT);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, leaf);

	root->release_error = -EIO;
	KUNIT_ASSERT_EQ(test, -EIO, clk_set_rate(leaf->clk, 200));
	KUNIT_ASSERT_EQ(test, 1U, root->release_calls);
	root->release_error = 0;
	KUNIT_EXPECT_EQ(test, 0, clk_set_rate(leaf->clk, 200));
	KUNIT_EXPECT_EQ(test, 2U, root->release_calls);
}

static void clk_resource_unchanged_child_acquires_resources_test(struct kunit *test)
{
	struct clk_resource_test_clock *root, *child;
	const struct clk_hw *parents[1];

	root = clk_resource_test_register(test, "resource-root",
					  &clk_resource_test_rate_ops, NULL, 0, 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, root);
	parents[0] = &root->hw;
	child = clk_resource_test_register(test, "resource-child",
					   &clk_resource_test_rate_ops, parents, 1, 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, child);

	KUNIT_ASSERT_EQ(test, 0, clk_set_rate(root->clk, 200));
	KUNIT_ASSERT_EQ(test, 1U, child->set_rate_calls);
	KUNIT_EXPECT_EQ(test, 1U, child->acquire_calls);
}

static void clk_resource_unprepare_after_parent_fault_test(struct kunit *test)
{
	struct clk_resource_test_clock *old_parent, *new_parent, *mux;
	const struct clk_hw *parents[2];

	old_parent = clk_resource_test_register(test, "resource-old",
						&clk_resource_test_rate_ops, NULL, 0, 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, old_parent);
	new_parent = clk_resource_test_register(test, "resource-new",
						&clk_resource_test_rate_ops, NULL, 0, 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, new_parent);
	parents[0] = &old_parent->hw;
	parents[1] = &new_parent->hw;
	mux = clk_resource_test_register(test, "resource-mux",
					 &clk_resource_test_mux_ops, parents, 2, 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, mux);

	KUNIT_ASSERT_EQ(test, 0, clk_prepare_enable(mux->clk));
	mux->parent_error = -EIO;
	KUNIT_ASSERT_EQ(test, -EIO, clk_set_parent(mux->clk, new_parent->clk));
	clk_disable_unprepare(mux->clk);
	mux->parent_error = 0;
	KUNIT_EXPECT_EQ(test, 0, clk_set_parent(mux->clk, new_parent->clk));
	KUNIT_EXPECT_FALSE(test, clk_hw_is_prepared(&old_parent->hw));
	KUNIT_EXPECT_EQ(test, 0U, __clk_get_enable_count(mux->clk));
}

static void clk_resource_prepare_after_parent_fault_test(struct kunit *test)
{
	struct clk_resource_test_clock *old_parent, *new_parent, *mux;
	const struct clk_hw *parents[2];

	old_parent = clk_resource_test_register(test, "resource-old",
						&clk_resource_test_rate_ops, NULL, 0, 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, old_parent);
	new_parent = clk_resource_test_register(test, "resource-new",
						&clk_resource_test_rate_ops, NULL, 0, 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, new_parent);
	parents[0] = &old_parent->hw;
	parents[1] = &new_parent->hw;
	mux = clk_resource_test_register(test, "resource-mux",
					 &clk_resource_test_mux_ops, parents, 2,
					 CLK_OPS_PARENT_ENABLE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, mux);

	mux->parent_error = -EIO;
	KUNIT_ASSERT_EQ(test, -EIO, clk_set_parent(mux->clk, new_parent->clk));
	KUNIT_ASSERT_EQ(test, 0, clk_prepare_enable(mux->clk));
	mux->parent_error = 0;
	KUNIT_EXPECT_EQ(test, 0, clk_set_parent(mux->clk, new_parent->clk));
	KUNIT_EXPECT_FALSE(test, clk_hw_is_prepared(&old_parent->hw));
	KUNIT_EXPECT_EQ(test, 1U, __clk_get_enable_count(mux->clk));
	KUNIT_EXPECT_TRUE(test, clk_hw_is_prepared(&mux->hw));
	clk_disable_unprepare(mux->clk);
	KUNIT_EXPECT_FALSE(test, clk_hw_is_prepared(&new_parent->hw));
}

static void clk_resource_ancestor_parent_fault_recovery_test(struct kunit *test)
{
	struct clk_resource_test_clock *old_parent, *new_parent, *mux, *leaf;
	const struct clk_hw *parents[2];

	old_parent = clk_resource_test_register(test, "resource-old",
						&clk_resource_test_rate_ops, NULL, 0, 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, old_parent);
	new_parent = clk_resource_test_register(test, "resource-new",
						&clk_resource_test_rate_ops, NULL, 0, 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, new_parent);
	parents[0] = &old_parent->hw;
	parents[1] = &new_parent->hw;
	mux = clk_resource_test_register(test, "resource-mux",
					 &clk_resource_test_mux_ops, parents, 2, 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, mux);
	parents[0] = &mux->hw;
	leaf = clk_resource_test_register(test, "resource-leaf",
					  &clk_resource_test_pass_ops, parents, 1,
					  CLK_SET_RATE_PARENT);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, leaf);

	mux->parent_error = -EIO;
	KUNIT_ASSERT_EQ(test, -EIO, clk_set_parent(mux->clk, new_parent->clk));
	KUNIT_EXPECT_EQ(test, -EBUSY, clk_set_rate(leaf->clk, 100));
	mux->parent_error = 0;
	KUNIT_ASSERT_EQ(test, 0, clk_set_parent(mux->clk, new_parent->clk));
	KUNIT_EXPECT_EQ(test, 0, clk_set_rate(leaf->clk, 100));
}

static void clk_resource_grandparent_fault_test(struct kunit *test, bool rate_change)
{
	struct clk_resource_test_clock *old_root, *new_root;
	struct clk_resource_test_clock *old_parent, *new_parent, *mux;
	const struct clk_hw *parents[2];
	int ret;

	old_root = clk_resource_test_register(test, "resource-old-root",
					      &clk_resource_test_rate_ops, NULL, 0, 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, old_root);
	new_root = clk_resource_test_register(test, "resource-new-root",
					      &clk_resource_test_rate_ops, NULL, 0, 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, new_root);
	KUNIT_ASSERT_EQ(test, 0, clk_set_rate(new_root->clk, 200));

	parents[0] = &old_root->hw;
	old_parent = clk_resource_test_register(test, "resource-old-parent",
						&clk_resource_test_pass_ops, parents, 1, 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, old_parent);
	parents[0] = &new_root->hw;
	new_parent = clk_resource_test_register(test, "resource-new-parent",
						&clk_resource_test_pass_ops, parents, 1, 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, new_parent);
	parents[0] = &old_parent->hw;
	parents[1] = &new_parent->hw;
	mux = clk_resource_test_register(test, "resource-grandparent-mux",
					 &clk_resource_test_mux_ops, parents, 2, 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, mux);

	KUNIT_ASSERT_EQ(test, 0, clk_prepare_enable(mux->clk));
	mux->parent_error = -EIO;
	if (rate_change)
		ret = clk_set_rate(mux->clk, 200);
	else
		ret = clk_set_parent(mux->clk, new_parent->clk);
	KUNIT_EXPECT_EQ(test, -EIO, ret);

	clk_disable_unprepare(mux->clk);
	KUNIT_EXPECT_TRUE(test, clk_hw_is_prepared(&old_root->hw));
	KUNIT_EXPECT_TRUE(test, clk_hw_is_prepared(&new_root->hw));

	mux->parent_error = 0;
	if (rate_change)
		ret = clk_set_rate(mux->clk, 200);
	else
		ret = clk_set_parent(mux->clk, new_parent->clk);
	KUNIT_EXPECT_EQ(test, 0, ret);
	KUNIT_EXPECT_FALSE(test, clk_hw_is_prepared(&old_root->hw));
	KUNIT_EXPECT_FALSE(test, clk_hw_is_prepared(&new_root->hw));
	KUNIT_EXPECT_EQ(test, 0U, __clk_get_enable_count(mux->clk));
}

static void clk_resource_grandparent_set_parent_fault_test(struct kunit *test)
{
	clk_resource_grandparent_fault_test(test, false);
}

static void clk_resource_grandparent_set_rate_fault_test(struct kunit *test)
{
	clk_resource_grandparent_fault_test(test, true);
}

static struct kunit_case clk_resource_recovery_test_cases[] = {
	KUNIT_CASE(clk_resource_retry_reconciles_ancestor_test),
	KUNIT_CASE(clk_resource_unchanged_child_acquires_resources_test),
	KUNIT_CASE(clk_resource_unprepare_after_parent_fault_test),
	KUNIT_CASE(clk_resource_prepare_after_parent_fault_test),
	KUNIT_CASE(clk_resource_ancestor_parent_fault_recovery_test),
	KUNIT_CASE(clk_resource_grandparent_set_parent_fault_test),
	KUNIT_CASE(clk_resource_grandparent_set_rate_fault_test),
	{}
};

static struct kunit_suite clk_resource_recovery_test_suite = {
	.name = "clk-resource-recovery",
	.test_cases = clk_resource_recovery_test_cases,
};

kunit_test_suite(clk_resource_recovery_test_suite);
MODULE_LICENSE("GPL");
