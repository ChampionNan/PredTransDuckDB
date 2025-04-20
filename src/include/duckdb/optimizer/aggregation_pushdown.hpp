//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/aggregation_pushdown.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/unordered_set.hpp"
#include "duckdb/optimizer/rule.hpp"

namespace duckdb {

class Optimizer;
class Binder;

class AggregationPushdown {
public:
    explicit AggregationPushdown(Binder &binder, ClientContext &context) : binder(binder), context(context) {
        global_binding_map.clear();
    }

    unique_ptr<LogicalOperator> Rewrite(unique_ptr<LogicalOperator> op);

    unique_ptr<LogicalOperator> ReplaceRootCountWithSum(unique_ptr<LogicalOperator> op_node);

    ColumnBinding GetUpdatedBinding(const ColumnBinding& original);

    unique_ptr<LogicalOperator> AddAnnotAttributeDFS(unique_ptr<LogicalOperator> op_node);

    bool FindAnnotAttribute(LogicalOperator* op, ColumnBinding& annot_binding, LogicalType& annot_type);

    unique_ptr<LogicalOperator> AddProjectionWithAnnot(unique_ptr<LogicalOperator> op, unique_ptr<Expression> annot_expr, string name, vector<ColumnBinding> bindings_to_exclude);

    unique_ptr<LogicalOperator> CreateDynamicAggregate(unique_ptr<LogicalOperator> child_node);

    void UpdateJoinConditions(LogicalComparisonJoin& join);

    string GetColumnName(LogicalOperator* op, idx_t idx);

    unique_ptr<LogicalOperator> PruneAggregationColumns(unique_ptr<LogicalOperator> op);

    void PruneAggregationWithProjectionMap(LogicalOperator* op, const vector<idx_t>& projection_map);

private:
    Binder &binder;
    ClientContext &context;

    std::unordered_map<ColumnBinding, ColumnBinding, ColumnBindingHashFunction> global_binding_map;
};

} // namespace duckdb
