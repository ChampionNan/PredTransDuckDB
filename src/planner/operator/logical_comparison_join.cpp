#include "duckdb/common/string_util.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/common/enum_util.hpp"
namespace duckdb {

LogicalComparisonJoin::LogicalComparisonJoin(JoinType join_type, LogicalOperatorType logical_type)
    : LogicalJoin(join_type, logical_type) {
}

string LogicalComparisonJoin::ParamsToString() const {
	string result = EnumUtil::ToChars(join_type);
	for (auto &condition : conditions) {
		result += "\n";
		auto expr =
		    make_uniq<BoundComparisonExpression>(condition.comparison, condition.left->Copy(), condition.right->Copy());
		result += expr->ToString();
	}

	if (has_estimated_cardinality) {
		result += "\n(" + to_string(estimated_cardinality) + ")";
	} else {
		result += "\n(" + to_string(-1) + ")";
	}

	return result;
}
} // namespace duckdb
