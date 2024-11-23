#include "duckdb/planner/operator/logical_use_bf.hpp"

namespace duckdb {
#ifdef UseHashFilter
LogicalUseBF::LogicalUseBF(vector<shared_ptr<HashFilter>> bf) 
#else
LogicalUseBF::LogicalUseBF(vector<shared_ptr<BlockedBloomFilter>> bf) 
#endif
    : LogicalOperator(LogicalOperatorType::LOGICAL_USE_BF), bf_to_use(bf) {};

void LogicalUseBF::Serialize(Serializer &serializer) const {
	LogicalOperator::Serialize(serializer);
    throw InternalException("Shouldn't go here: LogicalUseBF::Serialize");
}

unique_ptr<LogicalOperator> LogicalUseBF::Deserialize(Deserializer &deserializer) {
    throw InternalException("Shouldn't go here: LogicalUseBF::Deserialize");
	return nullptr;
}

idx_t LogicalUseBF::EstimateCardinality(ClientContext &context) {
	return children[0]->EstimateCardinality(context);
}

void LogicalUseBF::ResolveTypes() {
	types = children[0]->types;
}

vector<ColumnBinding> LogicalUseBF::GetColumnBindings() {
	return children[0]->GetColumnBindings();
}

void LogicalUseBF::AddDownStreamOperator(LogicalCreateBF *op) {
	related_create_bf.emplace_back(op);
}

string LogicalUseBF::ParamsToString() const {
	string result = LogicalOperator::ParamsToString();
	auto ColomnBindings = const_cast<LogicalUseBF*>(this)->GetColumnBindings();
	for (auto &binding : ColomnBindings) {
		result += binding.ToString();
	}
	return result;
}

} // namespace duckdb