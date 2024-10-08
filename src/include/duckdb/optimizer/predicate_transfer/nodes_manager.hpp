#pragma once

#include "duckdb/main/client_context.hpp"

namespace duckdb {
class NodesManager {
public:
	explicit NodesManager(ClientContext &context) : context(context) {
	}

    idx_t NumNodes();

    void AddNode(LogicalOperator *op);

	void SortNodes();

	void ReSortNodes();

	const vector<RelationStats> GetRelationStats();

	LogicalOperator* getNode(idx_t table_binding) {
		auto itr = nodes.find(table_binding);
		if(itr == nodes.end()) {
			return nullptr;
			throw InternalException("table binding is not found!");
		} else {
			return itr->second;
		}
	}

	unordered_map<idx_t, LogicalOperator*>& getNodes() {
		return nodes;
	}

	void EraseNode(idx_t key);

	void DuplicateNodes() {
		duplicate_nodes = nodes;
		// for(auto itr : nodes) {
		// 	duplicate_nodes.insert(itr);
		// }
	}

	void RecoverNodes() {
		nodes = duplicate_nodes;
	}

	vector<LogicalOperator*>& getSortedNodes() {
		return sort_nodes;
	}
	
	void ExtractNodes(LogicalOperator &plan, vector<reference<LogicalOperator>> &filter_operators);

	ColumnBinding FindRename(ColumnBinding col);

private:
	ClientContext &context;

	unordered_map<idx_t, LogicalOperator*> nodes;
	unordered_map<idx_t, LogicalOperator*> duplicate_nodes;

	//! sorted
	vector<LogicalOperator*> sort_nodes;
	
	static int nodesCmp(LogicalOperator *a, LogicalOperator *b);

	struct HashFunc {
    	size_t operator()(const ColumnBinding& key) const {
        	return std::hash<uint64_t>()(key.table_index) ^ std::hash<uint64_t>()(key.column_index);
    	}
	};

	struct CmpFunc {
    	bool operator()(const ColumnBinding& a, const ColumnBinding& b) const {
        	return (a.table_index == b.table_index) && (a.column_index == b.column_index);
    	}
	};

	unordered_map<ColumnBinding, ColumnBinding, HashFunc, CmpFunc> rename_cols;
};
}