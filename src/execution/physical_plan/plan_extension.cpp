#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/planner/operator/logical_extension_operator.hpp"
#include "duckdb/execution/operator/helper/physical_extension.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/parallel/task_scheduler.hpp"


namespace duckdb {
    unique_ptr<PhysicalOperator> PhysicalPlanGenerator::CreatePlan(LogicalExtensionOperator &op) {
	    unique_ptr<PhysicalOperator> plan1 = CreatePlan(*op.children[0]);
	    unique_ptr<PhysicalOperator> plan2 = CreatePlan(*op.children[1]);
	    auto ext = make_uniq<PhysicalExtension>(plan1->types, op.estimated_cardinality);
		ext->children.emplace_back(std::move(plan1));
		ext->children.emplace_back(std::move(plan2));
	    unique_ptr<PhysicalOperator> plan = std::move(ext);
	    return plan;
    }
}