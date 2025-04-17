#include "duckdb/optimizer/aggregation_pushdown.hpp"

#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_order.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/function/scalar_function.hpp" 
#include "duckdb/common/operator/multiply.hpp" 
#include "duckdb/function/scalar/operators.hpp"

#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/core_functions/aggregate/sum_helpers.hpp"
#include "duckdb/function/function_binder.hpp"


/**
 * Process a logical plan with DFS to add annot attributes where appropriate
 */
namespace duckdb {
// Add this helper function in your AggregationPushdown class or at namespace level
template <class OP>
static scalar_function_t GetMultiplyFunction(PhysicalType type) {
    scalar_function_t function;
    switch (type) {
    case PhysicalType::INT32:
        function = &ScalarFunction::BinaryFunction<int32_t, int32_t, int32_t, OP>;
        break;
    case PhysicalType::INT64:
        function = &ScalarFunction::BinaryFunction<int64_t, int64_t, int64_t, OP>;
        break;
    case PhysicalType::FLOAT:
        function = &ScalarFunction::BinaryFunction<float, float, float, OP>;
        break;
    case PhysicalType::DOUBLE:
        function = &ScalarFunction::BinaryFunction<double, double, double, OP>;
        break;
    default:
        // Default to int32
        function = &ScalarFunction::BinaryFunction<int32_t, int32_t, int32_t, OP>;
        break;
    }
    return function;
}

AggregateFunction GetSumAggregate(PhysicalType type);

unique_ptr<LogicalOperator> AggregationPushdown::Rewrite(unique_ptr<LogicalOperator> op) {
    op = AddAnnotAttributeDFS(std::move(op));
    op = ReplaceRootCountWithSum(std::move(op));
    return op;
}

unique_ptr<LogicalOperator> AggregationPushdown::ReplaceRootCountWithSum(unique_ptr<LogicalOperator> op_node) {
    if (!op_node) {
        return op_node;
    }

    std::cout << "At ReplaceRootCountWithSum! " << std::endl;
    
    // Check if this is the root aggregate with COUNT(*)
    if (op_node->type == LogicalOperatorType::LOGICAL_PROJECTION && 
        op_node->children.size() == 1 && 
        op_node->children[0]->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
        
        auto &agg = op_node->children[0]->Cast<LogicalAggregate>();
        
        // Check if it's a simple COUNT(*) aggregation (no GROUP BY, single expression)
        if (agg.groups.empty() && agg.expressions.size() == 1) {
            auto &expr = agg.expressions[0];
            
            // Verify it's a COUNT(*) expression
            if (expr->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
                auto &bound_agg = expr->Cast<BoundAggregateExpression>();
                
                // Check if this is COUNT(*) or COUNT_STAR
                if (bound_agg.function.name == "count_star" || 
                    (bound_agg.function.name == "count" && bound_agg.children.empty())) {
                    
                    // Search for annot column in the child
                    ColumnBinding annot_binding;
                    if (agg.children.size() > 0 && 
                        FindAnnotAttribute(agg.children[0].get(), annot_binding)) {
                        
                        // Found annot - replace COUNT(*) with SUM(annot)
                        vector<unique_ptr<Expression>> sum_args;
                        
                        // Get the type of the annot column
                        auto annot_type = LogicalType(LogicalTypeId::BIGINT); // Default, adjust if needed
                        
                        // Try to get the actual type from bindings
                        auto child_types = agg.children[0]->types;
                        auto child_bindings = agg.children[0]->GetColumnBindings();
                        
                        for (idx_t j = 0; j < child_bindings.size(); j++) {
                            if (child_bindings[j] == annot_binding) {
                                annot_type = child_types[j];
                                std::cout << "Found annot type idnex: " << j << std::endl;
                                break;
                            }
                        }
                        
                        // Create reference to annot column
                        auto annot_col_ref = make_uniq<BoundColumnRefExpression>(
                            "annot",
                            annot_type,
                            annot_binding
                        );
                        
                        sum_args.push_back(std::move(annot_col_ref));
                        
                        AggregateFunction sum_function = GetSumAggregate(annot_type.InternalType());
                        if (sum_function.name.empty()) {
                            sum_function.name = "sum";  // Explicitly set the name
                        }        
                        sum_function.return_type = LogicalType::BIGINT;                

                        FunctionBinder function_binder(context);
                        auto sum_expr = function_binder.BindAggregateFunction(
                            sum_function, 
                            std::move(sum_args), 
                            nullptr, 
                            AggregateType::NON_DISTINCT
                        );
                        
                        // Preserve alias from original COUNT(*)
                        sum_expr->alias = "annot";
                        // Replace the expression
                        agg.expressions[0] = std::move(sum_expr);

                        // NOTE: Adjust projection column to annot only
                        auto &proj = op_node->Cast<LogicalProjection>();
                        vector<unique_ptr<Expression>> new_proj_exprs;

                        auto agg_binding = ColumnBinding(agg.aggregate_index, 0);
                        auto annot_col_ref_proj = make_uniq<BoundColumnRefExpression>(
                            "annot",
                            agg.expressions[0]->return_type, // Get the type from the aggregate expression
                            agg_binding                      // Use aggregate's binding, not the input binding
                        );
                        
                        // Add only this expression to the projection
                        new_proj_exprs.push_back(std::move(annot_col_ref_proj));
                        
                        // Replace all projection expressions with just the annot
                        proj.expressions = std::move(new_proj_exprs);
                        
                        // Update the projection's output types to match the new expression list
                        proj.types.clear();
                        proj.types.push_back(agg.expressions[0]->return_type);

                        op_node->ResolveOperatorTypes();
                        return op_node;
                    }
                }
            }
        }
    }
    else {
        throw std::runtime_error("Not implemented");
    }
}

unique_ptr<LogicalOperator> AggregationPushdown::AddAnnotAttributeDFS(unique_ptr<LogicalOperator> op_node) {
    if (!op_node) {
        return op_node;
    }
    std::cout << "At AddAnnotAttributeDFS! " << std::endl;
    std::cout << op_node->ToString();
    // Special handling for join operators
    if (op_node->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN ||
        op_node->type == LogicalOperatorType::LOGICAL_ASOF_JOIN ||
        op_node->type == LogicalOperatorType::LOGICAL_DELIM_JOIN) {
        
        auto &join = op_node->Cast<LogicalComparisonJoin>();
        
        // First recursively process children (but don't modify them yet)
        join.children[0] = AddAnnotAttributeDFS(std::move(join.children[0]));
        join.children[1] = AddAnnotAttributeDFS(std::move(join.children[1]));
        
        // Then apply CreateDynamicAggregate to children if needed
        // 2. THEN, apply transformations to children if needed
        // UNCOMMENT THESE CRITICAL LINES - they ensure consistent application of aggregates
        join.children[0] = CreateDynamicAggregate(std::move(join.children[0]));
        join.children[1] = CreateDynamicAggregate(std::move(join.children[1]));

        // Update join conditions to use new bindings
        UpdateJoinConditions(join);
        
        // 3. FINALLY, after children are properly transformed, handle this operator
        bool left_has_annot, right_has_annot;
        ColumnBinding left_annot, right_annot;
        
        left_has_annot = FindAnnotAttribute(join.children[0].get(), left_annot);
        right_has_annot = FindAnnotAttribute(join.children[1].get(), right_annot);

        // Handle the three different cases for annot propagation
        if (left_has_annot && right_has_annot) {
            // Case 1: Both children have annot -> create multiplication
            auto annot_type = LogicalType(LogicalTypeId::BIGINT); // Adjust as needed
            
            // Need a projection for the computed expression
            auto left_ref = make_uniq<BoundColumnRefExpression>(annot_type, left_annot);
            auto right_ref = make_uniq<BoundColumnRefExpression>(annot_type, right_annot);
            
            vector<unique_ptr<Expression>> mult_children;
            mult_children.push_back(std::move(left_ref));
            mult_children.push_back(std::move(right_ref));
            
            // Option 1: Use existing DuckDB multiplication function (best approach)
            ScalarFunction multiply_func(
                "*", 
                {annot_type, annot_type}, 
                annot_type,
                GetMultiplyFunction<MultiplyOperator>(annot_type.InternalType())
            );

            // Create the function expression
            auto mult_expr = make_uniq<BoundFunctionExpression>(
                annot_type,          // Return type
                multiply_func,       // Function
                std::move(mult_children),  // Arguments
                nullptr,             // Bind info
                true                 // Is operator
            );
            // Add projection with multiplication
            op_node->ResolveOperatorTypes();
            vector<ColumnBinding> bindings_to_exclude = {left_annot, right_annot};
            auto projection = AddProjectionWithAnnot(std::move(op_node), std::move(mult_expr), "annot", bindings_to_exclude);
            return projection;
        } 
        else {
            // Case 2 & 3: Only right has annot -> pass through
            // Similar to left case, for right projection map
            return op_node;
        }
    } else {
        // Process each child
        for (auto &child : op_node->children) {
            child = AddAnnotAttributeDFS(std::move(child));
        }
        return op_node;
    }
}

// Helper to find annot attribute in an operator
bool AggregationPushdown::FindAnnotAttribute(LogicalOperator* op, ColumnBinding& annot_binding) {
    // Direct scan for annot column
    std::cout << "At FindAnnotAttribute! " << std::endl;
    auto bindings = op->GetColumnBindings();
    
    // Check in expressions for special operators
    if (op->type == LogicalOperatorType::LOGICAL_PROJECTION) {
        auto& proj = op->Cast<LogicalProjection>();
        for (idx_t i = 0; i < proj.expressions.size(); i++) {
            auto& expr = proj.expressions[i];
            if (expr->GetName() == "annot") {
                annot_binding = ColumnBinding(proj.table_index, i);
                return true;
            }
        }
    }
    
    if (op->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
        auto& agg = op->Cast<LogicalAggregate>();
        for (idx_t i = 0; i < agg.expressions.size(); i++) {
            if (agg.expressions[i]->GetName() == "annot") {
                annot_binding = ColumnBinding(agg.aggregate_index, i);
                return true;
            }
        }
    }
    
    // Use position heuristics if needed
    // [position-based detection logic would go here]
    
    return false;
}

// Helper to add a projection with annot expression
unique_ptr<LogicalOperator> AggregationPushdown::AddProjectionWithAnnot(unique_ptr<LogicalOperator> op, unique_ptr<Expression> annot_expr, string name, vector<ColumnBinding> bindings_to_exclude) {
    // Get bindings from operator
    std::cout << "At AddProjectionWithAnnot! " << std::endl;
    std::cout << op->ToString();

    auto bindings = op->GetColumnBindings();
    
    // Create expressions for projection
    vector<unique_ptr<Expression>> projection_expressions;

    // Create and return the projection
    idx_t projection_index = binder.GenerateTableIndex();
    
    // Add all existing columns
    for (idx_t i = 0; i < bindings.size(); i++) {
        bool should_exclude = false;
        for (const auto& exclude_binding : bindings_to_exclude) {
            if (bindings[i] == exclude_binding) {
                should_exclude = true;
                break;
            }
        }
        
        if (should_exclude) {
            continue;  // Skip this binding
        }
        auto col_ref = make_uniq<BoundColumnRefExpression>(
            op->types[i],
            bindings[i]
        );
        projection_expressions.push_back(std::move(col_ref));
        // NOTE: Update to new binding
        ColumnBinding old_binding = bindings[i];
        ColumnBinding new_binding = ColumnBinding(projection_index, projection_expressions.size()-1);
        global_binding_map[old_binding] = new_binding;
    }
    
    // Then add the annot expression
    annot_expr->alias = name;
    projection_expressions.push_back(std::move(annot_expr));
    
    auto projection = make_uniq<LogicalProjection>(projection_index, std::move(projection_expressions));
    
    // Add the original operator as child
    projection->AddChild(std::move(op));
    projection->ResolveOperatorTypes();
    
    // Return the new projection
    return projection;
}

unique_ptr<LogicalOperator> AggregationPushdown::CreateDynamicAggregate(unique_ptr<LogicalOperator> child_node) {
    // Extract child column information before we modify it
    std::cout << "At CreateDynamicAggregate! " << std::endl;
    std::cout << child_node->ToString();

    vector<ColumnBinding> child_bindings = child_node->GetColumnBindings();
    vector<LogicalType> child_types = child_node->types;
    
    // Check if "annot" column exists
    bool has_annot_column = false;
    idx_t annot_idx = DConstants::INVALID_INDEX;
    string annot_name = "annot";
    
    if (child_node->type != LogicalOperatorType::LOGICAL_GET) {
        for (idx_t i = 0; i < child_node->expressions.size(); i++) {
            if (child_node->expressions[i]->GetName() == annot_name) {
                has_annot_column = true;
                annot_idx = i;
                break;
            }
        }
    } 
    
    // Get next available table indices
    idx_t group_index = binder.GenerateTableIndex();
    idx_t aggregate_index = binder.GenerateTableIndex();
    
    // Create the aggregate expression (COUNT(*) or SUM(annot))
    vector<unique_ptr<Expression>> select_list;
    
    if (has_annot_column) {
        // Create SUM(annot)
        vector<unique_ptr<Expression>> sum_args;
        
        // Create reference to annot column
        auto annot_col_ref = make_uniq<BoundColumnRefExpression>(
            annot_name,
            child_types[annot_idx],
            child_bindings[annot_idx]
        );
        
        sum_args.push_back(std::move(annot_col_ref));
        
        // Create an AggregateFunction object directly
        // AggregateFunction sum_function("sum", {child_types[annot_idx]}, LogicalType(child_types[annot_idx]),
        //                                 nullptr, nullptr, nullptr, nullptr, nullptr,
        //                                FunctionNullHandling::DEFAULT_NULL_HANDLING);
        // auto sum_function = aggregate_functions::SumFun::GetFunction(child_types[annot_idx]);
        AggregateFunction sum_function = GetSumAggregate(child_types[annot_idx].InternalType());
        if (sum_function.name.empty()) {
            sum_function.name = "sum";
        }

        sum_function.return_type = LogicalType::BIGINT;  

        FunctionBinder function_binder(context);
        auto sum_expr = function_binder.BindAggregateFunction(
            sum_function, 
            std::move(sum_args), 
            nullptr, 
            AggregateType::NON_DISTINCT
        );
        /*
        auto sum_expr = make_uniq<BoundAggregateExpression>(
            sum_function,          // AggregateFunction instance
            std::move(sum_args),   // Child expressions
            nullptr,               // Filter (no filter)
            nullptr,               // Bind info
            AggregateType::NON_DISTINCT  // Not distinct
        );*/
        
        sum_expr->alias = "annot";      // Ensure it has the annot name
        select_list.push_back(std::move(sum_expr));
    } else {
        // Create COUNT(*)
        vector<unique_ptr<Expression>> empty_args;
    
        // Create an AggregateFunction for COUNT(*)
        // AggregateFunction count_star_function("count_star", {}, LogicalType(LogicalTypeId::BIGINT),
        //                                       nullptr, nullptr, nullptr, nullptr, nullptr,
        //                                       FunctionNullHandling::DEFAULT_NULL_HANDLING);
        // auto count_star_function = aggregate_functions::CountStarFun::GetFunction();
        // AggregateFunction count_star_function("count_star", empty_args, LogicalType::BIGINT);
        auto count_star_fun = CountStarFun::GetFunction();
        if (count_star_fun.name.empty()) {
            count_star_fun.name = "count_star";
        }

        count_star_fun.return_type = LogicalType::BIGINT;  // Set the return type to BIGINT

        FunctionBinder function_binder(context);
        auto count_star = function_binder.BindAggregateFunction(
            count_star_fun, 
            {}, 
            nullptr, 
            AggregateType::NON_DISTINCT
        );
        /*
        auto count_star = make_uniq<BoundAggregateExpression>(
            count_star_function,     // AggregateFunction instance
            std::move(empty_args),   // No arguments for COUNT(*)
            nullptr,                 // Filter (no filter)
            nullptr,                 // Bind info
            AggregateType::NON_DISTINCT  // Not distinct
        );*/

        count_star->alias = "annot";    // Name the count(*) column as "annot"
        select_list.push_back(std::move(count_star));
    }
    
    // Create the LogicalAggregate
    auto aggregate = make_uniq<LogicalAggregate>(
        group_index, 
        aggregate_index,
        std::move(select_list)
    );
    
    int group_pos = 0;
    // Add all columns except "annot" to GROUP BY
    for (idx_t i = 0; i < child_bindings.size(); i++) {
        // Skip the annot column for grouping
        if (has_annot_column && i == annot_idx) {
            continue;
        }
        
        // Create GROUP BY expression for this column
        auto col_type = i < child_types.size() ? child_types[i] : LogicalType::BIGINT;
        auto col_name = GetColumnName(child_node.get(), i);
        
        auto group_expr = make_uniq<BoundColumnRefExpression>(
            col_name,
            col_type,
            child_bindings[i]  // Use NEW binding with the group_index
        );
        
        aggregate->groups.push_back(std::move(group_expr));

        // NOTE: Update to new binding
        ColumnBinding old_binding = child_bindings[i];
        ColumnBinding new_binding = ColumnBinding(group_index, group_pos++);
        global_binding_map[old_binding] = new_binding;
    }
    
    // Create a single grouping set with all group columns
    if (!aggregate->groups.empty()) {
        GroupingSet grouping_set;
        for (idx_t i = 0; i < aggregate->groups.size(); i++) {
            grouping_set.insert(i);
        }
        aggregate->grouping_sets.push_back(std::move(grouping_set));
    }
    
    // Save the original child and add it to the new aggregate
    aggregate->AddChild(std::move(child_node));
    aggregate->ResolveOperatorTypes();

    child_node = std::move(aggregate);
    
    std::cout << "After CreateDynamicAggregate! " << std::endl;
    std::cout << child_node->ToString();
    std::cout << child_node->children[0]->ToString();

    return child_node;
}

ColumnBinding AggregationPushdown::GetUpdatedBinding(const ColumnBinding& original) {
    // Follow the chain of mappings until we reach an unmapped binding
    ColumnBinding current = original;
    while (global_binding_map.count(current)) {
        current = global_binding_map[current];
        
        // Safety check to prevent infinite loops if there's a cycle
        if (current == original) {
            break;
        }
    }
    return current;
}

void AggregationPushdown::UpdateJoinConditions(LogicalComparisonJoin& join) {
    // Process each child separately
    for (idx_t child_idx = 0; child_idx < join.children.size(); child_idx++) {
        auto& child = join.children[child_idx];
        // Now update all join conditions that reference this child
        for (auto& condition : join.conditions) {
            // Update left side of condition
            if (condition.left->type == ExpressionType::BOUND_COLUMN_REF) {
                auto& left_col = condition.left->Cast<BoundColumnRefExpression>();
                left_col.binding = GetUpdatedBinding(left_col.binding);
            }
            
            // Update right side of condition
            if (condition.right->type == ExpressionType::BOUND_COLUMN_REF) {
                auto& right_col = condition.right->Cast<BoundColumnRefExpression>();
                right_col.binding = GetUpdatedBinding(right_col.binding);
            }
        }
    }
}

string AggregationPushdown::GetColumnName(LogicalOperator* op, idx_t idx) {
    if (op->type == LogicalOperatorType::LOGICAL_GET) {
        auto &get = op->Cast<LogicalGet>();
        if (idx < get.column_ids.size()) {
            // Map from output position (idx) to actual column ID
            idx_t actual_col_id = get.column_ids[idx];
            if (actual_col_id < get.names.size()) {
                return get.names[actual_col_id];
            }
        }
    } else if (op->type == LogicalOperatorType::LOGICAL_PROJECTION) {
        if (idx < op->expressions.size()) {
            return op->expressions[idx]->GetName();
        }
    } else if (op->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
        // For joins, need to check which side the column comes from
        auto &join = op->Cast<LogicalComparisonJoin>();
        auto left_count = join.children[0]->types.size();
        
        if (idx < left_count) {
            // Column from left child
            return GetColumnName(join.children[0].get(), idx);
        } else {
            // Column from right child
            return GetColumnName(join.children[1].get(), idx - left_count);
        }
    } else {
        throw NotImplementedException("GetColumnName not implemented for this operator type");
    }
    
    // Default if we can't find a name
    return "col" + std::to_string(idx);
}

unique_ptr<LogicalOperator> AggregationPushdown::PruneAggregationColumns(unique_ptr<LogicalOperator> op) {
    if (!op) {
        return op;
    }
    
    for (idx_t i = 0; i < op->children.size(); i++) {
        op->children[i] = PruneAggregationColumns(std::move(op->children[i]));
    }

    // First process this operator if it's a join
    if (op->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN ||
        op->type == LogicalOperatorType::LOGICAL_ASOF_JOIN ||
        op->type == LogicalOperatorType::LOGICAL_DELIM_JOIN) {
        
        auto &join = op->Cast<LogicalComparisonJoin>();
        
        // Process left child
        if (!join.left_projection_map.empty() && 
            join.children[0]->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
            std::cout << "Pruning left aggregate columns with projection_map" << std::endl;
            PruneAggregationWithProjectionMap(join.children[0].get(), join.left_projection_map);
            // Clear the join's left projection map
            join.left_projection_map.clear();
        }
        
        // Process right child
        if (!join.right_projection_map.empty() && 
            join.children[1]->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
            std::cout << "Pruning right aggregate columns with projection_map" << std::endl;
            PruneAggregationWithProjectionMap(join.children[1].get(), join.right_projection_map);
            // Clear the join's right projection map
            join.right_projection_map.clear();
        }

        UpdateJoinConditions(join);
    }

    return op;
}

void AggregationPushdown::PruneAggregationWithProjectionMap(LogicalOperator* op, const vector<idx_t>& projection_map) {
    auto &agg = op->Cast<LogicalAggregate>();
    
    // Create new vectors for the pruned expressions
    vector<unique_ptr<Expression>> new_agg_exprs;
    vector<unique_ptr<Expression>> new_groups;
    
    // Map to track which group columns and aggregate expressions to keep
    std::unordered_set<idx_t> kept_agg_indices;
    std::unordered_set<idx_t> kept_group_indices;

    for (idx_t proj_idx : projection_map) {
        if (proj_idx < agg.groups.size()) {
            // It refers to a group column
            kept_group_indices.insert(proj_idx);
        } else {
            // It refers to an aggregate expression
            idx_t agg_idx = proj_idx - agg.groups.size();
            if (agg_idx < agg.expressions.size()) {
                kept_agg_indices.insert(agg_idx);
            }
        }
    }
    
    agg.types.clear();
    idx_t new_group_idx = 0;
    // Keep only the wanted group expressions
    for (idx_t i = 0; i < agg.groups.size(); i++) {
        if (kept_group_indices.count(i) > 0) {
            agg.types.push_back(agg.groups[i]->return_type);
            new_groups.push_back(std::move(agg.groups[i]));
            auto old_binding = ColumnBinding(agg.group_index, i);
            auto new_binding = ColumnBinding(agg.group_index, new_group_idx++);
            global_binding_map[old_binding] = new_binding;
        }
    }
    idx_t new_agg_idx = 0;
    // Keep only the wanted aggregate expressions
    for (idx_t i = 0; i < agg.expressions.size(); i++) {
        if (kept_agg_indices.count(i) > 0) {
            agg.types.push_back(agg.expressions[i]->return_type);
            new_agg_exprs.push_back(std::move(agg.expressions[i]));
            auto old_binding = ColumnBinding(agg.aggregate_index, i);
            auto new_binding = ColumnBinding(agg.aggregate_index, new_agg_idx++);
            global_binding_map[old_binding] = new_binding;
        }
    }
    
    // Update the aggregate's expressions and groups
    agg.expressions = std::move(new_agg_exprs);
    agg.groups = std::move(new_groups);

    // Update grouping_sets to match new group structure
    if (!agg.groups.empty()) {
        agg.grouping_sets.clear();
        GroupingSet new_grouping_set;
        for (idx_t i = 0; i < agg.groups.size(); i++) {
            new_grouping_set.insert(i);
        }
        agg.grouping_sets.push_back(std::move(new_grouping_set));
    }

    // Update operator types
    op->ResolveOperatorTypes();
    
    std::cout << "After pruning, aggregate has " << agg.expressions.size()
              << " expressions and " << agg.groups.size() << " groups" << std::endl;
}

}