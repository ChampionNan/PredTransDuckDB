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
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"

// Self-defined functions & declarations
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
        case PhysicalType::INT128:  // Add this case for HUGEINT
            function = &ScalarFunction::BinaryFunction<__int128_t, __int128_t, __int128_t, OP>;
            break;
        case PhysicalType::FLOAT:
            function = &ScalarFunction::BinaryFunction<float, float, float, OP>;
            break;
        case PhysicalType::DOUBLE:
            function = &ScalarFunction::BinaryFunction<double, double, double, OP>;
            break;
        default:
            // Default to hugeint
            function = &ScalarFunction::BinaryFunction<hugeint_t, hugeint_t, hugeint_t, OP>;
            break;
    }
    return function;
}

AggregateFunction GetSumAggregate(PhysicalType type);
unique_ptr<FunctionData> BindDecimalMultiply(ClientContext &context, ScalarFunction &bound_function,
    vector<unique_ptr<Expression>> &arguments);

unique_ptr<LogicalOperator> AggregationPushdown::Rewrite(unique_ptr<LogicalOperator> op) {
    global_binding_map.clear();
    op = AddAnnotAttributeDFS(std::move(op));
    op = ReplaceRootCountWithSum(std::move(op));
    return op;
}

unique_ptr<LogicalOperator> AggregationPushdown::UpdateBinding(unique_ptr<LogicalOperator> op) {
    op = PruneAggregationColumns(std::move(op));
    op = UpdateAnnotMul(std::move(op));
    return op;
}

unique_ptr<LogicalOperator> AggregationPushdown::ReplaceRootCountWithSum(unique_ptr<LogicalOperator> op_node) {
    if (!op_node) {
        return op_node;
    }

    // FIXME: DEBUG ADDING
    // return std::move(op_node->children[0]);

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
                    LogicalType annot_type; // Default, adjust if needed
                    if (agg.children.size() > 0 && 
                        FindAnnotAttribute(agg.children[0].get(), annot_binding, annot_type)) {
                        
                        // Found annot - replace COUNT(*) with SUM(annot)
                        vector<unique_ptr<Expression>> sum_args;
                        
                        // Try to get the actual type from bindings
                        agg.ResolveOperatorTypes();
                        auto child_types = agg.children[0]->types;
                        auto child_bindings = agg.children[0]->GetColumnBindings();
                        
                        // Create reference to annot column
                        auto annot_col_ref = make_uniq<BoundColumnRefExpression>(
                            // "annot",
                            annot_type,
                            annot_binding
                        );
                        
                        sum_args.push_back(std::move(annot_col_ref));
                        
                        AggregateFunction sum_function = GetSumAggregate(annot_type.InternalType());
                        if (sum_function.name.empty()) {
                            sum_function.name = "sum";  // Explicitly set the name
                        }        
                        // sum_function.return_type = LogicalType::HUGEINT;                

                        FunctionBinder function_binder(context);
                        auto sum_expr = function_binder.BindAggregateFunction(
                            sum_function, 
                            std::move(sum_args), 
                            nullptr, 
                            AggregateType::NON_DISTINCT
                        );
                        // Replace the expression
                        agg.expressions[0] = std::move(sum_expr);

                        agg.ResolveOperatorTypes();

                        // NOTE: Adjust projection column to annot only
                        auto &proj = op_node->Cast<LogicalProjection>();
                        vector<unique_ptr<Expression>> new_proj_exprs;

                        auto agg_binding = ColumnBinding(agg.aggregate_index, 0);
                        auto annot_col_ref_proj = make_uniq<BoundColumnRefExpression>(
                            agg.expressions[0]->return_type, // Get the type from the aggregate expression
                            agg_binding                      // Use aggregate's binding, not the input binding
                        );
                        
                        // Add only this expression to the projection
                        new_proj_exprs.push_back(std::move(annot_col_ref_proj));
                        
                        // Replace all projection expressions with just the annot
                        proj.expressions = std::move(new_proj_exprs);
                        
                        // Update the projection's output types to match the new expression list
                        proj.types.clear();
                        proj.types.push_back(proj.expressions[0]->return_type);

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

bool AggregationPushdown::CheckPKFK(LogicalOperator* op) {
    // Check if operator is LogicalGet
    if (op->type != LogicalOperatorType::LOGICAL_GET) {
        return false; // Not a direct table, can't determine PK status
    }

    // Get all column bindings for this operator
    auto bindings = op->GetColumnBindings();
    
    auto& get_op = op->Cast<LogicalGet>();
    auto table_entry = get_op.GetTable();
    if (!table_entry) {
        return false; // Not a regular table
    }
    
    auto &constraints = table_entry->GetConstraints();
    
    // For each column binding from this operator
    for (idx_t i = 0; i < bindings.size(); i++) {
        // Map the binding column index to the actual column index in the table
        idx_t col_idx = bindings[i].column_index;
        if (col_idx >= get_op.column_ids.size()) {
            continue;
        }
        
        idx_t actual_col_idx = get_op.column_ids[col_idx];
        
        // Check constraints for any uniqueness guarantee
        for (auto& constraint : constraints) {
            if (constraint->type == ConstraintType::UNIQUE) {
                auto& unique_constraint = constraint->Cast<UniqueConstraint>();
                
                // For single-column unique constraint (primary key or unique)
                if (unique_constraint.index.index != DConstants::INVALID_INDEX) {
                    if (unique_constraint.index.index == actual_col_idx) {
                        std::cout << "Found unique constraint on column: " << get_op.names[col_idx] << std::endl;
                        return true;
                    }
                }
                // For multi-column unique constraint (primary key or unique)
                else if (!unique_constraint.columns.empty()) {
                    // Get column name from physical index
                    string column_name = get_op.names[actual_col_idx];
                    
                    // Check if column name is in the unique constraint
                    for (auto& constraint_col : unique_constraint.columns) {
                        if (constraint_col == column_name) {
                            std::cout << "Found column in multi-column unique constraint: " << column_name << std::endl;
                            return true;
                        }
                    }
                }
            }
        }
    }
    
    return false; // No unique key found
}

unique_ptr<LogicalOperator> AggregationPushdown::AddAnnotAttributeDFS(unique_ptr<LogicalOperator> op_node) {
    if (!op_node) {
        return op_node;
    }
    // Special handling for join operators
    if (op_node->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN ||
        op_node->type == LogicalOperatorType::LOGICAL_ASOF_JOIN ||
        op_node->type == LogicalOperatorType::LOGICAL_DELIM_JOIN) {
        
        auto &join = op_node->Cast<LogicalComparisonJoin>();
        
        // First recursively process children (but don't modify them yet)
        join.children[0] = AddAnnotAttributeDFS(std::move(join.children[0]));
        join.children[1] = AddAnnotAttributeDFS(std::move(join.children[1]));

        bool addLeft = true;
        bool addRight = true;
        
        // Check if any column from left child is a unique key
        if (CheckPKFK(join.children[0].get())) {
            addLeft = false;
            std::cout << "Left child has unique key, skipping annot" << std::endl;
        }
        
        // Check if any column from right child is a unique key
        if (CheckPKFK(join.children[1].get())) {
            addRight = false;
            std::cout << "Right child has unique key, skipping annot" << std::endl;
        }

        if (addLeft) {
            join.children[0] = CreateDynamicAggregate(std::move(join.children[0]));
        }
        if (addRight) {
            join.children[1] = CreateDynamicAggregate(std::move(join.children[1]));
        }
        // Update join conditions to use new bindings
        UpdateJoinConditions(join);
        op_node->ResolveOperatorTypes();
        
        // 3. FINALLY, after children are properly transformed, handle this operator
        bool left_has_annot, right_has_annot;
        ColumnBinding left_annot, right_annot;
        LogicalType left_type, right_type;
        
        left_has_annot = FindAnnotAttribute(join.children[0].get(), left_annot, left_type);
        right_has_annot = FindAnnotAttribute(join.children[1].get(), right_annot, right_type);

        // Handle the three different cases for annot propagation
        if (left_has_annot && right_has_annot) {
            // Case 1: Both children have annot -> create multiplication
            vector<unique_ptr<Expression>> mult_children;
            auto left_ref = make_uniq<BoundColumnRefExpression>(left_type, left_annot);
            mult_children.push_back(std::move(left_ref));
            auto right_ref = make_uniq<BoundColumnRefExpression>(right_type, right_annot);
            mult_children.push_back(std::move(right_ref));

            /*
            LogicalType left_operand_type = left_type;
            LogicalType right_operand_type = right_type;
            bool needs_cast = (left_type != right_type && (left_type.id() == LogicalTypeId::HUGEINT || right_type.id() == LogicalTypeId::HUGEINT));
            if (needs_cast) {
                // Cast to HUGEINT
                if (left_type.id() == LogicalTypeId::HUGEINT) {
                    auto left_ref = make_uniq<BoundColumnRefExpression>(left_type, left_annot);
                    mult_children.push_back(std::move(left_ref));
        
                    // Create right reference with CAST
                    auto right_ref = make_uniq<BoundColumnRefExpression>(right_type, right_annot);
                    auto cast_expr = BoundCastExpression::AddDefaultCastToType(
                        std::move(right_ref),
                        LogicalType(LogicalTypeId::HUGEINT)
                    );
                    mult_children.push_back(std::move(cast_expr));
                    right_operand_type = LogicalType::HUGEINT;
                } else {
                    auto left_ref = make_uniq<BoundColumnRefExpression>(left_type, left_annot);
                    auto cast_expr = BoundCastExpression::AddDefaultCastToType(
                        std::move(left_ref),
                        LogicalType(LogicalTypeId::HUGEINT)
                    );
                    mult_children.push_back(std::move(cast_expr));
        
                    // Create right reference normally
                    auto right_ref = make_uniq<BoundColumnRefExpression>(right_type, right_annot);
                    mult_children.push_back(std::move(right_ref));
        
                    // Set function argument types
                    left_operand_type = LogicalType::HUGEINT;
                }
            } else {
                auto left_ref = make_uniq<BoundColumnRefExpression>(left_type, left_annot);
                auto right_ref = make_uniq<BoundColumnRefExpression>(right_type, right_annot);
                
                mult_children.push_back(std::move(left_ref));
                mult_children.push_back(std::move(right_ref));
            }
            std::cout << "Print type!" << std::endl;
            std::cout << "Left type raw value: " << static_cast<int>(left_operand_type.InternalType()) << std::endl;
            std::cout << "Right type raw value: " << static_cast<int>(right_operand_type.InternalType()) << std::endl;
            */
            
            FunctionBinder function_binder(context);
            string error_msg;

            // Let DuckDB automatically select the right function and return type
            auto bound_expr = function_binder.BindScalarFunction(
                DEFAULT_SCHEMA,    // Schema name
                "*",                // Function name
                std::move(mult_children),
                error_msg,         // Error message
                true               // Don't check parameter count
            );
            if (!bound_expr) {
                throw Exception("Failed to bind multiplication function: " + error_msg);
            }
            auto mult_expr = std::move(bound_expr);


            // Create the function expression with the properly determined type
            /*
            auto mult_expr = make_uniq<BoundFunctionExpression>(
                bound_function.return_type,  // DuckDB determined return type
                bound_function,              // Properly bound function
                std::move(mult_children),    // Move arguments
                nullptr,                     // Bind info
                true                         // Is operator
            );*/
            // Method1
            /*
            ScalarFunction multiply_func(
                "*", 
                {left_operand_type, right_operand_type}, 
                LogicalType::HUGEINT, // Return type (will be determined by bind)
                GetMultiplyFunction<MultiplyOperator>(left_operand_type.InternalType())
            );

            // Create the function expression
            auto mult_expr = make_uniq<BoundFunctionExpression>(
                LogicalType::HUGEINT,          // Return type
                multiply_func,       // Function
                std::move(mult_children),  // Arguments
                nullptr,             // Bind info
                true                 // Is operator
            );*/
            // Add projection with multiplication
            
            /*
            // Create the ScalarFunction with explicit BindDecimalMultiply
            ScalarFunction multiply_func(
                "*",                       // Function name
                arg_types,                 // Argument types
                LogicalType::HUGEINT,      // Return type (will be determined by bind)
                nullptr,                   // Function pointer (will be set by bind)
                BindDecimalMultiply        // Explicitly use BindDecimalMultiply
            );

            auto bind_data = BindDecimalMultiply(context, multiply_func, mult_children);

            std::cout << "Multiplication return type: " << multiply_func.return_type.ToString() << std::endl;

            // Create the function expression with the determined return type
            auto mult_expr = make_uniq<BoundFunctionExpression>(
                multiply_func.return_type,  // Use the type determined by BindDecimalMultiply
                multiply_func,             // Function with proper implementation
                std::move(mult_children),  // Arguments
                std::move(bind_data),      // Pass the bind data
                true                       // Is operator
            );*/
            vector<ColumnBinding> bindings_to_exclude = {left_annot, right_annot};
            auto projection = AddProjectionWithAnnot(std::move(op_node), std::move(mult_expr), "annot", bindings_to_exclude);
            return projection;
        } else if (left_has_annot) {
            // Case 2: Only left child has annot
            auto left_ref = make_uniq<BoundColumnRefExpression>(left_type, left_annot);
            auto projection = AddProjectionWithAnnot(std::move(op_node), std::move(left_ref), "annot", {left_annot});
            return projection;
        } else if (right_has_annot) {
            // Case 3: Only right child has annot
            auto right_ref = make_uniq<BoundColumnRefExpression>(right_type, right_annot);
            auto projection = AddProjectionWithAnnot(std::move(op_node), std::move(right_ref), "annot", {right_annot});
            return projection;
        } else {
            // Case 4: Neither child has an annotation column
            // Create a projection without adding an annotation column
            auto projection = AddProjectionWithAnnot(std::move(op_node), nullptr, "", {});
            return projection;
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
bool AggregationPushdown::FindAnnotAttribute(LogicalOperator* op, ColumnBinding& annot_binding, LogicalType& annot_type) {
    // Direct scan for annot column
    std::cout << "At FindAnnotAttribute! " << std::endl;
    auto bindings = op->GetColumnBindings();
    
    // Check in expressions for special operators
    if (op->type == LogicalOperatorType::LOGICAL_PROJECTION) {
        auto& proj = op->Cast<LogicalProjection>();
        for (idx_t i = 0; i < proj.expressions.size(); i++) {
            auto& expr = proj.expressions[i];
            if (expr->GetName() == "annot") {
                std::cout << "Found annot in projection!" << std::endl;
                annot_binding = ColumnBinding(proj.table_index, i);
                annot_type = expr->return_type;
                return true;
            }
        }
    }
    
    if (op->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
        auto& agg = op->Cast<LogicalAggregate>();
        for (idx_t i = 0; i < agg.expressions.size(); i++) {
            if (agg.expressions[i]->GetName() == "annot") {
                annot_binding = ColumnBinding(agg.aggregate_index, i);
                annot_type = agg.expressions[i]->return_type;
                return true;
            }
        }
    }

    return false;
}

// Helper to add a projection with annot expression, both annot1 * annot2 and annot
unique_ptr<LogicalOperator> AggregationPushdown::AddProjectionWithAnnot(unique_ptr<LogicalOperator> op, unique_ptr<Expression> annot_expr, string name, vector<ColumnBinding> bindings_to_exclude) {
    // Get bindings from operator
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
    if (annot_expr) {
        annot_expr->alias = name;
        projection_expressions.push_back(std::move(annot_expr));
    }
    
    auto projection = make_uniq<LogicalProjection>(projection_index, std::move(projection_expressions));
    
    // Add the original operator as child
    projection->AddChild(std::move(op));
    projection->ResolveOperatorTypes();

    std::cout << "Projection created with index: " << projection_index << std::endl;
    projection->Print();
    
    // Return the new projection
    return projection;
}

unique_ptr<LogicalOperator> AggregationPushdown::CreateDynamicAggregate(unique_ptr<LogicalOperator> child_node) {
    // Extract child column information before we modify it
    vector<ColumnBinding> child_bindings = child_node->GetColumnBindings();
    child_node->ResolveOperatorTypes();
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
        std::cout << "Create SUM(annot)!" << std::endl;
        vector<unique_ptr<Expression>> sum_args;
        
        // Create reference to annot column
        auto annot_col_ref = make_uniq<BoundColumnRefExpression>(
            // annot_name,
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

        // sum_function.return_type = LogicalType::HUGEINT;  

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
        
        // sum_expr->alias = "annot";      // Ensure it has the annot name
        select_list.push_back(std::move(sum_expr));
    } else {
        std::cout << "Create COUNT(*)!" << std::endl;
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

        // count_star_fun.return_type = LogicalType::BIGINT;  // Set the return type to BIGINT


        // count_star_fun.return_type = LogicalType::BIGINT;  // Set the return type to BIGINT

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

        // count_star->alias = "annot";    // Name the count(*) column as "annot"
        select_list.push_back(std::move(count_star));
    }
    
    // Create the LogicalAggregate
    auto aggregate = make_uniq<LogicalAggregate>(group_index, aggregate_index, std::move(select_list));
    
    int group_pos = 0;
    // Add all columns except "annot" to GROUP BY
    for (idx_t i = 0; i < child_bindings.size(); i++) {
        // Skip the annot column for grouping
        if (has_annot_column && i == annot_idx) {
            continue;
        }
        
        // Create GROUP BY expression for this column
        auto col_type = child_types[i];
        
        auto group_expr = make_uniq<BoundColumnRefExpression>(
            // col_name,
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

    // NOTE: Add extra projection to ensure the new aggregate has the correct output types
    idx_t projection_index = binder.GenerateTableIndex();
    vector<unique_ptr<Expression>> proj_expressions;
    // Get the bindings from the aggregate
    auto agg_bindings = aggregate->GetColumnBindings();
    
    // First add all the group columns to the projection
    for (idx_t i = 0; i < aggregate->groups.size(); i++) {
        auto group_binding = agg_bindings[i];
        auto col_type = aggregate->groups[i]->return_type;
    
        auto proj_expr = make_uniq<BoundColumnRefExpression>(
            // aggregate->groups[i]->GetName(),  // Use the original name
            col_type,
            group_binding
        );
    
        proj_expressions.push_back(std::move(proj_expr));
        // Add binding mapping from aggregate to projection
        ColumnBinding old_binding = group_binding;
        ColumnBinding new_binding = ColumnBinding(projection_index, i);
        global_binding_map[old_binding] = new_binding;
    }

    // Then add the aggregate result (with alias "annot")
    if (!aggregate->expressions.empty()) {
        auto agg_binding = agg_bindings[aggregate->groups.size()]; // First binding after groups
        auto agg_type = aggregate->expressions[0]->return_type;
    
        auto proj_expr = make_uniq<BoundColumnRefExpression>(
            "annot",  // Set the alias here, only set alias in projection operator
            agg_type,
            agg_binding
        );
    
        proj_expressions.push_back(std::move(proj_expr));
        // Add binding mapping for the annot column
        ColumnBinding old_binding = agg_binding;
        ColumnBinding new_binding = ColumnBinding(projection_index, aggregate->groups.size());
        global_binding_map[old_binding] = new_binding;
    }

    // Create the projection
    auto projection = make_uniq<LogicalProjection>(
        projection_index,
        std::move(proj_expressions)
    );



    projection->AddChild(std::move(aggregate));
    projection->ResolveOperatorTypes();

    // child_node = std::move(projection);

    return projection;
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
    for (auto& condition : join.conditions) {
        // Update left side of condition
        if (condition.left->type == ExpressionType::BOUND_COLUMN_REF) {
            auto& left_col = condition.left->Cast<BoundColumnRefExpression>();
            left_col.binding = GetUpdatedBinding(left_col.binding);
            left_col.alias.clear();
        }
        
        // Update right side of condition
        if (condition.right->type == ExpressionType::BOUND_COLUMN_REF) {
            auto& right_col = condition.right->Cast<BoundColumnRefExpression>();
            right_col.binding = GetUpdatedBinding(right_col.binding);
            right_col.alias.clear();
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

// NOTE: After RemoveUnusedColumns optimization, the columns are already be pruned, and we should follow the columns in 
// the first child logical_projeciton operator of join operator to prune the aggregation columns and the second projection operator
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
        if (join.children[0]->type == LogicalOperatorType::LOGICAL_PROJECTION) {
            std::cout << "Pruning left projection, aggregation and projection columns with projection_map" << std::endl;
            PruneAggregationWithProjectionMap(join.children[0].get());
            // Clear the join's left projection map
        }
        
        // Process right child
        if (join.children[1]->type == LogicalOperatorType::LOGICAL_PROJECTION) {
            std::cout << "Pruning right projection, aggregation and projection columns with projection_map" << std::endl;
            PruneAggregationWithProjectionMap(join.children[1].get());
        }
        // UpdateJoinConditions(join); The direct connected operator has already been updated

    }

    return op;
}

unique_ptr<LogicalOperator> AggregationPushdown::UpdateAnnotMul(unique_ptr<LogicalOperator> op_node) {
    if (!op_node) {
        op_node;
    }
    
    // First process children recursively
    for (auto& child : op_node->children) {
        child = UpdateAnnotMul(std::move(child));
    }
    
    // Check if this is a projection that might contain a multiplication expression
    if (op_node->type == LogicalOperatorType::LOGICAL_PROJECTION && op_node->children.size() == 1 && 
        (op_node->children[0]->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN ||
         op_node->children[0]->type == LogicalOperatorType::LOGICAL_ASOF_JOIN ||
         op_node->children[0]->type == LogicalOperatorType::LOGICAL_DELIM_JOIN)) {
        
        auto& proj = op_node->Cast<LogicalProjection>();
        auto& join = op_node->children[0]->Cast<LogicalComparisonJoin>();
        
        // Check if the last expression is our multiplication expression named "annot"
        if (!proj.expressions.empty()) {
            auto& last_expr = proj.expressions.back();
            
            if (last_expr->GetName() == "annot" && last_expr->type == ExpressionType::BOUND_FUNCTION) {
                auto& func_expr = last_expr->Cast<BoundFunctionExpression>();
                
                // Verify this is a multiplication function
                if (func_expr.function.name == "*" && func_expr.children.size() == 2) {
                    // Find updated annot bindings in join children
                    bool left_has_annot, right_has_annot;
                    ColumnBinding left_annot, right_annot;
                    LogicalType left_type, right_type;
                    
                    left_has_annot = FindAnnotAttribute(join.children[0].get(), left_annot, left_type);
                    right_has_annot = FindAnnotAttribute(join.children[1].get(), right_annot, right_type);
                    
                    if (left_has_annot && right_has_annot) {
                        // Update the column references in the multiplication expression
                        for (idx_t i = 0; i < func_expr.children.size(); i++) {
                            auto& child = func_expr.children[i];
                            if (child->type == ExpressionType::BOUND_COLUMN_REF) {
                                auto& col_ref = child->Cast<BoundColumnRefExpression>();
                                // First child's binding should be left annot
                                if (i == 0) {
                                    std ::cout << "Updating left child binding" << std::endl;
                                    std::cout << left_annot.ToString() << std::endl;
                                    col_ref.binding = left_annot;
                                }
                                // Second child's binding should be right annot
                                else if (i == 1) {
                                    std::cout << "Updating right child binding" << std::endl;
                                    std::cout << right_annot.ToString() << std::endl;
                                    col_ref.binding = right_annot;
                                }
                            }
                        }
                        
                        std::cout << "Updated multiplication expression bindings for annot" << std::endl;
                    } else {
                        // FIXME: 
                        throw std::runtime_error("Annot attribute not found in join children");
                    }
                }
            }
        }
    }
    return op_node;
}

void AggregationPushdown::PruneAggregationWithProjectionMap(LogicalOperator* op) {
    // Get references to all operators in the chain
    auto &top_proj = op->Cast<LogicalProjection>();
    auto &agg = op->children[0]->Cast<LogicalAggregate>();
    bool has_bottom_proj = (agg.children.size() == 1 && 
                           agg.children[0]->type == LogicalOperatorType::LOGICAL_PROJECTION);
    LogicalProjection* bottom_proj = has_bottom_proj ? 
                                    &agg.children[0]->Cast<LogicalProjection>() : nullptr;

    // Step 1: Analyze top projection to determine which columns to keep in aggregate
    std::unordered_set<idx_t> kept_group_indices; // index need to keep in aggregation
    std::unordered_set<idx_t> kept_agg_indices;
    
    for (auto &expr : top_proj.expressions) {
        if (expr->type == ExpressionType::BOUND_COLUMN_REF) {
            auto &col_ref = expr->Cast<BoundColumnRefExpression>();
            
            // Check if it references a group column
            if (col_ref.binding.table_index == agg.group_index) {
                kept_group_indices.insert(col_ref.binding.column_index);
            }
        }
    }

    // Step 2: Determine which columns from bottom projection are needed
    std::unordered_set<idx_t> needed_bottom_columns;
    
    if (has_bottom_proj) {
        // Check both groups and aggregates in one pass
        for (idx_t i = 0; i < agg.groups.size(); i++) {
            if (kept_group_indices.count(i) > 0 && 
                agg.groups[i]->type == ExpressionType::BOUND_COLUMN_REF) {
                
                auto &col_ref = agg.groups[i]->Cast<BoundColumnRefExpression>();
                if (col_ref.binding.table_index == bottom_proj->table_index) {
                    needed_bottom_columns.insert(col_ref.binding.column_index);
                }
            }
        }
        // Step 3: Prune bottom projection if needed
        if (needed_bottom_columns.size() < bottom_proj->expressions.size()) {
            vector<unique_ptr<Expression>> new_bottom_exprs;
            std::unordered_map<idx_t, idx_t> column_index_map;
            idx_t new_idx = 0;
            
            // Prune and map in a single pass
            for (idx_t i = 0; i < bottom_proj->expressions.size(); i++) {
                if (needed_bottom_columns.count(i) > 0 || i == bottom_proj->expressions.size() - 1) {
                    // Store old → new mapping
                    column_index_map[i] = new_idx++;
                    
                    // Add the expression to keep
                    new_bottom_exprs.push_back(std::move(bottom_proj->expressions[i]));
                    
                    // Update global binding map only for non-annot column
                    if (i != bottom_proj->expressions.size() - 1) {
                        ColumnBinding old_binding(bottom_proj->table_index, i);
                        ColumnBinding new_binding(bottom_proj->table_index, new_idx - 1);
                        global_binding_map[old_binding] = new_binding;
                    }
                }
            }
            
            // Update the bottom projection
            bottom_proj->expressions = std::move(new_bottom_exprs);
            bottom_proj->types.clear();
            for (auto &expr : bottom_proj->expressions) {
                bottom_proj->types.push_back(expr->return_type);
            }
            
            // Update column references in one combined pass through aggregate
            for (auto &group : agg.groups) {
                if (group->type == ExpressionType::BOUND_COLUMN_REF) {
                    auto &col_ref = group->Cast<BoundColumnRefExpression>();
                    if (col_ref.binding.table_index == bottom_proj->table_index) {
                        col_ref.binding = GetUpdatedBinding(col_ref.binding);
                    }
                }
            }
            auto &agg_expr = agg.expressions[0];
            // Handle the SUM function specifically - this is more reliable than general binding updates
            if (agg_expr->type == ExpressionType::BOUND_AGGREGATE) {
                auto &bound_agg = agg_expr->Cast<BoundAggregateExpression>();
                // Check if this is a SUM function
                if (bound_agg.function.name == "sum" && !bound_agg.children.empty()) {
                    std::cout << "Updating SUM function argument" << std::endl;
                    // For each child of the sum function (typically just one argument)
                    for (auto &child : bound_agg.children) {
                        if (child->type == ExpressionType::BOUND_COLUMN_REF) {
                            auto &col_ref = child->Cast<BoundColumnRefExpression>();
                            // If this binding refers to a column in the bottom projection
                            if (col_ref.binding.table_index == bottom_proj->table_index) {
                                // Get the original column index
                                col_ref.binding.column_index = bottom_proj->expressions.size() - 1;
                            }
                        } else if (child->type == ExpressionType::OPERATOR_CAST) {
                            // Handle the case where the child is a cast expression
                            auto &cast_expr = child->Cast<BoundCastExpression>();
                            if (cast_expr.child->type == ExpressionType::BOUND_COLUMN_REF) {
                                auto &col_ref = cast_expr.child->Cast<BoundColumnRefExpression>();
                                if (col_ref.binding.table_index == bottom_proj->table_index) {
                                    col_ref.binding.column_index = bottom_proj->expressions.size() - 1;
                                }
                            }
                        } else if (child->type == ExpressionType::CAST) {
                            // Handle the case where the child is a cast expression
                            auto &cast_expr = child->Cast<BoundCastExpression>();
                            if (cast_expr.child->type == ExpressionType::BOUND_COLUMN_REF) {
                                auto &col_ref = cast_expr.child->Cast<BoundColumnRefExpression>();
                                if (col_ref.binding.table_index == bottom_proj->table_index) {
                                    col_ref.binding.column_index = bottom_proj->expressions.size() - 1;
                                }
                            }
                        } else {
                            throw std::runtime_error("Unsupported aggregate expression type 2 in PruneAggregationWithProjectionMap");
                        }
                    }
                }
            }
            bottom_proj->ResolveOperatorTypes();
        }
    }

    // Step 4: Prune the aggregate in a single pass for groups and expressions
    vector<unique_ptr<Expression>> new_groups;
    vector<unique_ptr<Expression>> new_agg_exprs;
    
    agg.types.clear();
    idx_t new_group_idx = 0;
    
    // Process groups - combine pruning, binding update, and type tracking
    for (idx_t i = 0; i < agg.groups.size(); i++) {
        if (kept_group_indices.count(i) > 0) {
            // Store binding mapping
            ColumnBinding old_binding(agg.group_index, i);
            ColumnBinding new_binding(agg.group_index, new_group_idx++);
            global_binding_map[old_binding] = new_binding;
            
            // Add to types and new groups in one step
            agg.types.push_back(agg.groups[i]->return_type);
            new_groups.push_back(std::move(agg.groups[i]));
        }
    }
    
    // Process aggregate expressions - annot! 
    for (idx_t i = 0; i < agg.expressions.size(); i++) {
        agg.types.push_back(agg.expressions[i]->return_type);
        new_agg_exprs.push_back(std::move(agg.expressions[i]));
    }
    
    // Update the aggregate
    agg.groups = std::move(new_groups);
    agg.expressions = std::move(new_agg_exprs);
    
    // Update grouping sets
    if (!agg.groups.empty()) {
        agg.grouping_sets.clear();
        GroupingSet new_grouping_set;
        for (idx_t i = 0; i < agg.groups.size(); i++) {
            new_grouping_set.insert(i);
        }
        agg.grouping_sets.push_back(std::move(new_grouping_set));
    }
    
    // Step 5: Update top projection references
    for (auto &expr : top_proj.expressions) {
        if (expr->type == ExpressionType::BOUND_COLUMN_REF) {
            auto &col_ref = expr->Cast<BoundColumnRefExpression>();
            col_ref.binding = GetUpdatedBinding(col_ref.binding);
        }
    }
    
    // Update types in one pass
    top_proj.types.clear();
    for (auto &expr : top_proj.expressions) {
        top_proj.types.push_back(expr->return_type);
    }
    
    agg.ResolveOperatorTypes();
    top_proj.ResolveOperatorTypes();
    
    std::cout << "After pruning: Aggregate has " << agg.groups.size() << " groups and " 
              << agg.expressions.size() << " expressions" << std::endl;


    return ;
}

}