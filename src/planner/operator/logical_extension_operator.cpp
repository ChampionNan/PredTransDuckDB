#include "duckdb/planner/operator/logical_extension_operator.hpp"
#include "duckdb/execution/column_binding_resolver.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"

#include "duckdb/planner/operator/logical_execute.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/execution/operator/helper/physical_extension.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/execution/operator/helper/physical_execute.hpp"


namespace duckdb {

void LogicalExtensionOperator::ResolveColumnBindings(ColumnBindingResolver &res, vector<ColumnBinding> &bindings) {
	// general case
	// first visit the children of this operator
	/*
	for (auto &child : children) {
		res.VisitOperator(*child);
	}
	// now visit the expressions of this operator to resolve any bound column references
	for (auto &expression : expressions) {
		res.VisitExpression(&expression);
	}*/
	res.VisitOperator(*children[0]);
	for (auto &cond : conditions) {
		res.VisitExpression(&cond.left);
	}
	for (auto &expr : duplicate_eliminated_columns) {
		res.VisitExpression(&expr);
	}
	res.VisitOperator(*children[1]);
	for (auto &cond : conditions) {
		res.VisitExpression(&cond.right);
	}
	// finally update the current set of bindings to the current set of column bindings
	// bindings = children[0]->GetColumnBindings();
	bindings = GetColumnBindings();
}

void LogicalExtensionOperator::Serialize(Serializer &serializer) const {
	LogicalOperator::Serialize(serializer);
	serializer.WriteProperty(200, "extension_name", GetExtensionName());
}

unique_ptr<LogicalOperator> LogicalExtensionOperator::Deserialize(Deserializer &deserializer) {
	auto &config = DBConfig::GetConfig(deserializer.Get<ClientContext &>());
	auto extension_name = deserializer.ReadProperty<string>(200, "extension_name");
	for (auto &extension : config.operator_extensions) {
		if (extension->GetName() == extension_name) {
			return extension->Deserialize(deserializer);
		}
	}
	throw SerializationException("No deserialization method exists for extension: " + extension_name);
}

void LogicalExtensionOperator::ResolveTypes() {
	types = children[0]->types;
}

vector<ColumnBinding> LogicalExtensionOperator::GetColumnBindings() {
	return children[0]->GetColumnBindings();
}

string LogicalExtensionOperator::GetExtensionName() const {
	throw SerializationException("LogicalExtensionOperator::GetExtensionName not implemented which is required for "
	                             "serializing extension operators");
}

} // namespace duckdb
