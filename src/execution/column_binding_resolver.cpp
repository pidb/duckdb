#include "duckdb/execution/column_binding_resolver.hpp"

#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_create_index.hpp"
#include "duckdb/planner/operator/logical_delim_join.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"

#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/to_string.hpp"

namespace duckdb {

ColumnBindingResolver::ColumnBindingResolver() {
}

void ColumnBindingResolver::VisitOperator(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN || op.type == LogicalOperatorType::LOGICAL_DELIM_JOIN) {
		// special case: comparison join
		auto &comp_join = (LogicalComparisonJoin &)op;
		// first get the bindings of the LHS and resolve the LHS expressions
		VisitOperator(*comp_join.children[0]);
		for (auto &cond : comp_join.conditions) {
			VisitExpression(&cond.left);
		}
		if (op.type == LogicalOperatorType::LOGICAL_DELIM_JOIN) {
			// visit the duplicate eliminated columns on the LHS, if any
			auto &delim_join = (LogicalDelimJoin &)op;
			for (auto &expr : delim_join.duplicate_eliminated_columns) {
				VisitExpression(&expr);
			}
		}
		// then get the bindings of the RHS and resolve the RHS expressions
		VisitOperator(*comp_join.children[1]);
		for (auto &cond : comp_join.conditions) {
			VisitExpression(&cond.right);
		}
		// finally update the bindings with the result bindings of the join
		bindings = op.GetColumnBindings();
		return;
	} else if (op.type == LogicalOperatorType::LOGICAL_ANY_JOIN) {
		// ANY join, this join is different because we evaluate the expression on the bindings of BOTH join sides at
		// once i.e. we set the bindings first to the bindings of the entire join, and then resolve the expressions of
		// this operator
		VisitOperatorChildren(op);
		bindings = op.GetColumnBindings();
		VisitOperatorExpressions(op);
		return;
	} else if (op.type == LogicalOperatorType::LOGICAL_CREATE_INDEX) {
		// CREATE INDEX statement, add the columns of the table with table index 0 to the binding set
		// afterwards bind the expressions of the CREATE INDEX statement
		auto &create_index = (LogicalCreateIndex &)op;
		bindings = LogicalOperator::GenerateColumnBindings(0, create_index.table.columns.LogicalColumnCount());
		VisitOperatorExpressions(op);
		return;
	} else if (op.type == LogicalOperatorType::LOGICAL_GET) {
		//! We first need to update the current set of bindings and then visit operator expressions
		bindings = op.GetColumnBindings();
		VisitOperatorExpressions(op);
		return;
	} else if (op.type == LogicalOperatorType::LOGICAL_INSERT) {
		//! We want to execute the normal path, but also add a dummy 'excluded' binding if there is a
		// ON CONFLICT DO UPDATE clause
		auto &insert_op = (LogicalInsert &)op;
		if (insert_op.action_type != OnConflictAction::THROW) {
			VisitOperatorChildren(op);
			auto dummy_bindings = LogicalOperator::GenerateColumnBindings(
			    insert_op.excluded_table_index, insert_op.table->columns.PhysicalColumnCount());
			bindings.insert(bindings.begin(), dummy_bindings.begin(), dummy_bindings.end());
			if (insert_op.on_conflict_condition) {
				VisitExpression(&insert_op.on_conflict_condition);
			}
			if (insert_op.do_update_condition) {
				VisitExpression(&insert_op.do_update_condition);
			}
			VisitOperatorExpressions(op);
			bindings = op.GetColumnBindings();
			return;
		}
	}
	// general case
	// first visit the children of this operator
	VisitOperatorChildren(op);
	// now visit the expressions of this operator to resolve any bound column references
	VisitOperatorExpressions(op);
	// finally update the current set of bindings to the current set of column bindings
	bindings = op.GetColumnBindings();
}

unique_ptr<Expression> ColumnBindingResolver::VisitReplace(BoundColumnRefExpression &expr,
                                                           unique_ptr<Expression> *expr_ptr) {
	D_ASSERT(expr.depth == 0);
	// check the current set of column bindings to see which index corresponds to the column reference
	for (idx_t i = 0; i < bindings.size(); i++) {
		if (expr.binding == bindings[i]) {
			return make_unique<BoundReferenceExpression>(expr.alias, expr.return_type, i);
		}
	}
	// LCOV_EXCL_START
	// could not bind the column reference, this should never happen and indicates a bug in the code
	// generate an error message
	string bound_columns = "[";
	for (idx_t i = 0; i < bindings.size(); i++) {
		if (i != 0) {
			bound_columns += " ";
		}
		bound_columns += to_string(bindings[i].table_index) + "." + to_string(bindings[i].column_index);
	}
	bound_columns += "]";

	throw InternalException("Failed to bind column reference \"%s\" [%d.%d] (bindings: %s)", expr.alias,
	                        expr.binding.table_index, expr.binding.column_index, bound_columns);
	// LCOV_EXCL_STOP
}

unordered_set<idx_t> ColumnBindingResolver::VerifyInternal(LogicalOperator &op) {
	unordered_set<idx_t> result;
	for (auto &child : op.children) {
		auto child_indexes = VerifyInternal(*child);
		for (auto index : child_indexes) {
			D_ASSERT(index != DConstants::INVALID_INDEX);
			if (result.find(index) != result.end()) {
				throw InternalException("Duplicate table index \"%lld\" found", index);
			}
			result.insert(index);
		}
	}
	auto indexes = op.GetTableIndex();
	for (auto index : indexes) {
		D_ASSERT(index != DConstants::INVALID_INDEX);
		if (result.find(index) != result.end()) {
			throw InternalException("Duplicate table index \"%lld\" found", index);
		}
		result.insert(index);
	}
	return result;
}

void ColumnBindingResolver::Verify(LogicalOperator &op) {
#ifdef DEBUG
	VerifyInternal(op);
#endif
}

} // namespace duckdb
