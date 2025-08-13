#include "duckdb/optimizer/statistics_propagator.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

namespace duckdb {

unique_ptr<NodeStatistics> StatisticsPropagator::PropagateStatistics(LogicalProjection &proj,
                                                                     unique_ptr<LogicalOperator> *node_ptr) {
	// first propagate to the child
	node_stats = PropagateStatistics(proj.children[0]);
	if (proj.children[0]->type == LogicalOperatorType::LOGICAL_EMPTY_RESULT) {
		ReplaceWithEmptyResult(*node_ptr);
		proj.estimated_cardinality = 0;
		proj.has_estimated_cardinality = true;
		return std::move(node_stats);
	}
	// then propagate to each of the expressions
	for (idx_t i = 0; i < proj.expressions.size(); i++) {
		auto stats = PropagateExpression(proj.expressions[i]);
		if (stats) {
			ColumnBinding binding(proj.table_index, i);
			statistics_map.insert(make_pair(binding, std::move(stats)));
		}
	}

	// NOTE: Update
	auto result_stats = make_uniq<NodeStatistics>();
	if (node_stats) {
		if (node_stats->has_estimated_cardinality) {
			result_stats->estimated_cardinality = node_stats->estimated_cardinality;
			result_stats->has_estimated_cardinality = true;

			proj.estimated_cardinality = node_stats->estimated_cardinality;
			proj.has_estimated_cardinality = true;
		}
		if (node_stats->has_max_cardinality) {
			result_stats->max_cardinality = node_stats->max_cardinality;
			result_stats->has_max_cardinality = true;
		}
	}

	return std::move(result_stats);
}

} // namespace duckdb
