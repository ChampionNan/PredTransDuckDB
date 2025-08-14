#include "duckdb/optimizer/statistics_propagator.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"

namespace duckdb {

unique_ptr<NodeStatistics> StatisticsPropagator::PropagateStatistics(LogicalAggregate &aggr,
                                                                     unique_ptr<LogicalOperator> *node_ptr) {
	// first propagate statistics in the child node
	node_stats = PropagateStatistics(aggr.children[0]);

	// handle the groups: simply propagate statistics and assign the stats to the group binding
	aggr.group_stats.resize(aggr.groups.size());
	for (idx_t group_idx = 0; group_idx < aggr.groups.size(); group_idx++) {
		auto stats = PropagateExpression(aggr.groups[group_idx]);
		aggr.group_stats[group_idx] = stats ? stats->ToUnique() : nullptr;
		if (!stats) {
			continue;
		}
		if (aggr.grouping_sets.size() > 1) {
			// aggregates with multiple grouping sets can introduce NULL values to certain groups
			// FIXME: actually figure out WHICH groups can have null values introduced
			stats->Set(StatsInfo::CAN_HAVE_NULL_VALUES);
			continue;
		}
		ColumnBinding group_binding(aggr.group_index, group_idx);
		statistics_map[group_binding] = std::move(stats);
	}
	// propagate statistics in the aggregates
	for (idx_t aggregate_idx = 0; aggregate_idx < aggr.expressions.size(); aggregate_idx++) {
		auto stats = PropagateExpression(aggr.expressions[aggregate_idx]);
		if (!stats) {
			continue;
		}
		ColumnBinding aggregate_binding(aggr.aggregate_index, aggregate_idx);
		statistics_map[aggregate_binding] = std::move(stats);
	}

	// NOTE: Operation for update statistics & store for correspond logical_operator
	auto result_stats = make_uniq<NodeStatistics>();

	if (aggr.groups.empty()) {
		result_stats->has_estimated_cardinality = 1;
		result_stats->estimated_cardinality = true;
		result_stats->max_cardinality = 1;
		result_stats->has_max_cardinality = true;
		aggr.estimated_cardinality = 1;
		aggr.has_estimated_cardinality = true;
	} else {
		idx_t estimateted_groups = 1;
		bool has_valid_stats = false;

		for (idx_t group_idx = 0; group_idx < aggr.group_stats.size(); group_idx++) {
			if (aggr.group_stats[group_idx]) {
				auto distinct_count = aggr.group_stats[group_idx]->GetDistinctCount();
				if (distinct_count > 0) {
					estimateted_groups = std::min(estimateted_groups * distinct_count, 
												  node_stats ? node_stats->estimated_cardinality : STANDARD_VECTOR_SIZE);
					has_valid_stats = true;
				}
			}
		}

		if (has_valid_stats) {
			result_stats->has_estimated_cardinality = true;
			result_stats->estimated_cardinality = estimateted_groups;
		} else {
			result_stats->has_estimated_cardinality = true;
			result_stats->estimated_cardinality = node_stats->estimated_cardinality;
		}
		aggr.estimated_cardinality = result_stats->estimated_cardinality;
		aggr.has_estimated_cardinality = true;
	}

	if (node_stats && node_stats->has_max_cardinality) {
		result_stats->has_max_cardinality = true;
		result_stats->max_cardinality = node_stats->max_cardinality;
	} else {
		result_stats->has_max_cardinality = true;
		result_stats->max_cardinality = result_stats->estimated_cardinality;
	}

	// the max cardinality of an aggregate is the max cardinality of the input (i.e. when every row is a unique group)
	return std::move(result_stats);
}

} // namespace duckdb
