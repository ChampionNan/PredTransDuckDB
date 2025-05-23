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
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"


namespace duckdb {
// Aggregation Function Support for SUM
AggregateFunction GetSumAggregate(PhysicalType type);
vector<AggregationPushdown::MinMaxColumnInfo> AggregationPushdown::minmax_columns;
vector<AggregationPushdown::JoinInfo> AggregationPushdown::join_pushdown_info;

// Aggregation Function Support for MIN
AggregateFunction GetMinAggregate(ClientContext &context, const LogicalType &input_type) {
    vector<LogicalType> argument_types = {input_type};
    auto &catalog = Catalog::GetSystemCatalog(context);
    auto entry = catalog.GetEntry(context, CatalogType::AGGREGATE_FUNCTION_ENTRY, DEFAULT_SCHEMA, "min", OnEntryNotFound::RETURN_NULL);
    if (!entry) {
        // Fall back to a basic one if not found
        AggregateFunction min_func("min", argument_types, input_type, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        return min_func;
    }
    auto &func_catalog_entry = entry->Cast<AggregateFunctionCatalogEntry>();
    return func_catalog_entry.functions.GetFunctionByArguments(context, argument_types);
}

// Aggregation Function Support for MAX
AggregateFunction GetMaxAggregate(ClientContext &context, const LogicalType &input_type) {
    vector<LogicalType> argument_types = {input_type};
    auto &catalog = Catalog::GetSystemCatalog(context);
    auto entry = catalog.GetEntry(context, CatalogType::AGGREGATE_FUNCTION_ENTRY, DEFAULT_SCHEMA, "max", OnEntryNotFound::RETURN_NULL);
    if (!entry) {
        // Fall back to a basic one if not found
        AggregateFunction max_func("max", argument_types, input_type, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        return max_func;
    }
    auto &func_catalog_entry = entry->Cast<AggregateFunctionCatalogEntry>();
    return func_catalog_entry.functions.GetFunctionByArguments(context, argument_types);
}


// Part1
unique_ptr<LogicalOperator> AggregationPushdown::Rewrite(unique_ptr<LogicalOperator> op) {
    global_binding_map.clear();
    minmax_columns.clear();
    if (query_type == QueryType::MINMAX_AGGREGATE) {
        StoreMinMaxAggregates(op->children[0].get());
    }
    op = AddAnnotAttributeDFS(std::move(op));
    op = ReplaceRootCountWithSum(std::move(op));
    return op;
}

// Part3
unique_ptr<LogicalOperator> AggregationPushdown::ApplyAgg(unique_ptr<LogicalOperator> op) {
    global_binding_map.clear();
    minmax_columns.clear();
    op = AddAnnotAttributeDFS(std::move(op), true);
    op = ReplaceRootCountWithSum(std::move(op));
    return op;
}

// Part2
unique_ptr<LogicalOperator> AggregationPushdown::UpdateBinding(unique_ptr<LogicalOperator> op) {
    op = PruneAggregation(std::move(op), &AggregationPushdown::PruneAggregationWithProjectionMap);
    op = UpdateAnnotMul(std::move(op));
    return op;
}

void AggregationPushdown::StoreMinMaxAggregates(LogicalOperator* op) {
    auto& agg = op->Cast<LogicalAggregate>();
    for (idx_t i = 0; i < agg.expressions.size(); i++) {
        auto& expr = agg.expressions[i];
        
        if (expr->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
            auto& bound_agg = expr->Cast<BoundAggregateExpression>();
            
            // Check if it's a MIN or MAX function
            if (bound_agg.function.name == "min" || bound_agg.function.name == "max") {
                // Create a new info entry
                MinMaxColumnInfo info;
                
                if (bound_agg.children.size() > 0 && 
                    bound_agg.children[0]->type == ExpressionType::BOUND_COLUMN_REF) {
                    // Extract the actual column binding from the expression
                    auto& col_ref = bound_agg.children[0]->Cast<BoundColumnRefExpression>();
                    info.original_binding = col_ref.binding;
                    info.binding = col_ref.binding;
                } else {
                    throw std::runtime_error("Complex MIN/MAX aggregate expression case");
                }
                
                // Store the function name
                info.function_name = bound_agg.function.name;
                
                // Add to our tracking vector
                minmax_columns.push_back(info);
                // std::cout << "Stored MIN/MAX aggregate: " << info.ToString() << std::endl;
            }
        }
    }
}

unique_ptr<LogicalOperator> AggregationPushdown::ReplaceRootCountWithSum(unique_ptr<LogicalOperator> op_node) {
    if (!op_node) {
        return op_node;
    }

    // Check if this is the root aggregate with COUNT(*)
    if (query_type == QueryType::COUNT_STAR) {
        auto &agg = op_node->children[0]->Cast<LogicalAggregate>();
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
    } else if (query_type == QueryType::MINMAX_AGGREGATE) {
        auto &agg = op_node->children[0]->Cast<LogicalAggregate>();
    
        // First, get the child bindings for reference
        auto child_bindings = agg.children[0]->GetColumnBindings();
        auto child_types = agg.children[0]->types;
        vector<unique_ptr<Expression>> agg_expressions;

        for (const auto& info : minmax_columns) {
            ColumnBinding original = info.original_binding;
            ColumnBinding new_binding = info.binding;

            for (idx_t i = 0; i < child_bindings.size(); i++) {
                if (child_bindings[i] == new_binding) {
                    // std::cout << "Replace final root binding: " << child_bindings[i].ToString() << " -> " << new_binding.ToString() << std::endl;
                    vector<unique_ptr<Expression>> minmax_list;
                    auto col_ref = make_uniq<BoundColumnRefExpression>(child_types[i], child_bindings[i]);
                    minmax_list.push_back(std::move(col_ref));
                    if (info.function_name == "min") {
                        auto min_func = GetMinAggregate(context, child_types[i]);
                        FunctionBinder function_binder(context);
                        auto min_expr = function_binder.BindAggregateFunction(
                            min_func, 
                            std::move(minmax_list), 
                            nullptr, 
                            AggregateType::NON_DISTINCT
                        );
                        agg_expressions.push_back(std::move(min_expr));
                    } else if (info.function_name == "max") {
                        auto max_func = GetMaxAggregate(context, child_types[i]);
                        FunctionBinder function_binder(context);
                        auto max_expr = function_binder.BindAggregateFunction(
                            max_func, 
                            std::move(minmax_list), 
                            nullptr, 
                            AggregateType::NON_DISTINCT
                        );
                        agg_expressions.push_back(std::move(max_expr));
                    } else {
                        throw std::runtime_error("Upsupport function name: " + info.function_name);
                    }
                }
            }
        }
        if (!agg_expressions.empty()) {
            agg.expressions = std::move(agg_expressions);        
            agg.ResolveOperatorTypes();
        }
        auto &proj = op_node->Cast<LogicalProjection>();
        vector<unique_ptr<Expression>> new_proj_exprs;
        auto agg_bindings = agg.GetColumnBindings();
        for (idx_t i = 0; i < agg_bindings.size(); i++) {
            // Create binding from the aggregate
            // Create a reference expression to the aggregate output
            auto ref_expr = make_uniq<BoundColumnRefExpression>(
                agg.expressions[i]->return_type,
                agg_bindings[i]
            );
            
            // Add to new projection expressions
            new_proj_exprs.push_back(std::move(ref_expr));
        }
        // Replace the projection's expressions
        proj.expressions = std::move(new_proj_exprs);
        proj.ResolveOperatorTypes();
        return op_node;

    }
    else {
        throw std::runtime_error("Not implemented");
    }
}

bool AggregationPushdown::CheckPKFK(LogicalOperator* op) {
    // TODO: Add filter_op check
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
                        // std::cout << "Found unique constraint on column: " << get_op.names[col_idx] << std::endl;
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
                            // std::cout << "Found column in multi-column unique constraint: " << column_name << std::endl;
                            return true;
                        }
                    }
                }
            }
        }
    }
    
    return false; // No unique key found
}

unique_ptr<LogicalOperator> AggregationPushdown::AddAnnotAttributeDFS(unique_ptr<LogicalOperator> op_node, bool applyFlag) {
    static int current_join_id = 0;
    if (!op_node) {
        return op_node;
    }
    for (idx_t i = 0; i < op_node->children.size(); i++) {
        op_node->children[i] = AddAnnotAttributeDFS(std::move(op_node->children[i]), applyFlag);
    }
    // Special handling for join operators
    if (op_node->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN ||
        op_node->type == LogicalOperatorType::LOGICAL_ASOF_JOIN ||
        op_node->type == LogicalOperatorType::LOGICAL_DELIM_JOIN) {
        
        auto &join = op_node->Cast<LogicalComparisonJoin>();
        
        // join.children[0] = AddAnnotAttributeDFS(std::move(join.children[0]));
        // join.children[1] = AddAnnotAttributeDFS(std::move(join.children[1]));

        bool addLeft = true;
        bool addRight = true;
        
        // Check if any column from left child is a unique key
        if (CheckPKFK(join.children[0].get())) {
            addLeft = false;
            // std::cout << "Left child has unique key, skipping annot" << std::endl;
        }
        
        // Check if any column from right child is a unique key
        if (CheckPKFK(join.children[1].get())) {
            addRight = false;
            // std::cout << "Right child has unique key, skipping annot" << std::endl;
        }

        if (applyFlag) {
            if (current_join_id < join_pushdown_info.size()) {
                bool left_pushdown = join_pushdown_info[current_join_id].left_pushdown;
                bool right_pushdown = join_pushdown_info[current_join_id].right_pushdown;
                current_join_id++;
                if (!left_pushdown) {
                    addLeft = false;
                }
                if (!right_pushdown) {
                    addRight = false;
                }
            }
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
        if (query_type == QueryType::COUNT_STAR) {
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
    
                vector<ColumnBinding> bindings_to_exclude = {left_annot, right_annot};
                // NOTE: for minman_aggregate, we need to include all annot columns, and no exclude
                auto projection = AddProjectionWithAnnot(std::move(op_node), std::move(mult_expr), "annot", bindings_to_exclude);
                return projection;
            }
            /*
            else if (left_has_annot) {
                // Case 2: Only left child has annot
                auto left_ref = make_uniq<BoundColumnRefExpression>(left_type, left_annot);
                // NOTE: For single side, we just add to output
                auto projection = AddProjectionWithAnnot(std::move(op_node), std::move(left_ref), "annot", {left_annot});
                return projection;
            } else if (right_has_annot) {
                // Case 3: Only right child has annot
                auto right_ref = make_uniq<BoundColumnRefExpression>(right_type, right_annot);
                auto projection = AddProjectionWithAnnot(std::move(op_node), std::move(right_ref), "annot", {right_annot});
                return projection;
            }*/
            else {
                // Case 4: Neither child has an annotation column
                // auto projection = AddProjectionWithAnnot(std::move(op_node), nullptr, "", {});
                return std::move(op_node);
            }
        } else if (query_type == QueryType::MINMAX_AGGREGATE) {
            // TODO: FIX, Update remove this
            //std::cout << "AddAnnotAttributeDFS: MINMAX_AGGREGATE" << std::endl;
            // op_node->Print();
            vector<ColumnBinding> left_annots, right_annots;
            vector<LogicalType> left_types, right_types;
            vector<ColumnBinding> bindings_to_exclude;
            vector<unique_ptr<Expression>> annot_exprs;

            bool left_has_annots = FindAllAnnotAttributes(join.children[0].get(), left_annots, left_types);
            bool right_has_annots = FindAllAnnotAttributes(join.children[1].get(), right_annots, right_types);

            if (left_has_annots) {
                for (idx_t i = 0; i < left_annots.size(); i++) {
                    auto left_ref = make_uniq<BoundColumnRefExpression>(left_types[i], left_annots[i]);
                    annot_exprs.push_back(std::move(left_ref));
                    bindings_to_exclude.push_back(left_annots[i]);
                }
            }
            if (right_has_annots) {
                for (idx_t i = 0; i < right_annots.size(); i++) {
                    auto right_ref = make_uniq<BoundColumnRefExpression>(right_types[i], right_annots[i]);
                    annot_exprs.push_back(std::move(right_ref));
                    bindings_to_exclude.push_back(right_annots[i]);
                }
            }

            if (left_has_annots || right_has_annots) {
                auto projection = AddProjectionWithAnnot(std::move(op_node), std::move(annot_exprs), "annot", bindings_to_exclude);
                return projection;
            } else {
                // No annot columns found, return the original operator
                return std::move(op_node);
            }
            
        } else {
            throw std::runtime_error("Not implemented in AddAnnotAttributeDFS");
        }
    } else {
        return std::move(op_node);
    }
}

// Returns all annotation columns from an operator
bool AggregationPushdown::FindAllAnnotAttributes(LogicalOperator* op, vector<ColumnBinding>& annot_binding, vector<LogicalType>& annot_type) {
    // Check in expressions for special operators
    if (op->type == LogicalOperatorType::LOGICAL_PROJECTION) {
        auto& proj = op->Cast<LogicalProjection>();
        for (idx_t i = 0; i < proj.expressions.size(); i++) {
            auto& expr = proj.expressions[i];
            // Regular annot column
            if (expr->GetName() == "annot") {
                // std::cout << "Found annot in projection at index " << i << std::endl;
                annot_binding.push_back(ColumnBinding(proj.table_index, i));
                annot_type.push_back(expr->return_type);
            }
        }
        return annot_binding.size() > 0;
    } else if (op->type == LogicalOperatorType::LOGICAL_GET || op->type == LogicalOperatorType::LOGICAL_FILTER) {
        return false;
    } else if (op->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN ||
               op->type == LogicalOperatorType::LOGICAL_ASOF_JOIN ||
               op->type == LogicalOperatorType::LOGICAL_DELIM_JOIN ) {
        auto& join = op->Cast<LogicalComparisonJoin>();
        bool found_any = false;

        if (FindAllAnnotAttributes(join.children[0].get(), annot_binding, annot_type)) {
            found_any = true;
        }

        if (FindAllAnnotAttributes(join.children[1].get(), annot_binding, annot_type)) {
            found_any = true;
        }
        
        return found_any;
    } else {
        throw std::runtime_error("Unsupported operator type for FindAllAnnotAttributes");
    }
}

bool AggregationPushdown::FindAnnotAttribute(LogicalOperator* op, ColumnBinding& annot_binding, LogicalType& annot_type) {
    // Check in expressions for special operators
    if (op->type == LogicalOperatorType::LOGICAL_PROJECTION) {
        auto& proj = op->Cast<LogicalProjection>();
        for (idx_t i = 0; i < proj.expressions.size(); i++) {
            auto& expr = proj.expressions[i];
            if (expr->GetName() == "annot") {
                // std::cout << "Found annot in projection!" << std::endl;
                annot_binding = ColumnBinding(proj.table_index, i);
                annot_type = expr->return_type;
                return true;
            }
        }
    } else if (op->type == LogicalOperatorType::LOGICAL_GET || op->type == LogicalOperatorType::LOGICAL_FILTER) {
        return false;
    } else if (op->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN ||
               op->type == LogicalOperatorType::LOGICAL_ASOF_JOIN ||
               op->type == LogicalOperatorType::LOGICAL_DELIM_JOIN ) {
        auto& join = op->Cast<LogicalComparisonJoin>();
        bool found_any = false;

        if (FindAnnotAttribute(join.children[0].get(), annot_binding, annot_type)) {
            found_any = true;
            if (found_any) return true;
        }

        if (FindAnnotAttribute(join.children[1].get(), annot_binding, annot_type)) {
            found_any = true;
        }
        
        return found_any;
    } else {
        throw std::runtime_error("Unsupported operator type for FindAnnotAttribute");
    }

    return false;
}

// Helper function to get all annot column indices with proper offsets from an operator
void AggregationPushdown::GetAnnotColumnBindingsIdx(LogicalOperator* op, vector<idx_t>& annot_indices) {
    if (!op) return;
    
    // Handle different operator types appropriately
    if (op->type == LogicalOperatorType::LOGICAL_PROJECTION) {
        auto& proj = op->Cast<LogicalProjection>();
        for (idx_t i = 0; i < proj.expressions.size(); i++) {
            if (proj.expressions[i]->GetName() == "annot") {
                annot_indices.push_back(i);
            }
        }
    } 
    else if (op->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN ||
             op->type == LogicalOperatorType::LOGICAL_ASOF_JOIN ||
             op->type == LogicalOperatorType::LOGICAL_DELIM_JOIN) {
        auto& join = op->Cast<LogicalComparisonJoin>();
        
        // First get annot indices from left child
        vector<idx_t> left_annot_indices;
        GetAnnotColumnBindingsIdx(join.children[0].get(), left_annot_indices);
        
        // Add left child indices directly (no offset needed)
        annot_indices.insert(annot_indices.end(), left_annot_indices.begin(), left_annot_indices.end());
        
        // Then get annot indices from right child
        vector<idx_t> right_annot_indices;
        GetAnnotColumnBindingsIdx(join.children[1].get(), right_annot_indices);
        
        // Add offset to right child indices (offset = number of columns in left child)
        idx_t left_column_count = join.children[0]->types.size();
        for (auto right_idx : right_annot_indices) {
            annot_indices.push_back(left_column_count + right_idx);
        }
    }
    else if (op->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
        auto& agg = op->Cast<LogicalAggregate>();
        
        // First check aggregate expressions (these come after groups in the output)
        for (idx_t i = 0; i < agg.expressions.size(); i++) {
            // FIXME: aggregation alias is not annot here
            if (agg.expressions[i]->GetName() == "annot") {
                // Offset by the number of group columns
                annot_indices.push_back(agg.groups.size() + i);
            }
        }
    }
    else if (!op->children.empty()) {
        // For other operators, recursively check children
        for (auto& child : op->children) {
            GetAnnotColumnBindingsIdx(child.get(), annot_indices);
        }
    }
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
        // NOTE: minmax_aggregate, we need to add all annot columns
        projection_expressions.push_back(std::move(col_ref));
        UpdateBindingMap(bindings[i], ColumnBinding(projection_index, projection_expressions.size()-1));
    }
    
    // Then add the annot expression
    if (annot_expr) {
        annot_expr->alias = name;
        if (annot_expr->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
            auto& col_ref = annot_expr->Cast<BoundColumnRefExpression>();
            UpdateBindingMap(col_ref.binding, ColumnBinding(projection_index, projection_expressions.size()));
        } else if (annot_expr->GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
            auto& func_expr = annot_expr->Cast<BoundFunctionExpression>();
            for (auto& child : func_expr.children) {
                if (child->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
                    auto& child_ref = child->Cast<BoundColumnRefExpression>();
                    UpdateBindingMap(child_ref.binding, ColumnBinding(projection_index, projection_expressions.size()));
                }
            }
        } else {
            throw std::runtime_error("Unsupported expression type for annot");
        }
        projection_expressions.push_back(std::move(annot_expr));
    }
    
    auto projection = make_uniq<LogicalProjection>(projection_index, std::move(projection_expressions));
    
    // Add the original operator as child
    projection->AddChild(std::move(op));
    projection->ResolveOperatorTypes();
    UpdateMinMax();

    // Return the new projection
    return projection;
}

unique_ptr<LogicalOperator> AggregationPushdown::AddProjectionWithAnnot(unique_ptr<LogicalOperator> op, vector<unique_ptr<Expression>> annot_exprs, string name, vector<ColumnBinding> bindings_to_exclude) {       // Flag to control annotation placement
    // Get bindings from operator
    auto bindings = op->GetColumnBindings();
    // Create expressions for projection
    vector<unique_ptr<Expression>> projection_expressions;
    // Create and return the projection
    idx_t projection_index = binder.GenerateTableIndex();
    vector<idx_t> annot_indices;

    if (annot_exprs.size() > 0) {
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
            UpdateBindingMap(bindings[i], ColumnBinding(projection_index, projection_expressions.size()-1));
        }
    
        // Then add all annotation expressions
        for (idx_t i = 0; i < annot_exprs.size(); i++) {
            if (annot_exprs[i]) {
                annot_exprs[i]->alias = name;
                if (annot_exprs[i]->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
                    auto& col_ref = annot_exprs[i]->Cast<BoundColumnRefExpression>();
                    UpdateBindingMap(col_ref.binding, ColumnBinding(projection_index, projection_expressions.size()));
                } 
                else if (annot_exprs[i]->GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
                    // For function expressions, update bindings in child expressions
                    auto& func_expr = annot_exprs[i]->Cast<BoundFunctionExpression>();
                    for (auto& child : func_expr.children) {
                        if (child->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
                            auto& child_ref = child->Cast<BoundColumnRefExpression>();
                            UpdateBindingMap(child_ref.binding, ColumnBinding(projection_index, projection_expressions.size()));
                        }
                    }
                } else {
                    throw std::runtime_error("Unsupported expression type for annot");
                }
                projection_expressions.push_back(std::move(annot_exprs[i]));
            }
        }
    } else {
        // Reorder the binding, put annot at last, which makes it the same as aggregation operator in dynamic
        for (idx_t i = 0; i < bindings.size(); i++) {
            bool should_exclude = false;
            for (const auto& info : minmax_columns) {
                if (info.binding == bindings[i]) {
                    should_exclude = true;
                    annot_indices.push_back(i);
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
            UpdateBindingMap(bindings[i], ColumnBinding(projection_index, projection_expressions.size()-1));
        }
        for (auto i : annot_indices) {
            auto col_ref = make_uniq<BoundColumnRefExpression>(
                name,
                op->types[i],
                bindings[i]
            );
            projection_expressions.push_back(std::move(col_ref));
            UpdateBindingMap(bindings[i], ColumnBinding(projection_index, projection_expressions.size()-1));
        }
    }
    
    auto projection = make_uniq<LogicalProjection>(projection_index, std::move(projection_expressions));
    
    // Add the original operator as child
    projection->AddChild(std::move(op));
    projection->ResolveOperatorTypes();
    UpdateMinMax();

    // Return the new projection
    return projection;
}

unique_ptr<LogicalOperator> AggregationPushdown::CreateDynamicAggregate(unique_ptr<LogicalOperator> child_node) {
    // Extract child column information before we modify it
    // std::cout << "CreateDynamicAggregate" << std::endl;
    // child_node->Print();
    vector<ColumnBinding> child_bindings = child_node->GetColumnBindings();
    child_node->ResolveOperatorTypes();
    vector<LogicalType> child_types = child_node->types;
    
    vector<idx_t> annot_indices;
    string annot_name = "annot";
    
    if (child_node->type != LogicalOperatorType::LOGICAL_GET && child_node->type != LogicalOperatorType::LOGICAL_FILTER) {
        GetAnnotColumnBindingsIdx(child_node.get(), annot_indices);
    } else if (child_node->type == LogicalOperatorType::LOGICAL_FILTER &&
               child_node->children.size() == 1 && child_node->children[0]->type == LogicalOperatorType::LOGICAL_GET) {
        return child_node;
    } else {
        int agg_num = 0;
        for (idx_t i = 0; i < child_bindings.size(); i++) {
            for (const auto& info : minmax_columns) {
                if (info.binding == child_bindings[i]) {
                    agg_num ++;
                    break;
                }
            }
        }
        if (child_bindings.size() - agg_num > GROUP_BY_NUM) {
            return child_node; // Too many columns, don't aggregate
        }
    }

    // Get next available table indices
    idx_t group_index = binder.GenerateTableIndex();
    idx_t aggregate_index = binder.GenerateTableIndex();
    
    // Create the aggregate expression (COUNT(*) or SUM(annot))
    vector<unique_ptr<Expression>> select_list;
    
    int agg_pos = 0;
    if (annot_indices.size() > 0) {
        if (query_type == QueryType::COUNT_STAR) {
            // std::cout << "Create SUM(annot)!" << std::endl;
            vector<unique_ptr<Expression>> sum_args;
            auto annot_idx = annot_indices[0];
            
            auto annot_col_ref = make_uniq<BoundColumnRefExpression>(
                child_types[annot_idx],
                child_bindings[annot_idx]
            );
            
            sum_args.push_back(std::move(annot_col_ref));
            
            AggregateFunction sum_function = GetSumAggregate(child_types[annot_idx].InternalType());
            if (sum_function.name.empty()) {
                sum_function.name = "sum";
            }

            FunctionBinder function_binder(context);
            auto sum_expr = function_binder.BindAggregateFunction(
                sum_function, 
                std::move(sum_args), 
                nullptr, 
                AggregateType::NON_DISTINCT
            );
            select_list.push_back(std::move(sum_expr));
        } else if (query_type == QueryType::MINMAX_AGGREGATE) {
            for (idx_t i = 0; i < annot_indices.size(); i++) {
                auto annot_idx = annot_indices[i];
                string agg_function_name;
                vector<unique_ptr<Expression>> sum_args;

                for (const auto& info : minmax_columns) {
                    if (info.binding == child_bindings[annot_idx]) {
                        // std::cout << "Found minmax column: " << child_bindings[annot_idx].ToString() << std::endl;
                        agg_function_name = info.function_name;
                        break;
                    }
                }

                vector<unique_ptr<Expression>> minmax_args;
                // Create reference to annot column
                auto annot_col_ref = make_uniq<BoundColumnRefExpression>(child_types[annot_idx], child_bindings[annot_idx]);
                FunctionBinder function_binder(context);

                sum_args.push_back(std::move(annot_col_ref));

                if (agg_function_name == "min") {
                    auto agg_function = GetMinAggregate(context, child_types[annot_idx]);
                    auto sum_expr = function_binder.BindAggregateFunction(
                        agg_function, 
                        std::move(sum_args), 
                        nullptr, 
                        AggregateType::NON_DISTINCT
                    );
                    select_list.push_back(std::move(sum_expr));
                } else if (agg_function_name == "max") {
                    auto agg_function = GetMaxAggregate(context, child_types[annot_idx]);
                    auto sum_expr = function_binder.BindAggregateFunction(
                        agg_function, 
                        std::move(sum_args), 
                        nullptr, 
                        AggregateType::NON_DISTINCT
                    );
                    select_list.push_back(std::move(sum_expr));
                }
                UpdateBindingMap(child_bindings[annot_idx], ColumnBinding(aggregate_index, agg_pos++));
            }
        }
    } else {
        if (query_type == QueryType::COUNT_STAR) {
            // std::cout << "Create COUNT(*)!" << std::endl;
            vector<unique_ptr<Expression>> empty_args;
            auto count_star_fun = CountStarFun::GetFunction();
            if (count_star_fun.name.empty()) {
                count_star_fun.name = "count_star";
            }
    
            FunctionBinder function_binder(context);
            auto count_star = function_binder.BindAggregateFunction(
                count_star_fun, 
                {}, 
                nullptr, 
                AggregateType::NON_DISTINCT
            );
            select_list.push_back(std::move(count_star));
        } else if (query_type == QueryType::MINMAX_AGGREGATE) {
            for (idx_t i = 0; i < child_bindings.size(); i++) {
                for (const auto& info : minmax_columns) {
                    if (info.binding == child_bindings[i]) {
                        vector<unique_ptr<Expression>> sum_args;

                        annot_indices.push_back(i);
                        auto annot_col_ref = make_uniq<BoundColumnRefExpression>(child_types[i], child_bindings[i]);
                        FunctionBinder function_binder(context);
                        
                        sum_args.push_back(std::move(annot_col_ref));

                        if (info.function_name == "min") {
                            auto agg_function = GetMinAggregate(context, child_types[i]);
                            auto agg_expr = function_binder.BindAggregateFunction(
                                agg_function, 
                                std::move(sum_args), 
                                nullptr, 
                                AggregateType::NON_DISTINCT
                            );
                            select_list.push_back(std::move(agg_expr));

                        } else if (info.function_name == "max") {
                            auto agg_function = GetMaxAggregate(context, child_types[i]);
                            auto agg_expr = function_binder.BindAggregateFunction(
                                agg_function, 
                                std::move(sum_args), 
                                nullptr, 
                                AggregateType::NON_DISTINCT
                            );
                            select_list.push_back(std::move(agg_expr));
                        }
                        UpdateBindingMap(child_bindings[i], ColumnBinding(aggregate_index, agg_pos++));
                    }
                }

            }
        }
    }
    
    // Create the LogicalAggregate
    auto aggregate = make_uniq<LogicalAggregate>(group_index, aggregate_index, std::move(select_list));

    int group_pos = 0;
    for (idx_t i = 0; i < child_bindings.size(); i++) {
        // Skip any annotation columns for grouping
        bool is_annot_column = false;
        for (const auto& annot_idx : annot_indices) {
            if (i == annot_idx) {
                is_annot_column = true;
                break;
            }
        }
        
        if (is_annot_column) {
            continue; // Skip this column for grouping
        }
        
        // Create GROUP BY expression for this column
        auto col_type = child_types[i];
        
        auto group_expr = make_uniq<BoundColumnRefExpression>(
            col_type,
            child_bindings[i]
        );
        
        aggregate->groups.push_back(std::move(group_expr));
        UpdateBindingMap(child_bindings[i], ColumnBinding(group_index, group_pos++));
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
        UpdateBindingMap(group_binding, ColumnBinding(projection_index, i));
    }

    if (!aggregate->expressions.empty()) {
        for (idx_t i = 0; i < aggregate->expressions.size(); i++) {
            // Get the correct aggregate binding - groups come first in the binding list, then aggregates
            auto agg_binding = ColumnBinding(aggregate->aggregate_index, i);
            
            auto proj_expr = make_uniq<BoundColumnRefExpression>(
                "annot",
                aggregate->expressions[i]->return_type,
                agg_binding
            );
            proj_expressions.push_back(std::move(proj_expr));
            
            // Update the binding map with the correct index in the projection
            UpdateBindingMap(agg_binding, ColumnBinding(projection_index, i + aggregate->groups.size()));
        }
    }

    // Create the projection
    auto projection = make_uniq<LogicalProjection>(
        projection_index,
        std::move(proj_expressions)
    );

    projection->AddChild(std::move(aggregate));
    projection->ResolveOperatorTypes();
    UpdateMinMax();

    return projection;
}

void AggregationPushdown::UpdateMinMax() {
    if (query_type == QueryType::MINMAX_AGGREGATE) {
        // Update the minmax_columns with the new bindings
        for (auto& info : minmax_columns) {
            ColumnBinding new_binding = GetUpdatedBinding(info.binding);
            if (info.binding != new_binding) {
                // std::cout << "Updated min/max binding: " << info.binding.ToString() << " → " << new_binding.ToString() << std::endl;
                info.binding = new_binding;
            }
        }
    }
}

// Updates the global binding map with a new binding
void AggregationPushdown::UpdateBindingMapOnce(const ColumnBinding old_binding, const ColumnBinding new_binding) {
    if (old_binding == new_binding) {
        return; // No need to update if they're the same
    }
    
    global_binding_map[old_binding] = new_binding;
    
    // std::cout << "Updated binding: " << old_binding.ToString() << " → " << new_binding.ToString() << std::endl;
}

// Updates the global binding map with a new binding
void AggregationPushdown::UpdateBindingMap(const ColumnBinding old_binding, const ColumnBinding new_binding) {
    if (old_binding == new_binding) {
        return; // No need to update if they're the same
    }
    
    // 1. First add the direct mapping
    global_binding_map[old_binding] = new_binding;
    
    // 2. Then update any keys that map to old_binding to point to new_binding
    for (auto& entry : global_binding_map) {
        if (entry.first != old_binding && entry.second == old_binding) {
            entry.second = new_binding;
        }
    }
    
    // qstd::cout << "Updated binding: " << old_binding.ToString() << " → " << new_binding.ToString() << std::endl;
}

ColumnBinding AggregationPushdown::GetUpdatedBindingOnce(const ColumnBinding& original) {
    // Only do a single lookup in the map instead of following the chain
    if (global_binding_map.count(original) > 0) {
        auto result = global_binding_map[original];
        // std::cout << "GetUpdatedBinding: " << original.ToString() << " → " << result.ToString() << std::endl;
        return result;
    }
    
    return original;
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
    // std::cout << "GetUpdatedBinding: " << original.ToString() << " → " << current.ToString() << std::endl;
    return current;
}

void AggregationPushdown::UpdateJoinConditions(LogicalComparisonJoin& join) {
    // Process each child separately
    for (auto& condition : join.conditions) {
        // condition.old_left = condition.left->Copy();
        // condition.old_right = condition.right->Copy();
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

/*
void AggregationPushdown::ResetJoinCondition(JoinCondition &condition, JoinSide side) {
    if (side == JoinSide::LEFT && condition.left->type == ExpressionType::BOUND_COLUMN_REF) {
        condition.left = condition.old_left->Copy();
        condition.left->alias.clear();
    }
    if (side == JoinSide::RIGHT && condition.right->type == ExpressionType::BOUND_COLUMN_REF) {
        condition.right = condition.old_right->Copy();
        condition.right->alias.clear();
    }
}*/

// NOTE: After RemoveUnusedColumns optimization, the columns are already be pruned, and we should follow the columns in 
// the first child logical_projeciton operator of join operator to prune the aggregation columns and the second projection operator
unique_ptr<LogicalOperator> AggregationPushdown::PruneAggregation(unique_ptr<LogicalOperator> op, AggOptFunc func) {
    if (!op) {
        return op;
    }
    
    for (idx_t i = 0; i < op->children.size(); i++) {
        op->children[i] = PruneAggregation(std::move(op->children[i]), func);
    }

    // First process this operator if it's a join
    if (op->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN ||
        op->type == LogicalOperatorType::LOGICAL_ASOF_JOIN ||
        op->type == LogicalOperatorType::LOGICAL_DELIM_JOIN) {

        // std::cout << "PruneAggregation" << std::endl;
        // op->Print();
        
        auto &join = op->Cast<LogicalComparisonJoin>();
    
        if (join.children[0]->type == LogicalOperatorType::LOGICAL_PROJECTION) {
            join.children[0] = (this->*func)(std::move(join.children[0]));
        }
        if (join.children[1]->type == LogicalOperatorType::LOGICAL_PROJECTION) {
            join.children[1] = (this->*func)(std::move(join.children[1]));
        }
    }

    return op;
}

// Update annot1 * annot2 to the correct column references, only complex function can't change binding automatically
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
        // TODO: Here, only consider the last expression COUNT(*), we need to check all aggregation expressions
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
                                    // std ::cout << "Updating left child binding" << std::endl;
                                    // std::cout << left_annot.ToString() << std::endl;
                                    col_ref.binding = left_annot;
                                }
                                // Second child's binding should be right annot
                                else if (i == 1) {
                                    // std::cout << "Updating right child binding" << std::endl;
                                    // std::cout << right_annot.ToString() << std::endl;
                                    col_ref.binding = right_annot;
                                }
                            }
                        }
                        // std::cout << "Updated multiplication expression bindings for annot" << std::endl;
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

unique_ptr<LogicalOperator> AggregationPushdown::PruneAggregationWithProjectionMap(unique_ptr<LogicalOperator> op) {
    // Get references to all operators in the chain
    // std::cout << "PruneAggregationWithProjectionMap" << std::endl;
    // op->Print();
    auto &top_proj = op->Cast<LogicalProjection>();
    bool has_middle_agg = (op->children.size() == 1 && op->children[0]->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY);
    if (!has_middle_agg) {
        return std::move(op);
    }

    auto& agg = op->children[0]->Cast<LogicalAggregate>();
    bool has_bottom_proj = (agg.children.size() == 1 && agg.children[0]->type == LogicalOperatorType::LOGICAL_PROJECTION);
    LogicalProjection* bottom_proj = has_bottom_proj ? &agg.children[0]->Cast<LogicalProjection>() : nullptr;

    

    // Nothing to prune
    if (top_proj.GetColumnBindings().size() == agg.GetColumnBindings().size()) {
        return std::move(op);
    }

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
        for (idx_t i = 0; i < agg.expressions.size(); i++) {
            auto &agg_expr = agg.expressions[i];
                // Handle the SUM function specifically - this is more reliable than general binding updates
                if (agg_expr->type == ExpressionType::BOUND_AGGREGATE) {
                    auto &bound_agg = agg_expr->Cast<BoundAggregateExpression>();
                    for (auto &child : bound_agg.children) {
                        if (child->type == ExpressionType::BOUND_COLUMN_REF) {
                            auto &col_ref = child->Cast<BoundColumnRefExpression>();
                            // If this binding refers to a column in the bottom projection
                            if (col_ref.binding.table_index == bottom_proj->table_index) {
                                needed_bottom_columns.insert(col_ref.binding.column_index);
                            }
                        }
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
                if (needed_bottom_columns.count(i) > 0 || bottom_proj->expressions[i]->GetName() == "annot") {
                    // Store old → new mapping
                    column_index_map[i] = new_idx++;
                    
                    // Add the expression to keep
                    auto &expr = bottom_proj->expressions[i];
                    if (expr->type == ExpressionType::BOUND_COLUMN_REF) {
                        auto &col_ref = expr->Cast<BoundColumnRefExpression>();
                        col_ref.binding = GetUpdatedBindingOnce(col_ref.binding);
                    } else if (expr->type == ExpressionType::BOUND_FUNCTION) {
                        auto &func_expr = expr->Cast<BoundFunctionExpression>();
                        for (auto &child : func_expr.children) {
                            if (child->type == ExpressionType::BOUND_COLUMN_REF) {
                                auto &col_ref = child->Cast<BoundColumnRefExpression>();
                                col_ref.binding = GetUpdatedBindingOnce(col_ref.binding);
                            }
                        }
                    }
                    new_bottom_exprs.push_back(std::move(expr));
                    
                    // Update global binding map only for non-annot column, here, only prune child or comparion extra column, no need to update root min/max binding
                    UpdateBindingMapOnce(ColumnBinding(bottom_proj->table_index, i), ColumnBinding(bottom_proj->table_index, new_idx - 1));
                }
            }
            
            // Update the bottom projection
            bottom_proj->expressions = std::move(new_bottom_exprs);
            bottom_proj->ResolveOperatorTypes();
            
            // Update column references in one combined pass through aggregate
            for (auto &group : agg.groups) {
                if (group->type == ExpressionType::BOUND_COLUMN_REF) {
                    auto &col_ref = group->Cast<BoundColumnRefExpression>();
                    if (col_ref.binding.table_index == bottom_proj->table_index) {
                        col_ref.binding = GetUpdatedBindingOnce(col_ref.binding);
                    }
                }
            }
            for (idx_t i = 0; i < agg.expressions.size(); i++) {
                auto &agg_expr = agg.expressions[i];
                // Handle the SUM function specifically - this is more reliable than general binding updates
                if (agg_expr->type == ExpressionType::BOUND_AGGREGATE) {
                    auto &bound_agg = agg_expr->Cast<BoundAggregateExpression>();
                    // Check if this is a SUM function
                    if (bound_agg.function.name == "sum" && !bound_agg.children.empty()) {
                        // std::cout << "Updating SUM function argument" << std::endl;
                        // For each child of the sum function (typically just one argument)
                        for (auto &child : bound_agg.children) {
                            if (child->type == ExpressionType::BOUND_COLUMN_REF) {
                                auto &col_ref = child->Cast<BoundColumnRefExpression>();
                                // If this binding refers to a column in the bottom projection
                                if (col_ref.binding.table_index == bottom_proj->table_index) {
                                    // Get the original column index
                                    // NOTE: Fix here, we assume the last to be annot column, actually only relative offset change, the order didn't change, size() - k
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
                    } else if ((bound_agg.function.name == "min" || bound_agg.function.name == "max") && !bound_agg.children.empty()) {
                        for (auto &child : bound_agg.children) {
                            if (child->type == ExpressionType::BOUND_COLUMN_REF) {
                                auto &col_ref = child->Cast<BoundColumnRefExpression>();
                                col_ref.binding = GetUpdatedBindingOnce(col_ref.binding);
                            }
                        }
                    } else if (bound_agg.function.name == "count_star") {
                        continue;
                    } else {
                        throw std::runtime_error("Unsupported aggregate expression type 1 in PruneAggregationWithProjectionMap: " + bound_agg.function.name);
                    }
                }
            }
            bottom_proj->ResolveOperatorTypes();
        }
    }

    // Step 4: Prune the aggregate in a single pass for groups and expressions
    vector<unique_ptr<Expression>> new_groups;
    vector<unique_ptr<Expression>> new_agg_exprs;
    
    idx_t new_group_idx = 0;
    
    // Process groups - combine pruning, binding update, and type tracking
    for (idx_t i = 0; i < agg.groups.size(); i++) {
        if (kept_group_indices.count(i) > 0) {
            UpdateBindingMapOnce(ColumnBinding(agg.group_index, i), ColumnBinding(agg.group_index, new_group_idx++));
            new_groups.push_back(std::move(agg.groups[i]));
        }
    }
    
    // Process aggregate expressions - annot! 
    for (idx_t i = 0; i < agg.expressions.size(); i++) {
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
            col_ref.binding = GetUpdatedBindingOnce(col_ref.binding);
        } else if (expr->type == ExpressionType::BOUND_FUNCTION) {
            // Handle scalar functions like multiplication (annot1 * annot2)
            auto &func_expr = expr->Cast<BoundFunctionExpression>();
        
            // Update bindings in all child expressions
            for (auto &child : func_expr.children) {
                if (child->type == ExpressionType::BOUND_COLUMN_REF) {
                    auto &child_ref = child->Cast<BoundColumnRefExpression>();
                    child_ref.binding = GetUpdatedBindingOnce(child_ref.binding);
                }
            }
        }
    }
    
    agg.ResolveOperatorTypes();
    top_proj.ResolveOperatorTypes();
    
    return std::move(op);
}

bool AggregationPushdown::NotHasTooManyGroups(unique_ptr<LogicalOperator>& op) {
    // Check if this is an aggregate operator
    if (op->type == LogicalOperatorType::LOGICAL_PROJECTION && 
        op->children.size() == 1 &&
        op->children[0]->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
    
        auto &agg = op->children[0]->Cast<LogicalAggregate>();
        return agg.groups.size() <= GROUP_BY_NUM;
    }
    return false;
}

void AggregationPushdown::RecordAggPushdown(unique_ptr<LogicalOperator>& op) {
    static int join_counter = 0;
    if (!op) {
        return ;
    }
    for (idx_t i = 0; i < op->children.size(); i++) {
        RecordAggPushdown(op->children[i]);
    }
    // First process this operator if it's a join
    if (op->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN ||
        op->type == LogicalOperatorType::LOGICAL_ASOF_JOIN ||
        op->type == LogicalOperatorType::LOGICAL_DELIM_JOIN) {

        auto &join = op->Cast<LogicalComparisonJoin>();
        bool left = false, right = false;
    
        if (join.children[0]->type == LogicalOperatorType::LOGICAL_PROJECTION) {
            left = NotHasTooManyGroups(join.children[0]);
        }
        if (join.children[1]->type == LogicalOperatorType::LOGICAL_PROJECTION) {
            right = NotHasTooManyGroups(join.children[1]);
        }
        join_pushdown_info.push_back({join_counter++, left, right});
    }
}

unique_ptr<LogicalOperator> AggregationPushdown::RemoveHeavyAggregation(unique_ptr<LogicalOperator> op) {
    if (op->type == LogicalOperatorType::LOGICAL_PROJECTION && 
        op->children.size() == 1 &&
        op->children[0]->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
        auto &top_proj = op->Cast<LogicalProjection>();
        auto &agg = op->children[0]->Cast<LogicalAggregate>();
        bool has_bottom_proj = (agg.children.size() == 1 && agg.children[0]->type == LogicalOperatorType::LOGICAL_PROJECTION);
        // Heavy aggregation! 
        std::cout << "RemoveHeavyAggregation" << std::endl;
        op->Print();
        if (agg.groups.size() > GROUP_BY_NUM) {
            if (has_bottom_proj) {
                // Case 1: Bottom projection exists
                auto &bottom_proj = agg.children[0]->Cast<LogicalProjection>();
                auto original_source = std::move(bottom_proj.children[0]);

                if (query_type == QueryType::MINMAX_AGGREGATE) {
                    top_proj.expressions = std::move(bottom_proj.expressions);
                    // return std::move(bottom_proj.children[0]);
                } else if (query_type == QueryType::COUNT_STAR) {
                    vector<ColumnBinding> source_annot_bindings;
                    vector<LogicalType> source_annot_types;
                    bool source_has_annot = FindAllAnnotAttributes(original_source.get(), source_annot_bindings, source_annot_types);
                    bool bottom_proj_has_annot = bottom_proj.expressions.back()->GetName() == "annot";

                    if (bottom_proj_has_annot && source_has_annot) {
                        bool is_column_ref = (bottom_proj.expressions.back()->type == ExpressionType::BOUND_COLUMN_REF);
                        bool is_function = (bottom_proj.expressions.back()->type == ExpressionType::BOUND_FUNCTION);
                        std::string function_name;
                        if (is_function) {
                            function_name = bottom_proj.expressions.back()->Cast<BoundFunctionExpression>().function.name;
                        }
                        if (is_column_ref) {
                            top_proj.expressions = std::move(bottom_proj.expressions);
                        } else if (is_function && function_name == "*") {
                            if (source_annot_bindings.size() == 2) {
                                // NOTE: not change for annot, just move expression
                                top_proj.expressions = std::move(bottom_proj.expressions);
                            } else {
                                // NOTE: replace annot * annot to annot only, need to update the expressions
                                vector<unique_ptr<Expression>> new_expressions;
                                for (idx_t i = 0; i < bottom_proj.expressions.size() - 1; i++) {
                                    new_expressions.push_back(std::move(bottom_proj.expressions[i]));
                                }
                                auto new_ref = make_uniq<BoundColumnRefExpression>("annot", source_annot_types[0], source_annot_bindings[0]);
                                new_expressions.push_back(std::move(new_ref));
                                top_proj.expressions = std::move(new_expressions);
                            }
                        } else {
                            throw std::runtime_error("Unsupported expression type in RemoveHeavyAggregation");
                        }
                    } else if (!source_has_annot) {
                        // NOTE: Remove last annot expression
                        top_proj.expressions.clear();
                        for (idx_t i = 0; i < bottom_proj.expressions.size(); i++) {
                            auto &expr = bottom_proj.expressions[i];
                            if (expr->GetName() != "annot") {
                                top_proj.expressions.push_back(std::move(bottom_proj.expressions[i]));
                            }
                        }
                    } else {
                        throw std::runtime_error("Should not happen: Source has annot but bottom projection does not!");
                    }
                } else {
                    throw std::runtime_error("Unsupported query type in RemoveHeavyAggregation");
                }
                top_proj.children[0] = std::move(original_source);
            } else {
                // Case 2: No bottom projection, join both child don't have annot
                auto original_source = std::move(agg.children[0]);
                vector<unique_ptr<Expression>> new_expressions;
                
                // 2.1 add all group expressions for the new projection
                for (idx_t i = 0; i < agg.groups.size(); i++) {
                    auto &col_ref = agg.groups[i]->Cast<BoundColumnRefExpression>();
                    auto new_ref = make_uniq<BoundColumnRefExpression>(
                        col_ref.alias.empty() ? col_ref.GetName() : col_ref.alias,
                        col_ref.return_type, 
                        col_ref.binding
                    );
                    new_expressions.push_back(std::move(new_ref));
                }

                // 2.2 add aggregation expression
                for (auto &agg_expr : agg.expressions) {
                    if (agg_expr->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
                        auto &bound_agg = agg_expr->Cast<BoundAggregateExpression>();
                        string func_name = bound_agg.function.name;
                        
                        if (func_name == "count_star" || func_name == "count") {
                            if (query_type == QueryType::COUNT_STAR) {
                                // NOTE: Not pushing count(*) to the new projection expression
                                continue;
                            }
                        } else if (func_name == "min" || func_name == "max") {
                            if (query_type == QueryType::MINMAX_AGGREGATE && !bound_agg.children.empty()) {
                                // Extract the binding from the expression
                                auto& child = bound_agg.children[0];
                                
                                if (child->type == ExpressionType::BOUND_COLUMN_REF) {
                                    auto& child_ref = child->Cast<BoundColumnRefExpression>();
                                    // Add a direct reference to the original column
                                    auto col_ref = make_uniq<BoundColumnRefExpression>(
                                        "annot", 
                                        child->return_type,
                                        child_ref.binding
                                    );
                                    new_expressions.push_back(std::move(col_ref));
                                }
                            }
                        } else {
                            throw std::runtime_error("Unsupported aggregate function in RemoveHeavyAggregation");
                        }
                    }
                }
                top_proj.expressions = std::move(new_expressions);
                top_proj.children[0] = std::move(original_source);
            }
            op->ResolveOperatorTypes();
        } else {
            if (has_bottom_proj) {
                auto &bottom_proj = agg.children[0]->Cast<LogicalProjection>();
                // NOTE: Should have some fix here, to update the COUNT annot1 * annot2
                if (query_type == QueryType::COUNT_STAR) {
                    vector<ColumnBinding> source_annot_bindings;
                    vector<LogicalType> source_annot_types;
                    vector<idx_t> source_annot_idx;
                    bool source_has_annot = FindAllAnnotAttributes(bottom_proj.children[0].get(), source_annot_bindings, source_annot_types);
                    idx_t annot_num = source_annot_bindings.size();
                    // annot expression
                    auto& expr = bottom_proj.expressions.back();
                    auto& agg_expr = agg.expressions.back();
                    if (expr->GetName() == "annot" && expr->type == ExpressionType::BOUND_FUNCTION) {
                        auto& func_expr = expr->Cast<BoundFunctionExpression>();
                        if (func_expr.function.name == "*" && func_expr.children.size() == 2) {
                            if (annot_num == 1) {
                                // Update the annot type
                                auto proj_expr = make_uniq<BoundColumnRefExpression>(
                                    "annot",                  // Use consistent name
                                    source_annot_types[0],    // Use type from source
                                    source_annot_bindings[0]  // Direct binding to source annotation
                                );
                                expr = std::move(proj_expr);
                                vector<unique_ptr<Expression>> sum_args;
                                auto bottom_proj_bindings = bottom_proj.GetColumnBindings();
                                // Create a column reference that points to the LAST column in bottom_pro
                                auto annot_ref = make_uniq<BoundColumnRefExpression>(
                                    "annot",
                                    source_annot_types[0],
                                    bottom_proj_bindings[bottom_proj_bindings.size() - 1]  // Use the LAST column binding
                                );
                                        
                                sum_args.push_back(std::move(annot_ref));   
                                AggregateFunction sum_function = GetSumAggregate(source_annot_types[0].InternalType());
                                sum_function.name = "sum";
                                FunctionBinder function_binder(context);
                                auto sum_expr = function_binder.BindAggregateFunction(
                                    sum_function,
                                    std::move(sum_args),
                                    nullptr,
                                    AggregateType::NON_DISTINCT
                                );
                                agg_expr = std::move(sum_expr);
                            } else if (annot_num == 0) {
                                auto count_star_fun = CountStarFun::GetFunction();
                                if (count_star_fun.name.empty()) {
                                    count_star_fun.name = "count_star";
                                }
    
                                FunctionBinder function_binder(context);
                                auto count_star = function_binder.BindAggregateFunction(
                                    count_star_fun, 
                                    {}, 
                                    nullptr, 
                                    AggregateType::NON_DISTINCT
                                );
                                
                                auto cast_expr = BoundCastExpression::AddDefaultCastToType(
                                    std::move(count_star),
                                    LogicalType::HUGEINT  // Note the parentheses to create the type object
                                );
                                agg_expr = std::move(cast_expr);
                                bottom_proj.expressions.pop_back();
                            } else {
                                // Case: Multiple annotation columns in source, fix type error
                                vector<unique_ptr<Expression>> mult_children;
                                // Get source annotation columns and create new references
                                auto left_ref = make_uniq<BoundColumnRefExpression>(
                                    "annot", 
                                    source_annot_types[0], 
                                    source_annot_bindings[0]
                                );
                                
                                auto right_ref = make_uniq<BoundColumnRefExpression>(
                                    "annot", 
                                    source_annot_types[1], 
                                    source_annot_bindings[1]
                                );
                                
                                // Check for type mismatch between BIGINT and HUGEINT
                                if (source_annot_types[0].id() == LogicalTypeId::BIGINT && 
                                    source_annot_types[1].id() == LogicalTypeId::HUGEINT) {
                                    // Cast the left operand from BIGINT to HUGEINT
                                    auto cast_expr = BoundCastExpression::AddDefaultCastToType(
                                        std::move(left_ref),
                                        LogicalType::HUGEINT
                                    );
                                    mult_children.push_back(std::move(cast_expr));
                                    mult_children.push_back(std::move(right_ref));
                                } else if (source_annot_types[1].id() == LogicalTypeId::BIGINT && 
                                           source_annot_types[0].id() == LogicalTypeId::HUGEINT) {
                                    // Cast the right operand from BIGINT to HUGEINT
                                    mult_children.push_back(std::move(left_ref));
                                    auto cast_expr = BoundCastExpression::AddDefaultCastToType(
                                        std::move(right_ref),
                                        LogicalType::HUGEINT
                                    );
                                    mult_children.push_back(std::move(cast_expr));
                                } else {
                                    // No type mismatch, use expressions directly
                                    mult_children.push_back(std::move(left_ref));
                                    mult_children.push_back(std::move(right_ref));
                                }
                                
                                FunctionBinder function_binder(context);
                                string error_msg;
                                
                                auto bound_expr = function_binder.BindScalarFunction(
                                    DEFAULT_SCHEMA, 
                                    "*", 
                                    std::move(mult_children),
                                    error_msg,
                                    true
                                );
                                
                                if (!bound_expr) {
                                    throw Exception("Failed to bind multiplication function: " + error_msg);
                                }
                                
                                // Replace the expression with our newly created one
                                bound_expr->alias = "annot";
                                expr = std::move(bound_expr);
                            }
                        }
                    } else if (expr->GetName() == "annot" && expr->type == ExpressionType::BOUND_COLUMN_REF) {
                        auto& col_ref = expr->Cast<BoundColumnRefExpression>();
                        if (col_ref.binding.table_index == bottom_proj.table_index) {
                            if (annot_num == 0) {
                                auto count_star_fun = CountStarFun::GetFunction();
                                if (count_star_fun.name.empty()) {
                                    count_star_fun.name = "count_star";
                                }
    
                                FunctionBinder function_binder(context);
                                auto count_star = function_binder.BindAggregateFunction(
                                    count_star_fun, 
                                    {}, 
                                    nullptr, 
                                    AggregateType::NON_DISTINCT
                                );
                                
                                auto cast_expr = BoundCastExpression::AddDefaultCastToType(
                                    std::move(count_star),
                                    LogicalType::HUGEINT  // Note the parentheses to create the type object
                                );

                                agg_expr = std::move(cast_expr);
                                bottom_proj.expressions.pop_back();
                            }
                        }
                    } else {
                        throw std::runtime_error("Unsupported expression type in RemoveHeavyAggregation");
                    }
                    bottom_proj.ResolveOperatorTypes();
                    agg.ResolveOperatorTypes();
                }
            }
        }
    }
    op->ResolveOperatorTypes();
    return std::move(op);
}

}