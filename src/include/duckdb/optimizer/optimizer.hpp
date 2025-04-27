//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/optimizer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/optimizer/expression_rewriter.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/common/enums/optimizer_type.hpp"

#include <functional>

namespace duckdb {
class Binder;

// Add this to the AggregationPushdown class in aggregation_pushdown.hpp
enum class QueryType {
    SELECT_STAR,         // SELECT * FROM 
    SELECT_DISTINCT,      // SELECT DISTINCT a FROM 
    COUNT_STAR,         // SELECT COUNT(*) FROM (no GROUP BY)
    MINMAX_AGGREGATE,   // SELECT MIN(a), MAX(b) FROM (no GROUP BY)
    OTHER               // Any other query pattern
};

class Optimizer {
public:
	Optimizer(Binder &binder, ClientContext &context);

	//! Optimize a plan by running specialized optimizers
	unique_ptr<LogicalOperator> Optimize(unique_ptr<LogicalOperator> plan);
	//! Return a reference to the client context of this optimizer
	ClientContext &GetContext();

	QueryType DetectQueryType(LogicalOperator* op);
	void PrintOperatorBindings(LogicalOperator* op, const string& prefix = "");

	ClientContext &context;
	Binder &binder;
	ExpressionRewriter rewriter;

private:
	void RunOptimizer(OptimizerType type, const std::function<void()> &callback);
	void Verify(LogicalOperator &op);

private:
	unique_ptr<LogicalOperator> plan;
};

} // namespace duckdb
