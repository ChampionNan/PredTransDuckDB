#include "duckdb/optimizer/join_order/join_order_optimizer.hpp"
#include "duckdb/optimizer/join_order/cost_model.hpp"
#include "duckdb/optimizer/join_order/plan_enumerator.hpp"
#include "duckdb/common/enums/join_type.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/pair.hpp"
#include "duckdb/planner/expression/list.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/list.hpp"

#include "duckdb/optimizer/predicate_transfer/setting.hpp"

namespace duckdb {

static bool HasJoin(LogicalOperator *op) {
	while (!op->children.empty()) {
		if (op->children.size() == 1) {
			op = op->children[0].get();
		}
		if (op->children.size() == 2) {
			return true;
		}
	}
	return false;
}

unique_ptr<LogicalOperator> JoinOrderOptimizer::OptimizeInitial(unique_ptr<LogicalOperator> plan) {
    // make sure query graph manager has not extracted a relation graph already
	std::cout << "At JoinOrderOptimizer OptimizeInitial!" << std::endl;
    LogicalOperator *op = plan.get();

    // extract the relations that go into the hyper graph.
    bool reorderable = query_graph_manager.Build(*op);

    auto relation_stats = query_graph_manager.relation_manager.GetRelationStats();
    unique_ptr<LogicalOperator> new_logical_plan = nullptr;

    if (reorderable) {
        // Always create and keep the instances for later reuse in OptimizeInitial
        keep_cost_model = make_uniq<CostModel>(query_graph_manager);
        keep_plan_enumerator = make_uniq<PlanEnumerator>(query_graph_manager, *keep_cost_model, query_graph_manager.GetQueryGraphEdges());
    
        keep_plan_enumerator->InitLeafPlans();

#ifdef ExactLeftDeep
        auto final_plan = keep_plan_enumerator->SolveJoinOrderLeftDeep();
#elif defined(RandomBushy)
        auto final_plan = keep_plan_enumerator->SolveJoinOrderRandom();
#elif defined(RandomLeftDeep)
        auto final_plan = keep_plan_enumerator->SolveJoinOrderLeftDeepRandom();
#else
        auto final_plan = keep_plan_enumerator->SolveJoinOrder();
#endif
        new_logical_plan = query_graph_manager.Reconstruct(std::move(plan), *final_plan);
    } else {
        new_logical_plan = std::move(plan);
        if (relation_stats.size() == 1) {
            new_logical_plan->estimated_cardinality = relation_stats.at(0).cardinality;
            new_logical_plan->has_estimated_cardinality = true;
        }
    }

    if (HasJoin(new_logical_plan.get())) {
        new_logical_plan = query_graph_manager.LeftRightOptimizations(std::move(new_logical_plan));
    }

    return new_logical_plan;
}

unique_ptr<LogicalOperator> JoinOrderOptimizer::Optimize(unique_ptr<LogicalOperator> plan, optional_ptr<RelationStats> stats) {

	// make sure query graph manager has not extracted a relation graph already
	LogicalOperator *op = plan.get();

	// extract the relations that go into the hyper graph.
	// We optimize the children of any non-reorderable operations we come across.
	bool reorderable = query_graph_manager.Build(*op);

	// std::cout << "Print Relations in JoinOrderOptimizer::Optimize: " << std::endl;
	// query_graph_manager.relation_manager.PrintRelations();
	

	// get relation_stats here since the reconstruction process will move all of the relations.
	auto relation_stats = query_graph_manager.relation_manager.GetRelationStats();
	unique_ptr<LogicalOperator> new_logical_plan = nullptr;

	// Debug: Print all edges in query graph
    // std::cout << "Dumping all query graph edges in Optimize:" << std::endl;
    // const auto &query_graph = query_graph_manager.GetQueryGraphEdges();
    // std::cout << query_graph.ToString() << std::endl; // Use ToString() which is const-qualified
	if (reorderable) {
		// query graph now has filters and relations
		auto cost_model = CostModel(query_graph_manager);

		// Initialize a plan enumerator.
		auto plan_enumerator =
			PlanEnumerator(query_graph_manager, cost_model, query_graph_manager.GetQueryGraphEdges());
	
		// Initialize the leaf/single node plans
		plan_enumerator.InitLeafPlans();
	
		// Ask the plan enumerator to enumerate a number of join orders
#ifdef ExactLeftDeep
		auto final_plan = plan_enumerator.SolveJoinOrderLeftDeep();
#elif defined(RandomBushy)
		auto final_plan = plan_enumerator.SolveJoinOrderRandom();
#elif defined(RandomLeftDeep)
		auto final_plan = plan_enumerator.SolveJoinOrderLeftDeepRandom();
#else
		auto final_plan = plan_enumerator.SolveJoinOrder();
#endif
		// TODO: add in the check that if no plan exists, you have to add a cross product.
	
		// now reconstruct a logical plan from the query graph plan
		new_logical_plan = query_graph_manager.Reconstruct(std::move(plan), *final_plan);
	} else {
		new_logical_plan = std::move(plan);
		if (relation_stats.size() == 1) {
			new_logical_plan->estimated_cardinality = relation_stats.at(0).cardinality;
			new_logical_plan->has_estimated_cardinality = true;
		}
	}

	// only perform left right optimizations when stats is null (means we have the top level optimize call)
	// Don't check reorderability because non-reorderable joins will result in 1 relation, but we can
	// still switch the children.
	if (stats == nullptr && HasJoin(new_logical_plan.get())) {
		new_logical_plan = query_graph_manager.LeftRightOptimizations(std::move(new_logical_plan));
	}

	// Propagate up a stats object from the top of the new_logical_plan if stats exist.
	if (stats) {
		auto cardinality = new_logical_plan->EstimateCardinality(context);
		auto bindings = new_logical_plan->GetColumnBindings();
		auto new_stats = RelationStatisticsHelper::CombineStatsOfReorderableOperator(bindings, relation_stats);
		new_stats.cardinality = cardinality;
		RelationStatisticsHelper::CopyRelationStats(*stats, new_stats);
	}

	return new_logical_plan;
}

// Use for two case: 1. GYO 2. Arrange the plan as we have the exec_order for select * query
unique_ptr<LogicalOperator> JoinOrderOptimizer::CallSolveJoinOrderFixed(unique_ptr<LogicalOperator> plan, vector<LogicalOperator*> &exec_order) {
	// Store a clean copy before any modifications
    auto plan_backup = plan->Copy(context);
	
	// make sure query graph manager has not extracted a relation graph already
	LogicalOperator *op = plan.get();
	// extract the relations that go into the hyper graph.
	// We optimize the children of any non-reorderable operations we come across.
	bool reorderable = query_graph_manager.Build(*op, false);
	// get relation_stats here since the reconstruction process will move all of the relations.
	auto relation_stats = query_graph_manager.relation_manager.GetRelationStats();
	unique_ptr<LogicalOperator> new_logical_plan = nullptr;

#ifdef PLAN_DEBUG
	// std::cout << "Print Relations in JoinOrderOptimizer::CallSolveJoinOrderFixed: " << std::endl;
	// query_graph_manager.relation_manager.PrintRelations();
	// Debug: Print all edges in query graph
    // std::cout << "Dumping all query graph edges in CallSolveJoinOrderFixed:" << std::endl;
    // const auto &query_graph = query_graph_manager.GetQueryGraphEdges();
    // std::cout << query_graph.ToString() << std::endl; // Use ToString() which is const-qualified
	// std::cout << "Relations in CallSolveJoinOrderFixed: " << std::endl;
	// for (idx_t i = 0; i < query_graph_manager.relation_manager.NumRelations(); i++) {
	// 	auto &relation = query_graph_manager.set_manager.GetJoinRelation(i);
	//	std::cout << relation.ToString() << std::endl;
	// }
#endif

	// NOTE: Fallback to DuckDB plan when #tables >= 9
	if (query_graph_manager.relation_manager.NumRelations() >= 9) {
		GYO = false;
	}

	if (!exec_order.empty() || GYO) {
		// query graph now has filters and relations
		auto cost_model = CostModel(query_graph_manager);
		// Initialize a plan enumerator.
		auto plan_enumerator = PlanEnumerator(query_graph_manager, cost_model, query_graph_manager.GetQueryGraphEdges());

		if (!exec_order.empty()) {
			plan_enumerator.InitLeafPlans();
			unique_ptr<JoinNode> final_plan;
			final_plan = plan_enumerator.SolveJoinOrderFixed(exec_order);
			new_logical_plan = query_graph_manager.Reconstruct(std::move(plan), *final_plan);
		} else {
			plan_enumerator.root_op = op;
			auto gyo_join_tree = plan_enumerator.SolveJoinOrderGYO();
			if (gyo_join_tree) {
				new_logical_plan = query_graph_manager.Reconstruct(std::move(plan), *gyo_join_tree);
				std::cout << "GYO join tree found and reconstructed! " << std::endl;
			} else {
				// Unable to handle with GYO
				std::cout << "GYO join tree not found! " << std::endl;
				GYO = false;
			}
		}
	}

	if (!GYO || !new_logical_plan){
		std::cout << "CallSolveJoinOrderFixed Failed and fall back to DuckDB implementation! " << std::endl;
		// Create a completely fresh optimizer with clean state
        JoinOrderOptimizer fallback_optimizer(context);  // false = no GYO for fallback
        return fallback_optimizer.Optimize(std::move(plan_backup));
	}

	// only perform left right optimizations when stats is null (means we have the top level optimize call)
	// Don't check reorderability because non-reorderable joins will result in 1 relation, but we can
	// still switch the children.
	if (HasJoin(new_logical_plan.get())) {
		new_logical_plan = query_graph_manager.LeftRightOptimizations(std::move(new_logical_plan));
	}

	return new_logical_plan;
}

} // namespace duckdb
