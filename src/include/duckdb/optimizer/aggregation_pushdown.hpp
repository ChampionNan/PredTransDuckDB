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

// Add this to the AggregationPushdown class in aggregation_pushdown.hpp
enum class QueryType {
    SELECT_STAR,         // SELECT * FROM 
    SELECT_DISTINCT,      // SELECT DISTINCT a FROM 
    COUNT_STAR,         // SELECT COUNT(*) FROM (no GROUP BY)
    MINMAX_AGGREGATE,   // SELECT MIN(a), MAX(b) FROM (no GROUP BY)
    OTHER               // Any other query pattern
};

class AggregationPushdown {

    using AggOptFunc = unique_ptr<LogicalOperator> (AggregationPushdown::*)(unique_ptr<LogicalOperator>);

public:
    // Add this to your class declaration in aggregation_pushdown.hpp
    struct MinMaxColumnInfo {
        ColumnBinding original_binding; // Original column binding
        ColumnBinding binding;      // Current column binding
        string function_name;       // "min" or "max"
        
        string ToString() const {
            return function_name + "[" + original_binding.ToString() + "] -> " + binding.ToString();
        }
    };

    struct JoinInfo {
        int join_id;  // Sequential ID
        bool left_pushdown;
        bool right_pushdown;
    };


public:
    explicit AggregationPushdown(Binder &binder, ClientContext &context, QueryType query_type) : binder(binder), context(context), query_type(query_type) {
        global_binding_map.clear();
    }

    unique_ptr<LogicalOperator> Rewrite(unique_ptr<LogicalOperator> op);

    unique_ptr<LogicalOperator> ApplyAgg(unique_ptr<LogicalOperator> op);

    unique_ptr<LogicalOperator> UpdateBinding(unique_ptr<LogicalOperator> op);

    void StoreMinMaxAggregates(LogicalOperator* op);

    unique_ptr<LogicalOperator> ReplaceRootCountWithSum(unique_ptr<LogicalOperator> op_node);

    void UpdateMinMax();

    void UpdateBindingMapOnce(const ColumnBinding old_binding, const ColumnBinding new_binding);

    void UpdateBindingMap(const ColumnBinding old_binding, const ColumnBinding new_binding);

    ColumnBinding GetUpdatedBindingOnce(const ColumnBinding& original);

    ColumnBinding GetUpdatedBinding(const ColumnBinding& original);

    unique_ptr<LogicalOperator> AddAnnotAttributeDFS(unique_ptr<LogicalOperator> op_node, bool applyFlag = false);

    bool FindAllAnnotAttributes(LogicalOperator* op, vector<ColumnBinding>& annot_binding, vector<LogicalType>& annot_type);
    bool FindAnnotAttribute(LogicalOperator* op, ColumnBinding& annot_binding, LogicalType& annot_type);

    void GetAnnotColumnBindingsIdx(LogicalOperator* op, vector<idx_t>& annot_indices);

    unique_ptr<LogicalOperator> AddProjectionWithAnnot(unique_ptr<LogicalOperator> op, unique_ptr<Expression> annot_expr, string name, vector<ColumnBinding> bindings_to_exclude);
    unique_ptr<LogicalOperator> AddProjectionWithAnnot(unique_ptr<LogicalOperator> op, vector<unique_ptr<Expression>> annot_exprs, string name, vector<ColumnBinding> bindings_to_exclude);

    unique_ptr<LogicalOperator> CreateDynamicAggregate(unique_ptr<LogicalOperator> child_node);

    void UpdateJoinConditions(LogicalComparisonJoin& join);

    string GetColumnName(LogicalOperator* op, idx_t idx);

    // void ResetJoinCondition(JoinCondition &condition, JoinSide side);

    unique_ptr<LogicalOperator> PruneAggregation(unique_ptr<LogicalOperator> op, AggOptFunc func);

    bool CheckPKFK(LogicalOperator* op);

    unique_ptr<LogicalOperator> UpdateAnnotMul(unique_ptr<LogicalOperator> op_node);

    unique_ptr<LogicalOperator> PruneAggregationWithProjectionMap(unique_ptr<LogicalOperator> op);

    bool NotHasTooManyGroups(unique_ptr<LogicalOperator>& op);

    void RecordAggPushdown(unique_ptr<LogicalOperator>& op);

    unique_ptr<LogicalOperator> RemoveHeavyAggregation(unique_ptr<LogicalOperator> op);

private:
    Binder &binder;
    ClientContext &context;
    QueryType query_type;

    std::unordered_map<ColumnBinding, ColumnBinding, ColumnBindingHashFunction> global_binding_map;
    static vector<MinMaxColumnInfo> minmax_columns;  // Store MIN/MAX column info
    static vector<JoinInfo> join_pushdown_info;
};

} // namespace duckdb
