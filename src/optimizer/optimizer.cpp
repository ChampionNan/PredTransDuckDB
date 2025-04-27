#include "duckdb/optimizer/optimizer.hpp"

#include "duckdb/execution/column_binding_resolver.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/optimizer/column_lifetime_optimizer.hpp"
#include "duckdb/optimizer/common_aggregate_optimizer.hpp"
#include "duckdb/optimizer/compressed_materialization.hpp"
#include "duckdb/optimizer/cse_optimizer.hpp"
#include "duckdb/optimizer/deliminator.hpp"
#include "duckdb/optimizer/expression_heuristics.hpp"
#include "duckdb/optimizer/filter_pullup.hpp"
#include "duckdb/optimizer/filter_pushdown.hpp"
#include "duckdb/optimizer/in_clause_rewriter.hpp"
#include "duckdb/optimizer/predicate_transfer/predicate_transfer_optimizer.hpp"
#include "duckdb/optimizer/join_order/join_order_optimizer.hpp"
#include "duckdb/optimizer/regex_range_filter.hpp"
#include "duckdb/optimizer/remove_duplicate_groups.hpp"
#include "duckdb/optimizer/remove_unused_columns.hpp"
#include "duckdb/optimizer/rule/equal_or_null_simplification.hpp"
#include "duckdb/optimizer/rule/in_clause_simplification.hpp"
#include "duckdb/optimizer/rule/list.hpp"
#include "duckdb/optimizer/statistics_propagator.hpp"
#include "duckdb/optimizer/topn_optimizer.hpp"
#include "duckdb/optimizer/unnest_rewriter.hpp"
#include "duckdb/optimizer/aggregation_pushdown.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/planner.hpp"

#include "duckdb/optimizer/predicate_transfer/setting.hpp"

namespace duckdb {


Optimizer::Optimizer(Binder &binder, ClientContext &context) : context(context), binder(binder), rewriter(context) {
	rewriter.rules.push_back(make_uniq<ConstantFoldingRule>(rewriter));
	rewriter.rules.push_back(make_uniq<DistributivityRule>(rewriter));
	rewriter.rules.push_back(make_uniq<ArithmeticSimplificationRule>(rewriter));
	rewriter.rules.push_back(make_uniq<CaseSimplificationRule>(rewriter));
	rewriter.rules.push_back(make_uniq<ConjunctionSimplificationRule>(rewriter));
	rewriter.rules.push_back(make_uniq<DatePartSimplificationRule>(rewriter));
	rewriter.rules.push_back(make_uniq<ComparisonSimplificationRule>(rewriter));
	rewriter.rules.push_back(make_uniq<InClauseSimplificationRule>(rewriter));
	rewriter.rules.push_back(make_uniq<EqualOrNullSimplification>(rewriter));
	rewriter.rules.push_back(make_uniq<MoveConstantsRule>(rewriter));
	rewriter.rules.push_back(make_uniq<LikeOptimizationRule>(rewriter));
	rewriter.rules.push_back(make_uniq<OrderedAggregateOptimizer>(rewriter));
	rewriter.rules.push_back(make_uniq<RegexOptimizationRule>(rewriter));
	rewriter.rules.push_back(make_uniq<EmptyNeedleRemovalRule>(rewriter));
	rewriter.rules.push_back(make_uniq<EnumComparisonRule>(rewriter));

#ifdef DEBUG
	for (auto &rule : rewriter.rules) {
		// root not defined in rule
		D_ASSERT(rule->root);
	}
#endif
}

ClientContext &Optimizer::GetContext() {
	return context;
}

void Optimizer::RunOptimizer(OptimizerType type, const std::function<void()> &callback) {
	auto &config = DBConfig::GetConfig(context);
	if (config.options.disabled_optimizers.find(type) != config.options.disabled_optimizers.end()) {
		// optimizer is marked as disabled: skip
		return;
	}
	auto &profiler = QueryProfiler::Get(context);
	profiler.StartPhase(OptimizerTypeToString(type));
	callback();
	profiler.EndPhase();
	if (plan) {
		Verify(*plan);
	}
}

void Optimizer::Verify(LogicalOperator &op) {
	ColumnBindingResolver::Verify(op);
}

unique_ptr<LogicalOperator> Optimizer::Optimize(unique_ptr<LogicalOperator> plan_p) {
	// std::cout << "At whole Optimize! " << std::endl;
	// auto total_start = std::chrono::high_resolution_clock::now();
	Verify(*plan_p);

	switch (plan_p->type) {
	case LogicalOperatorType::LOGICAL_TRANSACTION:
		return plan_p; // skip optimizing simple & often-occurring plans unaffected by rewrites
	default:
		break;
	}

	this->plan = std::move(plan_p);
	// first we perform expression rewrites using the ExpressionRewriter
	// this does not change the logical plan structure, but only simplifies the expression trees
	RunOptimizer(OptimizerType::EXPRESSION_REWRITER, [&]() { rewriter.VisitOperator(*plan); });

	// perform filter pullup
	RunOptimizer(OptimizerType::FILTER_PULLUP, [&]() {
		FilterPullup filter_pullup;
		plan = filter_pullup.Rewrite(std::move(plan));
	});

	// perform filter pushdown
	RunOptimizer(OptimizerType::FILTER_PUSHDOWN, [&]() {
		FilterPushdown filter_pushdown(*this);
		plan = filter_pushdown.Rewrite(std::move(plan));
	});

	RunOptimizer(OptimizerType::REGEX_RANGE, [&]() {
		RegexRangeFilter regex_opt;
		plan = regex_opt.Rewrite(std::move(plan));
	});

	// removes any redundant DelimGets/DelimJoins
	RunOptimizer(OptimizerType::DELIMINATOR, [&]() {
		Deliminator deliminator;
		plan = deliminator.Optimize(std::move(plan));
	});

	// then we perform the join ordering optimization
	// this also rewrites cross products + filters into joins and performs filter pushdowns
	// auto start2 = std::chrono::high_resolution_clock::now();
	RunOptimizer(OptimizerType::JOIN_ORDER, [&]() {
		JoinOrderOptimizer optimizer(context);
		plan = optimizer.Optimize(std::move(plan));
		std::cout << "After First Join Order Plan " << std::endl;
		plan->Print();
	});

	// NOTE: Add query type detection here
#ifdef YANPLUS
	auto query_type = DetectQueryType(plan.get());
	std::cout << "Query Type: " << static_cast<int>(query_type) << std::endl;

	if (query_type == QueryType::SELECT_STAR) {
		PredicateTransferOptimizer PT(context);
		plan = PT.PreOptimize(std::move(plan));
		auto BFOrder = PT.GetBFOrder();
		/*std::cout << "BFOrder Size: " << BFOrder.size() << std::endl;
		for (auto &node : BFOrder) {
			std::cout << "BFOrder Node: " << node->ParamsToString() << std::endl;
		}*/
		RunOptimizer(OptimizerType::JOIN_ORDER, [&]() {
			JoinOrderOptimizer optimizer2(context);
			plan = optimizer2.CallSolveJoinOrderFixed(std::move(plan), BFOrder);
			std::cout << "After Second Join Order Plan Begin " << std::endl;
			plan->Print();
		});
		plan = PT.Optimize(std::move(plan));
		std::cout << "After PT Plan " << std::endl;
		plan->Print();
		PT.PrintUseBFAndRelatedCreate(plan);
	}
#endif

	// rewrites UNNESTs in DelimJoins by moving them to the projection
	RunOptimizer(OptimizerType::UNNEST_REWRITER, [&]() {
		UnnestRewriter unnest_rewriter;
		plan = unnest_rewriter.Optimize(std::move(plan));
	});

#ifdef YANPLUS
	if (query_type == QueryType::COUNT_STAR) {
		std::cout << "Before AGGREGATION_PUSHDOWN Plan " << std::endl;
		plan->Print();
		PrintOperatorBindings(plan.get());

		RunOptimizer(OptimizerType::AGGREGATION_PUSHDOWN, [&]() {
			AggregationPushdown aggregation_pushdown(binder, context);
			plan = aggregation_pushdown.Rewrite(std::move(plan));
		});
		std::cout << "After AddAnnotAttributeDFS! Binding" << std::endl;
		plan->Print();
		PrintOperatorBindings(plan.get());
	}
#endif

	// removes unused columns
	RunOptimizer(OptimizerType::UNUSED_COLUMNS, [&]() {
		RemoveUnusedColumns unused(binder, context, true);
		unused.VisitOperator(*plan);
	});

	RunOptimizer(OptimizerType::IN_CLAUSE, [&]() {
		InClauseRewriter ic_rewriter(context, *this);
		plan = ic_rewriter.Rewrite(std::move(plan));
	});

	// Remove duplicate groups from aggregates
	RunOptimizer(OptimizerType::DUPLICATE_GROUPS, [&]() {
		RemoveDuplicateGroups remove;
		remove.VisitOperator(*plan);
	});

	// then we extract common subexpressions inside the different operators
	RunOptimizer(OptimizerType::COMMON_SUBEXPRESSIONS, [&]() {
		CommonSubExpressionOptimizer cse_optimizer(binder);
		cse_optimizer.VisitOperator(*plan);
	});

	// perform statistics propagation
	column_binding_map_t<unique_ptr<BaseStatistics>> statistics_map;
	// RunOptimizer(OptimizerType::STATISTICS_PROPAGATION, [&]() {
	// 	StatisticsPropagator propagator(*this);
	// 	propagator.PropagateStatistics(plan);
	// 	statistics_map = propagator.GetStatisticsMap();
	// });

	// creates projection maps so unused columns are projected out early
	RunOptimizer(OptimizerType::COLUMN_LIFETIME, [&]() {
		ColumnLifetimeAnalyzer column_lifetime(true);
		column_lifetime.VisitOperator(*plan);
	});

	// remove duplicate aggregates
	RunOptimizer(OptimizerType::COMMON_AGGREGATE, [&]() {
		CommonAggregateOptimizer common_aggregate;
		common_aggregate.VisitOperator(*plan);
	});

	// creates projection maps so unused columns are projected out early
	RunOptimizer(OptimizerType::COLUMN_LIFETIME, [&]() {
		ColumnLifetimeAnalyzer column_lifetime(true);
		column_lifetime.VisitOperator(*plan);
	});

	// compress data based on statistics for materializing operators
	RunOptimizer(OptimizerType::COMPRESSED_MATERIALIZATION, [&]() {
		CompressedMaterialization compressed_materialization(context, binder, std::move(statistics_map));
		compressed_materialization.Compress(plan);
	});

	// transform ORDER BY + LIMIT to TopN
	RunOptimizer(OptimizerType::TOP_N, [&]() {
		TopN topn;
		plan = topn.Optimize(std::move(plan));
	});

	// apply simple expression heuristics to get an initial reordering
	RunOptimizer(OptimizerType::REORDER_FILTER, [&]() {
		ExpressionHeuristics expression_heuristics(*this);
		plan = expression_heuristics.Rewrite(std::move(plan));
	});

	for (auto &optimizer_extension : DBConfig::GetConfig(context).optimizer_extensions) {
		RunOptimizer(OptimizerType::EXTENSION, [&]() {
			optimizer_extension.optimize_function(context, optimizer_extension.optimizer_info.get(), plan);
		});
	}

#ifdef YANPLUS
	if (query_type == QueryType::COUNT_STAR) {
		std::cout << "Before AGGREGATION_PUSHDOWN Join projection prune " << std::endl;
		plan->Print();
		PrintOperatorBindings(plan.get());
		RunOptimizer(OptimizerType::AGGREGATION_PUSHDOWN, [&]() {
			AggregationPushdown aggregation_pushdown(binder, context);
			plan = aggregation_pushdown.UpdateBinding(std::move(plan));
		});
	}
#endif

	std::cout << "After All Optimizations Plan " << std::endl;
	plan->Print();
	PrintOperatorBindings(plan.get());

	// auto total_end = std::chrono::high_resolution_clock::now();
	// std::cout << "Total Opt Time: " << std::chrono::duration_cast<std::chrono::microseconds>(total_end - total_start).count() << " µs" << std::endl;

	Planner::VerifyPlan(context, plan);

	return std::move(plan);
}


QueryType Optimizer::DetectQueryType(LogicalOperator* op) {
    if (!op) {
        return QueryType::OTHER;
    }
    
    // Case 2: SELECT COUNT(*) FROM table
    // Usually implemented as a projection over an aggregate
    if (op->type == LogicalOperatorType::LOGICAL_PROJECTION && 
        op->children.size() == 1 && 
        op->children[0]->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
        
        auto& agg = op->children[0]->Cast<LogicalAggregate>();
        
        // Check if this is a COUNT(*) aggregation (no GROUP BY, single expression)
        if (agg.groups.empty() && agg.expressions.size() == 1) {
            auto& expr = agg.expressions[0];
            
            // Verify it's a COUNT(*) expression
            if (expr->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
                auto& bound_agg = expr->Cast<BoundAggregateExpression>();
                
                // Check if this is COUNT(*) or COUNT_STAR
                if (bound_agg.function.name == "count_star" || 
                    (bound_agg.function.name == "count" && bound_agg.children.empty())) {
                    return QueryType::COUNT_STAR;
                }
            }
        }
    }
    
    // Case 3: SELECT MIN(a), MAX(b) FROM table
    // Also projection over aggregate, but with MIN/MAX functions
    if (op->type == LogicalOperatorType::LOGICAL_PROJECTION && 
        op->children.size() == 1 && 
        op->children[0]->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
        
        auto& agg = op->children[0]->Cast<LogicalAggregate>();
        
        // Check if this has no GROUP BY
        if (agg.groups.empty() && !agg.expressions.empty()) {
            bool is_minmax_aggregate = true;
            
            // Check all aggregate expressions
            for (auto& expr : agg.expressions) {
                if (expr->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
                    auto& bound_agg = expr->Cast<BoundAggregateExpression>();
                    
                    // Only MIN, MAX allowed for simple aggregate type
                    if (bound_agg.function.name != "min" && 
                        bound_agg.function.name != "max") {
                        is_minmax_aggregate = false;
                        break;
                    }
                } else {
                    is_minmax_aggregate = false;
                    break;
                }
            }
            
            if (is_minmax_aggregate) {
                return QueryType::MINMAX_AGGREGATE;
            }
        }
    }

	// Case 4: SELECT distinct a FROM 
    // Usually implemented as a projection over a scan/get
    if (op->type == LogicalOperatorType::LOGICAL_DISTINCT && 
        op->children.size() == 1 && 
        op->children[0]->type == LogicalOperatorType::LOGICAL_PROJECTION) {
        return QueryType::SELECT_DISTINCT;
    }

	// Case 1: SELECT * FROM, full query
    // Usually implemented as a projection over a scan/get
    if (op->type == LogicalOperatorType::LOGICAL_PROJECTION && 
        op->children.size() == 1 && 
        op->children[0]->type != LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY){ 
        return QueryType::SELECT_STAR;
    }
    
    // Any other query pattern
    return QueryType::OTHER;
}


void Optimizer::PrintOperatorBindings(LogicalOperator* op, const string& prefix) {
    if (!op) return;
    
    // First collect all base tables and column_ids mappings from the operator tree
    std::unordered_map<idx_t, std::tuple<string, vector<string>, vector<column_t>>> table_map;
    std::function<void(const LogicalOperator*)> collect_tables = [&](const LogicalOperator* node) {
        if (!node) return;
        
        if (node->type == LogicalOperatorType::LOGICAL_GET) {
            auto& get = (const LogicalGet&)(*node);
            table_map[get.table_index] = {get.function.to_string(get.bind_data.get()), get.names, get.column_ids};
        }
        
        for (auto& child : node->children) {
            collect_tables(child.get());
        }
    };
    
    collect_tables(op);
    std::cout << "=================================================================" << std::endl;
    // Print operator type
    std::cout << prefix << "Operator: " << LogicalOperatorToString(op->type) << std::endl;
    
    // Print detailed information for specific operator types
    if (op->type == LogicalOperatorType::LOGICAL_PROJECTION) {
        auto& proj = op->Cast<LogicalProjection>();
        std::cout << prefix << "Projection Expressions: " << std::endl;
        for (idx_t i = 0; i < proj.expressions.size(); i++) {
            auto& expr = proj.expressions[i];
            std::cout << prefix << "  [" << i << "] " << expr->ToString() << " (type: " << expr->return_type.ToString() << ")";
            if (expr->GetName() == "annot") {
                std::cout << " [ANNOT COLUMN]";
            }
            std::cout << std::endl;
            
            // Print more details for function expressions
            if (expr->type == ExpressionType::BOUND_FUNCTION) {
                auto& func_expr = expr->Cast<BoundFunctionExpression>();
                std::cout << prefix << "    Function: " << func_expr.function.name << std::endl;
                std::cout << prefix << "    Children: " << func_expr.children.size() << std::endl;
                
                for (idx_t j = 0; j < func_expr.children.size(); j++) {
                    auto& child = func_expr.children[j];
                    std::cout << prefix << "      [" << j << "] " << child->ToString();
                    
                    if (child->type == ExpressionType::BOUND_COLUMN_REF) {
                        auto& col_ref = child->Cast<BoundColumnRefExpression>();
                        std::cout << " (binding: " << col_ref.binding.table_index 
                                  << "." << col_ref.binding.column_index << ")";
                    }
                    std::cout << std::endl;
				}
        	} else if (expr->type == ExpressionType::BOUND_COLUMN_REF) {
				auto& col_ref = expr->Cast<BoundColumnRefExpression>();
				std::cout << " (binding: " << col_ref.binding.table_index 
					  << "." << col_ref.binding.column_index << ")";
			} else if (expr->type == ExpressionType::CAST) {
				auto& cast_expr = expr->Cast<BoundCastExpression>();
				std::cout << " (cast type: " << cast_expr.return_type.ToString() << ")";
			}
			std::cout << std::endl;
        	std::cout << prefix << "Projection Table Index: " << proj.table_index << std::endl;
		}
    }
    else if (op->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
        auto& agg = op->Cast<LogicalAggregate>();
        
        std::cout << prefix << "Group Index: " << agg.group_index << std::endl;
        std::cout << prefix << "Aggregate Index: " << agg.aggregate_index << std::endl;
        
        // Print group expressions
        std::cout << prefix << "Group Expressions: " << std::endl;
        for (idx_t i = 0; i < agg.groups.size(); i++) {
            auto& expr = agg.groups[i];
            std::cout << prefix << "  [" << i << "] " << expr->ToString() << " (type: " << expr->return_type.ToString() << ")";
            
            if (expr->type == ExpressionType::BOUND_COLUMN_REF) {
                auto& col_ref = expr->Cast<BoundColumnRefExpression>();
                std::cout << " (binding: " << col_ref.binding.table_index 
                          << "." << col_ref.binding.column_index << ")";
            }
            std::cout << std::endl;
        }
        
        // Print aggregate expressions
        std::cout << prefix << "Aggregate Expressions: " << std::endl;
        for (idx_t i = 0; i < agg.expressions.size(); i++) {
            auto& expr = agg.expressions[i];
            std::cout << prefix << "  [" << i << "] " << expr->ToString() << " (type: " << expr->return_type.ToString() << ")";
            if (expr->GetName() == "annot") {
                std::cout << " [ANNOT COLUMN]";
            }
            std::cout << std::endl;
            
            if (expr->type == ExpressionType::BOUND_AGGREGATE) {
                auto& agg_expr = expr->Cast<BoundAggregateExpression>();
                std::cout << prefix << "    Aggregate Function: " << agg_expr.function.name << std::endl;
                std::cout << prefix << "    Children: " << agg_expr.children.size() << std::endl;
                
                for (idx_t j = 0; j < agg_expr.children.size(); j++) {
                    auto& child = agg_expr.children[j];
                    std::cout << prefix << "      [" << j << "] " << child->ToString();
                    
                    if (child->type == ExpressionType::BOUND_COLUMN_REF) {
                        auto& col_ref = child->Cast<BoundColumnRefExpression>();
                        std::cout << " (binding: " << col_ref.binding.table_index 
                                  << "." << col_ref.binding.column_index << ")";
                    }
                    std::cout << std::endl;
                }
            }
        }
    }
    // If this is a join, print join conditions
    else if (op->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
        auto& join = op->Cast<LogicalComparisonJoin>();
        // Print left projection map
        std::cout << prefix << "Left Projection Map: ";
        if (!join.left_projection_map.empty()) {
            std::cout << "[";
            for (idx_t i = 0; i < join.left_projection_map.size(); i++) {
                if (i > 0) std::cout << ", ";
                std::cout << join.left_projection_map[i];
            }
            std::cout << "]" << std::endl;
        } else {
            std::cout << "empty (all columns preserved)" << std::endl;
        }
    
        // Print right projection map
        std::cout << prefix << "Right Projection Map: ";
        if (!join.right_projection_map.empty()) {
            std::cout << "[";
            for (idx_t i = 0; i < join.right_projection_map.size(); i++) {
                if (i > 0) std::cout << ", ";
                std::cout << join.right_projection_map[i];
            }
            std::cout << "]" << std::endl;
        } else {
            std::cout << "empty (all columns preserved)" << std::endl;
        }
        std::cout << prefix << "Join Conditions:" << std::endl;
        for (auto& condition : join.conditions) {
            std::cout << prefix << "  - Left: " << condition.left->ToString() << std::endl;
            
            if (condition.left->type == ExpressionType::BOUND_COLUMN_REF) {
                auto& left_col = condition.left->Cast<BoundColumnRefExpression>();
                std::cout << prefix << "    Left binding: [" << left_col.binding.table_index 
                          << "." << left_col.binding.column_index << "]" << std::endl;
            }
            
            std::cout << prefix << "    Right: " << condition.right->ToString() << std::endl;
            
            if (condition.right->type == ExpressionType::BOUND_COLUMN_REF) {
                auto& right_col = condition.right->Cast<BoundColumnRefExpression>();
                std::cout << prefix << "    Right binding: [" << right_col.binding.table_index 
                          << "." << right_col.binding.column_index << "]" << std::endl;
            }
            
            std::cout << prefix << "    Comparison: " << EnumUtil::ToChars(condition.comparison) << std::endl;
        }
    }
    
    // Print column bindings with table info
    auto bindings = op->GetColumnBindings();
    std::cout << prefix << "Column Bindings: " << std::endl;
    for (size_t i = 0; i < bindings.size(); i++) {
        auto& binding = bindings[i];
        std::cout << prefix << "    [" << i << "] " << binding.table_index << "." 
                  << binding.column_index;
        
        // If this is a table we know about
        if (table_map.find(binding.table_index) != table_map.end()) {
            auto& [table_name, column_names, column_ids] = table_map[binding.table_index];
            std::cout << " (Table: " << table_name;
            
            // For LogicalGet operators, use column_ids to get the actual column
            if (!column_ids.empty() && binding.column_index < column_ids.size()) {
                // Map binding.column_index to actual column ID
                idx_t actual_col_id = column_ids[binding.column_index];
                
                if (actual_col_id < column_names.size()) {
                    std::cout << ", Column: " << column_names[actual_col_id] 
                              << ", binding.column_index=" << binding.column_index 
                              << " maps to actual column_id=" << actual_col_id;
                }
            } 
            // Fall back to direct binding if not a LogicalGet mapping
            else if (binding.column_index < column_names.size()) {
                std::cout << ", Column: " << column_names[binding.column_index] 
                          << " (direct binding)";
            }
            std::cout << ")";
        } else {
            // This might be a derived table (projection, aggregation, etc.)
            std::cout << " (Derived column)";
        }
        
        std::cout << std::endl;
    }
    std::cout << "=================================================================" << std::endl;
    
    // Print children recursively
    for (size_t i = 0; i < op->children.size(); i++) {
        std::cout << prefix << "Child " << i << ":" << std::endl;
        PrintOperatorBindings(op->children[i].get(), prefix + "  ");
    }
}

} // namespace duckdb
