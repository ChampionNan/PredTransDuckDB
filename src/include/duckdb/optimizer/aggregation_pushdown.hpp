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
    SUM,              // SELECT SUM(a) FROM
    OTHER               // Any other query pattern
};

class AggregationPushdown {

    using AggOptFunc = unique_ptr<LogicalOperator> (AggregationPushdown::*)(unique_ptr<LogicalOperator>);

public:
    // Add this to your class declaration in aggregation_pushdown.hpp
    struct AggColumnInfo {
        ColumnBinding original_binding; // Original column binding
        ColumnBinding binding;          // Current column binding
        string function_name;           // "min" or "max", for min/max aggregates only
        
        string ToString() const {
            return function_name + "[" + original_binding.ToString() + "] -> " + binding.ToString();
        }
    };

    struct SumAggInfo {
        string expression_string;                   // Store original form, like a * b + c
        vector<AggColumnInfo> involved_columns;     // a, b, c related binding info
        ColumnBinding result_binding;               // Binding for the SUM aggregate
        unique_ptr<Expression> expression_tree;     // The actual expression for the SUM
        LogicalType result_type;                    // Type of the result
        string alias;                               // Alias for the SUM aggregate    

        // Constructor
        SumAggInfo() = default;
        
        // Copy constructor (needed because of unique_ptr)
        SumAggInfo(const SumAggInfo& other) 
            : expression_string(other.expression_string),
              involved_columns(other.involved_columns),
              result_binding(other.result_binding),
              result_type(other.result_type) {
            if (other.expression_tree) {
                expression_tree = other.expression_tree->Copy();
            }
        }

        // Assignment operator
        SumAggInfo& operator=(const SumAggInfo& other) {
            if (this != &other) {
                expression_string = other.expression_string;
                involved_columns = other.involved_columns;
                result_binding = other.result_binding;
                result_type = other.result_type;
                if (other.expression_tree) {
                    expression_tree = other.expression_tree->Copy();
                } else {
                    expression_tree.reset();
                }
            }
            return *this;
        }
        
        // Move constructor
        SumAggInfo(SumAggInfo&& other) noexcept = default;
        
        // Move assignment
        SumAggInfo& operator=(SumAggInfo&& other) noexcept = default;

        // Check if this SUM aggregate involves a specific column
        bool InvolveColumn(const ColumnBinding& binding) const {
            for (const auto& col : involved_columns) {
                if (col.binding == binding) {
                    return true;
                }
            }
            return false;
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

// 1. Main Function

    unique_ptr<LogicalOperator> Rewrite(unique_ptr<LogicalOperator> op);

    unique_ptr<LogicalOperator> ApplyAgg(unique_ptr<LogicalOperator> op);

    unique_ptr<LogicalOperator> UpdateBinding(unique_ptr<LogicalOperator> op);

    void StoreMinMaxAggregates(LogicalOperator* op);

    void StoreSumAggregates(LogicalOperator* op);

    unique_ptr<LogicalOperator> ReplaceRootCountWithSum(unique_ptr<LogicalOperator> op_node);

    unique_ptr<LogicalOperator> AddAnnotAttributeDFS(unique_ptr<LogicalOperator> op_node, bool applyFlag = false);

    unique_ptr<LogicalOperator> AddProjectionWithAnnot(unique_ptr<LogicalOperator> op, unique_ptr<Expression> annot_expr, string name, vector<ColumnBinding> bindings_to_exclude);
    unique_ptr<LogicalOperator> AddProjectionWithAnnot(unique_ptr<LogicalOperator> op, vector<unique_ptr<Expression>> annot_exprs, string name, vector<ColumnBinding> bindings_to_exclude);

    unique_ptr<LogicalOperator> CreateDynamicAggregate(unique_ptr<LogicalOperator> child_node);

// 2. Annot Tool Function

    void UpdateMinMax();

    void UpdateSum();

    void UpdateBindingMapOnce(const ColumnBinding old_binding, const ColumnBinding new_binding);

    void UpdateBindingMap(const ColumnBinding old_binding, const ColumnBinding new_binding);

    ColumnBinding GetUpdatedBindingOnce(const ColumnBinding& original);

    ColumnBinding GetUpdatedBinding(const ColumnBinding& original);

    bool FindAnnotAttribute(LogicalOperator* op, ColumnBinding& annot_binding, LogicalType& annot_type);
    bool FindAllAnnotAttributes(LogicalOperator* op, vector<ColumnBinding>& annot_binding, vector<LogicalType>& annot_type);
    bool FindAllAnnotAttributes(LogicalOperator* op, vector<ColumnBinding>& annot_binding, vector<LogicalType>& annot_type, vector<string>& alias_name);
    
    void GetAnnotColumnBindingsIdx(LogicalOperator* op, vector<idx_t>& annot_indices);

    void UpdateJoinConditions(LogicalComparisonJoin& join);

    string GetColumnName(LogicalOperator* op, idx_t idx);

    bool CheckPKFK(LogicalOperator* op);

    unique_ptr<LogicalOperator> UpdateAnnotMul(unique_ptr<LogicalOperator> op_node);


// 3. Optimization

    unique_ptr<LogicalOperator> PruneAggregation(unique_ptr<LogicalOperator> op, AggOptFunc func);

    unique_ptr<LogicalOperator> PruneAggregationWithProjectionMap(unique_ptr<LogicalOperator> op);

    bool NotHasTooManyGroups(unique_ptr<LogicalOperator>& op);

    void RecordAggPushdown(unique_ptr<LogicalOperator>& op);

    // unique_ptr<LogicalOperator> RemoveHeavyAggregation(unique_ptr<LogicalOperator> op);

// 4. Print & Expression Function

    void ExtractColumnsFromSumExpression(Expression* expr, vector<AggColumnInfo>& columns);

    void AddAlias(const string& alias);

    bool HasAlias(const string& alias) const;

    size_t GetAliasCount() const;
    

private:
    Binder &binder;
    ClientContext &context;
    QueryType query_type;

    std::unordered_map<ColumnBinding, ColumnBinding, ColumnBindingHashFunction> global_binding_map;
    static vector<AggColumnInfo> minmax_columns;  // Store MIN/MAX column info
    static vector<SumAggInfo> sum_aggregates; // Store SUM aggregate info
    static unordered_set<string> alias_set;  // Fast lookup set for all aliases
    static vector<JoinInfo> join_pushdown_info;
};

} // namespace duckdb
