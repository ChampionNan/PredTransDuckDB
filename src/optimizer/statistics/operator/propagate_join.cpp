#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/optimizer/filter_pushdown.hpp"
#include "duckdb/optimizer/statistics_propagator.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/operator/logical_any_join.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_cross_product.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_join.hpp"
#include "duckdb/planner/operator/logical_limit.hpp"
#include "duckdb/planner/operator/logical_positional_join.hpp"
#include "duckdb/common/enums/join_type.hpp"

namespace duckdb {

static void MaxCardinalities(unique_ptr<NodeStatistics> &stats, NodeStatistics &new_stats) {
	if (!stats->has_estimated_cardinality || !new_stats.has_estimated_cardinality || !stats->has_max_cardinality ||
	    !new_stats.has_max_cardinality) {
		stats = nullptr;
		return;
	}
	stats->estimated_cardinality = MaxValue<idx_t>(stats->estimated_cardinality, new_stats.estimated_cardinality);
	stats->max_cardinality = MaxValue<idx_t>(stats->max_cardinality, new_stats.max_cardinality);
}

void StatisticsPropagator::PropagateStatistics(LogicalComparisonJoin &join, unique_ptr<LogicalOperator> *node_ptr) {
	for (idx_t i = 0; i < join.conditions.size(); i++) {
		auto &condition = join.conditions[i];
		const auto stats_left = PropagateExpression(condition.left);
		const auto stats_right = PropagateExpression(condition.right);
		if (stats_left && stats_right) {
			if ((condition.comparison == ExpressionType::COMPARE_DISTINCT_FROM ||
			     condition.comparison == ExpressionType::COMPARE_NOT_DISTINCT_FROM) &&
			    stats_left->CanHaveNull() && stats_right->CanHaveNull()) {
				// null values are equal in this join, and both sides can have null values
				// nothing to do here
				continue;
			}
			auto prune_result = PropagateComparison(*stats_left, *stats_right, condition.comparison);
			// Add stats to logical_join for perfect hash join
			join.join_stats.push_back(stats_left->ToUnique());
			join.join_stats.push_back(stats_right->ToUnique());
			switch (prune_result) {
			case FilterPropagateResult::FILTER_FALSE_OR_NULL:
			case FilterPropagateResult::FILTER_ALWAYS_FALSE:
				// filter is always false or null, none of the join conditions matter
				switch (join.join_type) {
				case JoinType::RIGHT_SEMI:
				case JoinType::SEMI:
				case JoinType::INNER:
					// semi or inner join on false; entire node can be pruned
					ReplaceWithEmptyResult(*node_ptr);
					return;
				case JoinType::RIGHT_ANTI:
				case JoinType::ANTI: {
					if (join.join_type == JoinType::RIGHT_ANTI) {
						std::swap(join.children[0], join.children[1]);
					}
					// when the right child has data, return the left child
					// when the right child has no data, return an empty set
					auto limit = make_uniq<LogicalLimit>(1, 0, nullptr, nullptr);
					limit->AddChild(std::move(join.children[1]));
					auto cross_product = LogicalCrossProduct::Create(std::move(join.children[0]), std::move(limit));
					*node_ptr = std::move(cross_product);
					return;
				}
				case JoinType::LEFT:
					// anti/left outer join: replace right side with empty node
					ReplaceWithEmptyResult(join.children[1]);
					return;
				case JoinType::RIGHT:
					// right outer join: replace left side with empty node
					ReplaceWithEmptyResult(join.children[0]);
					return;
				default:
					// other join types: can't do much meaningful with this information
					// full outer join requires both sides anyway; we can skip the execution of the actual join, but eh
					// mark/single join requires knowing if the rhs has null values or not
					break;
				}
				break;
			case FilterPropagateResult::FILTER_ALWAYS_TRUE:
				// filter is always true
				// If this is the inequality for an AsOf join,
				// then we must leave it in because it also flags
				// the semantics of restricting to a single match
				// so we can't replace it with an equi-join on the remaining conditions.
				if (join.type == LogicalOperatorType::LOGICAL_ASOF_JOIN) {
					switch (condition.comparison) {
					case ExpressionType::COMPARE_GREATERTHAN:
					case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
					case ExpressionType::COMPARE_LESSTHAN:
					case ExpressionType::COMPARE_LESSTHANOREQUALTO:
						continue;
					default:
						break;
					}
				}
				if (join.conditions.size() > 1) {
					// there are multiple conditions: erase this condition
					join.conditions.erase(join.conditions.begin() + i);
					// remove the corresponding statistics
					join.join_stats.clear();
					i--;
					continue;
				} else {
					// this is the only condition and it is always true: all conditions are true
					switch (join.join_type) {
					case JoinType::RIGHT_SEMI:
					case JoinType::SEMI: {
						if (join.join_type == JoinType::RIGHT_SEMI) {
							std::swap(join.children[0], join.children[1]);
						}
						// when the right child has data, return the left child
						// when the right child has no data, return an empty set
						auto limit = make_uniq<LogicalLimit>(1, 0, nullptr, nullptr);
						limit->AddChild(std::move(join.children[1]));
						auto cross_product = LogicalCrossProduct::Create(std::move(join.children[0]), std::move(limit));
						*node_ptr = std::move(cross_product);
						return;
					}
					case JoinType::INNER: {
						// inner, replace with cross product
						auto cross_product =
						    LogicalCrossProduct::Create(std::move(join.children[0]), std::move(join.children[1]));
						*node_ptr = std::move(cross_product);
						return;
					}
					default:
						// we don't handle mark/single join here yet
						break;
					}
				}
				break;
			default:
				break;
			}
		}
		// after we have propagated, we can update the statistics on both sides
		// note that it is fine to do this now, even if the same column is used again later
		// e.g. if we have i=j AND i=k, and the stats for j and k are disjoint, we know there are no results
		// so if we have e.g. i: [0, 100], j: [0, 25], k: [75, 100]
		// we can set i: [0, 25] after the first comparison, and statically determine that the second comparison is fals

		// note that we can't update statistics the same for all join types
		// mark and single joins don't filter any tuples -> so there is no propagation possible
		// anti joins have inverse statistics propagation
		// (i.e. if we have an anti join on i: [0, 100] and j: [0, 25], the resulting stats are i:[25,100])
		// for now we don't handle anti joins
		if (condition.comparison == ExpressionType::COMPARE_DISTINCT_FROM ||
		    condition.comparison == ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
			// skip update when null values are equal (for now?)
			continue;
		}
		switch (join.join_type) {
		case JoinType::INNER:
		case JoinType::SEMI: {
			UpdateFilterStatistics(*condition.left, *condition.right, condition.comparison);
			auto updated_stats_left = PropagateExpression(condition.left);
			auto updated_stats_right = PropagateExpression(condition.right);

			// Try to push lhs stats down rhs and vice versa
			if (stats_left && stats_right && updated_stats_left && updated_stats_right &&
			    condition.left->type == ExpressionType::BOUND_COLUMN_REF &&
			    condition.right->type == ExpressionType::BOUND_COLUMN_REF) {
				CreateFilterFromJoinStats(join.children[0], condition.left, *stats_left, *updated_stats_left);
				CreateFilterFromJoinStats(join.children[1], condition.right, *stats_right, *updated_stats_right);
			}

			// Update join_stats when is already part of the join
			if (join.join_stats.size() == 2) {
				join.join_stats[0] = std::move(updated_stats_left);
				join.join_stats[1] = std::move(updated_stats_right);
			}
			break;
		}
		default:
			break;
		}
	}
}

void StatisticsPropagator::PropagateStatistics(LogicalAnyJoin &join, unique_ptr<LogicalOperator> *node_ptr) {
	// propagate the expression into the join condition
	PropagateExpression(join.condition);
}

void StatisticsPropagator::MultiplyCardinalities(unique_ptr<NodeStatistics> &stats, NodeStatistics &new_stats) {
	if (!stats->has_estimated_cardinality || !new_stats.has_estimated_cardinality || !stats->has_max_cardinality ||
	    !new_stats.has_max_cardinality) {
		stats = nullptr;
		return;
	}
	stats->estimated_cardinality = MaxValue<idx_t>(stats->estimated_cardinality, new_stats.estimated_cardinality);
	auto new_max = Hugeint::Multiply(stats->max_cardinality, new_stats.max_cardinality);
	if (new_max < NumericLimits<int64_t>::Maximum()) {
		int64_t result;
		if (!Hugeint::TryCast<int64_t>(new_max, result)) {
			throw InternalException("Overflow in cast in statistics propagation");
		}
		D_ASSERT(result >= 0);
		stats->max_cardinality = idx_t(result);
	} else {
		stats = nullptr;
	}
}

double StatisticsPropagator::EstimateConstantComparisonSelectivity(BaseStatistics &stats, 
                                                                  ExpressionType comparison_type, 
                                                                  const Value &constant) {
    // Handle null comparisons
    if (!IsCompareDistinct(comparison_type) && constant.IsNull()) {
        return 0.0;
    }
    
    if (!stats.GetType().IsNumeric() || !NumericStats::HasMinMax(stats)) {
        return 0.2; // Default value
    }
    
    auto min_val = NumericStats::Min(stats);
    auto max_val = NumericStats::Max(stats);
    
    switch (comparison_type) {
    case ExpressionType::COMPARE_EQUAL:
        return 1.0 / std::max(stats.GetDistinctCount(), (idx_t)1);
        
    case ExpressionType::COMPARE_LESSTHAN:
    case ExpressionType::COMPARE_LESSTHANOREQUALTO: {
		if (constant <= min_val) return 0.0;
        if (constant >= max_val) return 1.0;
        
        double range = max_val.GetValue<double>() - min_val.GetValue<double>();
        double position = constant.GetValue<double>() - min_val.GetValue<double>();
        return position / range;
	}
    case ExpressionType::COMPARE_GREATERTHAN:
    case ExpressionType::COMPARE_GREATERTHANOREQUALTO: {
		if (constant >= max_val) return 0.0;
        if (constant <= min_val) return 1.0;  
        double range = max_val.GetValue<double>() - min_val.GetValue<double>();
        double position = max_val.GetValue<double>() - constant.GetValue<double>();
        return position / range;
	}
    default:
        return 0.2; // default
    }
	return 0.2; // default
}

double StatisticsPropagator::EstimateColumnComparisonSelectivity(BaseStatistics &lstats, BaseStatistics &rstats, ExpressionType comparison_type) {
    // For non-numeric types or missing stats, fallback to distinct count approach
    if (!lstats.GetType().IsNumeric() || !rstats.GetType().IsNumeric() ||
        !NumericStats::HasMinMax(lstats) || !NumericStats::HasMinMax(rstats)) {
        
        // Fallback: Use distinct count approach for non-numeric data
        if (comparison_type == ExpressionType::COMPARE_EQUAL ||
            comparison_type == ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
            auto left_distinct = lstats.GetDistinctCount();
            auto right_distinct = rstats.GetDistinctCount();
            return 1.0 / std::max({left_distinct, right_distinct, (idx_t)1});
        }
        return GetDefaultSelectivity(comparison_type);
    }
    
    // For numeric types, use range-based analysis
    auto lmin = NumericStats::Min(lstats);
    auto lmax = NumericStats::Max(lstats);
    auto rmin = NumericStats::Min(rstats);
    auto rmax = NumericStats::Max(rstats);
    
    switch (comparison_type) {
    case ExpressionType::COMPARE_EQUAL:
    case ExpressionType::COMPARE_NOT_DISTINCT_FROM: {
        // Calculate overlap between ranges
        double overlap_min = std::max(lmin.GetValue<double>(), rmin.GetValue<double>());
        double overlap_max = std::min(lmax.GetValue<double>(), rmax.GetValue<double>());
        
        if (overlap_max <= overlap_min) {
            return 0.0; // No overlap = no matches
        }
        
        double left_range = lmax.GetValue<double>() - lmin.GetValue<double>();
        double right_range = rmax.GetValue<double>() - rmin.GetValue<double>();
        double overlap_range = overlap_max - overlap_min;
        
        if (left_range <= 0 || right_range <= 0) {
            // Single value ranges - use distinct count approach
            auto left_distinct = lstats.GetDistinctCount();
            auto right_distinct = rstats.GetDistinctCount();
            return 1.0 / std::max({left_distinct, right_distinct, (idx_t)1});
        }
        
        // Estimate selectivity based on overlap proportion
        // This accounts for the fact that only overlapping values can match
        double overlap_selectivity = std::min(1.0, (overlap_range / left_range) * (overlap_range / right_range));
        
        // Combine with distinct count information for better estimate
        auto left_distinct = lstats.GetDistinctCount();
        auto right_distinct = rstats.GetDistinctCount();
        double distinct_selectivity = 1.0 / std::max({left_distinct, right_distinct, (idx_t)1});
        
        // Use the more conservative (smaller) estimate
        return std::min(overlap_selectivity, distinct_selectivity);
    }
    
    case ExpressionType::COMPARE_LESSTHAN:
    case ExpressionType::COMPARE_LESSTHANOREQUALTO: {
        // LEFT < RIGHT or LEFT <= RIGHT
        if (lmax <= rmin) {
            return 1.0; // All left values are less than all right values
        }
        if (lmin >= rmax) {
            return 0.0; // No left values are less than right values
        }
        
        // Partial overlap - estimate based on range positions
        double left_range = lmax.GetValue<double>() - lmin.GetValue<double>();
        double right_range = rmax.GetValue<double>() - rmin.GetValue<double>();
        
        if (left_range <= 0 || right_range <= 0) {
            return 0.33; // Default for single-value ranges
        }
        
        // Rough estimate: proportion of left values that could be less than right values
        double overlap_factor = (rmax.GetValue<double>() - lmin.GetValue<double>()) / 
                               (left_range + right_range);
        return std::max(0.0, std::min(1.0, overlap_factor));
    }
    
    case ExpressionType::COMPARE_GREATERTHAN:
    case ExpressionType::COMPARE_GREATERTHANOREQUALTO: {
        // LEFT > RIGHT or LEFT >= RIGHT
        if (lmin >= rmax) {
            return 1.0; // All left values are greater than all right values
        }
        if (lmax <= rmin) {
            return 0.0; // No left values are greater than right values
        }
        
        // Similar logic to less-than, but reversed
        double left_range = lmax.GetValue<double>() - lmin.GetValue<double>();
        double right_range = rmax.GetValue<double>() - rmin.GetValue<double>();
        
        if (left_range <= 0 || right_range <= 0) {
            return 0.33; // Default for single-value ranges
        }
        
        double overlap_factor = (lmax.GetValue<double>() - rmin.GetValue<double>()) / 
                               (left_range + right_range);
        return std::max(0.0, std::min(1.0, overlap_factor));
    }
    
    default:
        return GetDefaultSelectivity(comparison_type);
    }
}

double StatisticsPropagator::EstimateJoinSelectivity(LogicalJoin &join) {
	if (join.type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
        return 0.1;
    }
	auto &comp_join = join.Cast<LogicalComparisonJoin>();
    double overall_selectivity = 1.0;
	for (auto &condition : comp_join.conditions) {
		double selectivity = 0.1;
		if (condition.left->type == ExpressionType::VALUE_CONSTANT && condition.right->type == ExpressionType::BOUND_COLUMN_REF) {
			auto &right_column_ref = condition.right->Cast<BoundColumnRefExpression>();
			auto right_stats = statistics_map.find(right_column_ref.binding);
			if (right_stats != statistics_map.end()) {
				auto &left_constant = condition.left->Cast<BoundConstantExpression>();
				selectivity = EstimateConstantComparisonSelectivity(*right_stats->second, condition.comparison, left_constant.value);
			}
		} else if (condition.right->type == ExpressionType::VALUE_CONSTANT && condition.left->type == ExpressionType::BOUND_COLUMN_REF) {
			auto &left_column_ref = condition.left->Cast<BoundColumnRefExpression>();
			auto left_stats = statistics_map.find(left_column_ref.binding);
			if (left_stats != statistics_map.end()) {
				auto &right_constant = condition.right->Cast<BoundConstantExpression>();
				selectivity = EstimateConstantComparisonSelectivity(*left_stats->second, condition.comparison, right_constant.value);
			}
		} else if (condition.left->type == ExpressionType::BOUND_COLUMN_REF && condition.right->type == ExpressionType::BOUND_COLUMN_REF) {
			auto &left_column_ref = condition.left->Cast<BoundColumnRefExpression>();
			auto &right_column_ref = condition.right->Cast<BoundColumnRefExpression>();
			auto left_stats = statistics_map.find(left_column_ref.binding);
			auto right_stats = statistics_map.find(right_column_ref.binding);
			if (left_stats != statistics_map.end() && right_stats != statistics_map.end()) {
				selectivity = EstimateColumnComparisonSelectivity(*left_stats->second, *right_stats->second, condition.comparison);
			}
		}
		overall_selectivity *= selectivity;
    }
	return overall_selectivity;
}

unique_ptr<NodeStatistics> StatisticsPropagator::PropagateStatistics(LogicalJoin &join,
                                                                     unique_ptr<LogicalOperator> *node_ptr) {
	// first propagate through the children of the join
	auto left_stats = PropagateStatistics(join.children[0]);
	auto right_stats = PropagateStatistics(join.children[1]);

	if (!left_stats) {
    	std::cout << "Left child statistics missing for join type: " << std::endl;
	}
	if (!right_stats) {
    	std::cout << "Right child statistics missing for join type: " << std::endl;
	}
	if (left_stats && !left_stats->has_estimated_cardinality) {
    	std::cout << "Left child has stats but no estimated cardinality" << std::endl;
	}
	if (right_stats && !right_stats->has_estimated_cardinality) {
    	std::cout << "Right child has stats but no estimated cardinality" << std::endl;
	}

	if (left_stats && left_stats->has_estimated_cardinality && !left_stats->has_max_cardinality) {
		left_stats->max_cardinality = left_stats->estimated_cardinality;
		left_stats->has_max_cardinality = true;
	}
	if (right_stats && right_stats->has_estimated_cardinality && !right_stats->has_max_cardinality) {
		right_stats->max_cardinality = right_stats->estimated_cardinality;
		right_stats->has_max_cardinality = true;
	}

	if (left_stats && right_stats) {
		MultiplyCardinalities(left_stats, *right_stats);
	}

	auto join_type = join.join_type;
	// depending on the join type, we might need to alter the statistics
	// LEFT, FULL, RIGHT OUTER and SINGLE joins can introduce null values
	// this requires us to alter the statistics after this point in the query plan
	bool adds_null_on_left = IsRightOuterJoin(join_type);
	bool adds_null_on_right = IsLeftOuterJoin(join_type) || join_type == JoinType::SINGLE;

	vector<ColumnBinding> left_bindings, right_bindings;
	if (adds_null_on_left) {
		left_bindings = join.children[0]->GetColumnBindings();
	}
	if (adds_null_on_right) {
		right_bindings = join.children[1]->GetColumnBindings();
	}

	// then propagate into the join conditions
	switch (join.type) {
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
	case LogicalOperatorType::LOGICAL_DELIM_JOIN:
	case LogicalOperatorType::LOGICAL_ASOF_JOIN:
		PropagateStatistics(join.Cast<LogicalComparisonJoin>(), node_ptr);
		break;
	case LogicalOperatorType::LOGICAL_ANY_JOIN:
		PropagateStatistics(join.Cast<LogicalAnyJoin>(), node_ptr);
		break;
	default:
		break;
	}

	if (adds_null_on_right) {
		// left or full outer join: set IsNull() to true for all rhs statistics
		for (auto &binding : right_bindings) {
			auto stats = statistics_map.find(binding);
			if (stats != statistics_map.end()) {
				stats->second->Set(StatsInfo::CAN_HAVE_NULL_VALUES);
			}
		}
	}
	if (adds_null_on_left) {
		// right or full outer join: set IsNull() to true for all lhs statistics
		for (auto &binding : left_bindings) {
			auto stats = statistics_map.find(binding);
			if (stats != statistics_map.end()) {
				stats->second->Set(StatsInfo::CAN_HAVE_NULL_VALUES);
			}
		}
	}

	auto result_stats = make_uniq<NodeStatistics>();
	if (left_stats && right_stats && left_stats->has_estimated_cardinality && right_stats->has_estimated_cardinality) {
		switch (join.type) {
    	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
    	case LogicalOperatorType::LOGICAL_DELIM_JOIN:
    	case LogicalOperatorType::LOGICAL_ASOF_JOIN: {
			double join_selectivity = EstimateJoinSelectivity(join);
			idx_t base_cardinality = left_stats->estimated_cardinality * right_stats->estimated_cardinality;
			result_stats->estimated_cardinality = std::max((idx_t)(base_cardinality * join_selectivity), (idx_t)1);
            result_stats->has_estimated_cardinality = true;
			result_stats->has_max_cardinality = true;
			result_stats->max_cardinality = result_stats->estimated_cardinality;
		
			join.estimated_cardinality = result_stats->estimated_cardinality;
            join.has_estimated_cardinality = true;
			break;
		}
		default: {
			join.estimated_cardinality = left_stats->estimated_cardinality * right_stats->estimated_cardinality;
			result_stats = std::move(left_stats);
            break;
		}
		}
	} else {
		std::cout << "Join estimate error" << std::endl;
	}
	return std::move(result_stats);
}

unique_ptr<NodeStatistics> StatisticsPropagator::PropagateStatistics(LogicalPositionalJoin &join,
                                                                     unique_ptr<LogicalOperator> *node_ptr) {
	D_ASSERT(join.type == LogicalOperatorType::LOGICAL_POSITIONAL_JOIN);

	// first propagate through the children of the join
	node_stats = PropagateStatistics(join.children[0]);
	for (idx_t child_idx = 1; child_idx < join.children.size(); child_idx++) {
		auto child_stats = PropagateStatistics(join.children[child_idx]);
		if (!child_stats) {
			node_stats = nullptr;
		} else if (node_stats) {
			if (!node_stats->has_estimated_cardinality || !child_stats->has_estimated_cardinality ||
			    !node_stats->has_max_cardinality || !child_stats->has_max_cardinality) {
				node_stats = nullptr;
			} else {
				MaxCardinalities(node_stats, *child_stats);
			}
		}
	}

	// No conditions.

	// Positional Joins are always FULL OUTER

	// set IsNull() to true for all lhs statistics
	auto left_bindings = join.children[0]->GetColumnBindings();
	for (auto &binding : left_bindings) {
		auto stats = statistics_map.find(binding);
		if (stats != statistics_map.end()) {
			stats->second->Set(StatsInfo::CAN_HAVE_NULL_VALUES);
		}
	}

	// set IsNull() to true for all rhs statistics
	auto right_bindings = join.children[1]->GetColumnBindings();
	for (auto &binding : right_bindings) {
		auto stats = statistics_map.find(binding);
		if (stats != statistics_map.end()) {
			stats->second->Set(StatsInfo::CAN_HAVE_NULL_VALUES);
		}
	}

	return std::move(node_stats);
}

void StatisticsPropagator::CreateFilterFromJoinStats(unique_ptr<LogicalOperator> &child, unique_ptr<Expression> &expr,
                                                     const BaseStatistics &stats_before,
                                                     const BaseStatistics &stats_after) {
	// Only do this for integral colref's that have stats
	if (expr->type != ExpressionType::BOUND_COLUMN_REF || !expr->return_type.IsIntegral() ||
	    !NumericStats::HasMinMax(stats_before) || !NumericStats::HasMinMax(stats_after)) {
		return;
	}

	// Retrieve min/max
	auto min_before = NumericStats::Min(stats_before);
	auto max_before = NumericStats::Max(stats_before);
	auto min_after = NumericStats::Min(stats_after);
	auto max_after = NumericStats::Max(stats_after);

	vector<unique_ptr<Expression>> filter_exprs;
	if (min_after > min_before) {
		filter_exprs.emplace_back(
		    make_uniq<BoundComparisonExpression>(ExpressionType::COMPARE_GREATERTHANOREQUALTO, expr->Copy(),
		                                         make_uniq<BoundConstantExpression>(std::move(min_after))));
	}
	if (max_after < max_before) {
		filter_exprs.emplace_back(
		    make_uniq<BoundComparisonExpression>(ExpressionType::COMPARE_LESSTHANOREQUALTO, expr->Copy(),
		                                         make_uniq<BoundConstantExpression>(std::move(max_after))));
	}

	if (filter_exprs.empty()) {
		return;
	}

	auto filter = make_uniq<LogicalFilter>();
	filter->children.emplace_back(std::move(child));
	child = std::move(filter);

	for (auto &filter_expr : filter_exprs) {
		child->expressions.emplace_back(std::move(filter_expr));
	}

	FilterPushdown filter_pushdown(optimizer);
	child = filter_pushdown.Rewrite(std::move(child));
	PropagateExpression(expr);
}

} // namespace duckdb
