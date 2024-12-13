#include "duckdb/execution/operator/helper/physical_extension.hpp"

#include "duckdb/parallel/meta_pipeline.hpp"
#include "duckdb/parallel/pipeline.hpp"

namespace duckdb {

PhysicalExtension::PhysicalExtension(PhysicalOperator &plan1, PhysicalOperator &plan2)
    : PhysicalOperator(PhysicalOperatorType::EXTENSION, plan1.types, -1), plan1(plan1), plan2(plan2) {
}

vector<const_reference<PhysicalOperator>> PhysicalExtension::GetChildren() const {
	return {plan1, plan2};
}

void PhysicalExtension::BuildPipelines(Pipeline &current, MetaPipeline &meta_pipeline) {
    this->op_state.reset();
	this->sink_state.reset();

	// 'current' is the probe pipeline: add this operator
	auto &state = meta_pipeline.GetState();
	state.AddPipelineOperator(current, *this);
    auto &child_meta_pipeline = meta_pipeline.CreateChildMetaPipeline(current, *this);
	child_meta_pipeline.Build(plan2);
    plan1.BuildPipelines(current, meta_pipeline);
}

} // namespace duckdb
