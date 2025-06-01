//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/join_order/plan_enumerator.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/optimizer/join_order/join_relation.hpp"
#include "duckdb/optimizer/join_order/cardinality_estimator.hpp"
#include "duckdb/optimizer/join_order/query_graph.hpp"
#include "duckdb/optimizer/join_order/join_node.hpp"
#include "duckdb/optimizer/join_order/cost_model.hpp"
#include "duckdb/parser/expression_map.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"

#include "duckdb/common/common.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/pair.hpp"
#include "duckdb/common/enums/join_type.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/planner/column_binding_map.hpp"

#include <functional>

namespace duckdb {

class QueryGraphManager;

struct ColumnBindingHash {
    std::size_t operator()(const ColumnBinding& binding) const {
        // Hash the table_index and column_index directly
        std::size_t h1 = std::hash<idx_t>{}(binding.table_index);
        std::size_t h2 = std::hash<idx_t>{}(binding.column_index);
        // Combine the hashes - a simple but effective approach
        return h1 ^ (h2 << 1);
    }
};

// Relational hypergraph for GYO algorithm
struct RelationalHypergraph {
	// Maps column bindings to unique vertex IDs
	column_binding_map_t<idx_t> column_to_vertex;
	// Maps vertex IDs back to column bindings
	vector<ColumnBinding> vertex_to_column;
	// Each relation (hyperedge) is a set of vertices
	vector<unordered_set<idx_t>> relations;
	// Original relation index for each hyperedge
	vector<idx_t> relation_indices;
};

class PlanEnumerator {
public:
	explicit PlanEnumerator(QueryGraphManager &query_graph_manager, CostModel &cost_model,
	                        const QueryGraphEdges &query_graph)
	    : query_graph(query_graph), query_graph_manager(query_graph_manager), cost_model(cost_model),
	      full_plan_found(false), must_update_full_plan(false) {
	}

	//! Perform the join order solving
	unique_ptr<JoinNode> SolveJoinOrder();
	unique_ptr<JoinNode> SolveJoinOrderLeftDeep();
	unique_ptr<JoinNode> SolveJoinOrderRandom();
	unique_ptr<JoinNode> SolveJoinOrderLeftDeepRandom();
	unique_ptr<JoinNode> SolveJoinOrderFixed(vector<LogicalOperator*> &exec_order);
	void InitLeafPlans();

	static unique_ptr<LogicalOperator> BuildSideProbeSideSwaps(unique_ptr<LogicalOperator> plan);

private:
	QueryGraphEdges const &query_graph;
	//! The total amount of join pairs that have been considered
	idx_t pairs = 0;
	//! The set of edges used in the join optimizer
	QueryGraphManager &query_graph_manager;
	//! Cost model to evaluate cost of joins
	CostModel &cost_model;
	//! A map to store the optimal join plan found for a specific JoinRelationSet*
	reference_map_t<JoinRelationSet, unique_ptr<JoinNode>> plans;

	bool full_plan_found;
	bool must_update_full_plan;
	unordered_set<string> join_nodes_in_full_plan;

	unique_ptr<JoinNode> CreateJoinTree(JoinRelationSet &set,
	                                    const vector<reference<NeighborInfo>> &possible_connections, JoinNode &left,
	                                    JoinNode &right);

	//! Emit a pair as a potential join candidate. Returns the best plan found for the (left, right) connection (either
	//! the newly created plan, or an existing plan)
	JoinNode &EmitPair(JoinRelationSet &left, JoinRelationSet &right, const vector<reference<NeighborInfo>> &info);
	//! Tries to emit a potential join candidate pair. Returns false if too many pairs have already been emitted,
	//! cancelling the dynamic programming step.
	bool TryEmitPair(JoinRelationSet &left, JoinRelationSet &right, const vector<reference<NeighborInfo>> &info);

	bool EnumerateCmpRecursive(JoinRelationSet &left, JoinRelationSet &right, unordered_set<idx_t> &exclusion_set);
	//! Emit a relation set node
	bool EmitCSG(JoinRelationSet &node);
	//! Enumerate the possible connected subgraphs that can be joined together in the join graph
	bool EnumerateCSGRecursive(JoinRelationSet &node, unordered_set<idx_t> &exclusion_set);
	//! Generate cross product edges inside the side
	void GenerateCrossProducts();

	//! Solve the join order exactly using dynamic programming. Returns true if it was completed successfully (i.e. did
	//! not time-out)
	bool SolveJoinOrderExactly();
	//! Solve the join order approximately using a greedy algorithm
	void SolveJoinOrderApproximately();

	void UpdateDPTree(JoinNode &new_plan);

	void UpdateJoinNodesInFullPlan(JoinNode &node);
	bool NodeInFullPlan(JoinNode &node);

// GYO algorithm implementation
public:
    unique_ptr<JoinNode> SolveJoinOrderGYO();
private:
    // Reduction sequence for reconstructing the join tree
    struct GYOReductionStep {
        idx_t ear_relation_idx;      // Index of the relation being reduced
        idx_t witness_relation_idx;  // Index of the witness relation
    };
    // Build the relational hypergraph from DuckDB's structures
    RelationalHypergraph BuildRelationalHypergraph();
    // Check if a relation forms an ear according to GYO algorithm
    bool IsEar(RelationalHypergraph& graph, idx_t relation_idx, idx_t& witness_idx);
    vector<GYOReductionStep> gyo_reduction_sequence;
};

} // namespace duckdb
