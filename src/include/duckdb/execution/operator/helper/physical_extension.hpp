#pragma once

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/main/prepared_statement_data.hpp"

namespace duckdb {

class PhysicalExtension : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::EXTENSION;

public:
	explicit PhysicalExtension(PhysicalOperator &plan1, PhysicalOperator &plan2);

	PhysicalOperator &plan1;
	PhysicalOperator &plan2;

public:
	vector<const_reference<PhysicalOperator>> GetChildren() const override;

public:
	void BuildPipelines(Pipeline &current, MetaPipeline &meta_pipeline) override;
};

} // namespace duckdb
