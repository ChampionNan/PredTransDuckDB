#include "duckdb/execution/operator/helper/physical_extension.hpp"

#include "duckdb/parallel/meta_pipeline.hpp"
#include "duckdb/parallel/pipeline.hpp"

namespace duckdb {

PhysicalExtension::PhysicalExtension(vector<LogicalType> types, idx_t estimated_cardinality)
    : CachingPhysicalOperator(PhysicalOperatorType::EXTENSION, std::move(types), estimated_cardinality){
}

unique_ptr<OperatorState> PhysicalExtension::GetOperatorState(ExecutionContext &context) const {
	return make_uniq<CachingOperatorState>();
}

vector<const_reference<PhysicalOperator>> PhysicalExtension::GetChildren() const {
	vector<const_reference<PhysicalOperator>> result;
	for (auto &child : children) {
		result.push_back(*child);
	}
	return result;
}

void PhysicalExtension::BuildPipelines(Pipeline &current, MetaPipeline &meta_pipeline) {
	op_state.reset();

	auto &state = meta_pipeline.GetState();
	
	state.AddPipelineOperator(current, *this);
	children[0]->BuildPipelines(current, meta_pipeline);
}

vector<const_reference<PhysicalOperator>> PhysicalExtension::GetSources() const {
	return children[0]->GetSources();
}

string PhysicalExtension::ParamsToString() const {
	string result;
	return result;
}

OperatorResultType PhysicalExtension::ExecuteInternal(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
	GlobalOperatorState &gstate, OperatorState &state_p) const {
	auto &state = state_p.Cast<CachingOperatorState>();
	chunk.Reference(input);
	return OperatorResultType::NEED_MORE_INPUT;
}


} // namespace duckdb
