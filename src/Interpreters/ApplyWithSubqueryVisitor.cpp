#include <Interpreters/ApplyWithSubqueryVisitor.h>
#include <Interpreters/IdentifierSemantic.h>
#include <Interpreters/StorageID.h>
#include <Interpreters/misc.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTSelectQuery.h>
#include <Parsers/ASTSelectWithUnionQuery.h>
#include <Parsers/ASTSubquery.h>
#include <Parsers/ASTTablesInSelectQuery.h>
#include <Parsers/ASTWithElement.h>

/// proton: starts.
#include <TableFunctions/TableFunctionFactory.h>
/// proton: ends.

namespace DB
{
void ApplyWithSubqueryVisitor::visit(ASTPtr & ast, const Data & data)
{
    if (auto * node_select = ast->as<ASTSelectQuery>())
        visit(*node_select, data);
    else
    {
        for (auto & child : ast->children)
            visit(child, data);
        if (auto * node_func = ast->as<ASTFunction>())
            visit(*node_func, data);
        else if (auto * node_table = ast->as<ASTTableExpression>())
            visit(*node_table, data);
    }
}

void ApplyWithSubqueryVisitor::visit(ASTSelectQuery & ast, const Data & data)
{
    std::optional<Data> new_data;
    if (auto with = ast.with())
    {
        for (auto & child : with->children)
        {
            visit(child, new_data ? *new_data : data);
            if (auto * ast_with_elem = child->as<ASTWithElement>())
            {
                if (!new_data)
                    new_data = data;
                new_data->subqueries[ast_with_elem->name] = ast_with_elem->subquery;
            }
        }
    }

    for (auto & child : ast.children)
    {
        if (child != ast.with())
            visit(child, new_data ? *new_data : data);
    }
}

void ApplyWithSubqueryVisitor::visit(ASTSelectWithUnionQuery & ast, const Data & data)
{
    for (auto & child : ast.children)
        visit(child, data);
}

void ApplyWithSubqueryVisitor::visit(ASTTableExpression & table, const Data & data)
{
    if (table.database_and_table_name)
    {
        auto table_id = table.database_and_table_name->as<ASTTableIdentifier>()->getTableId();
        if (table_id.database_name.empty())
        {
            auto subquery_it = data.subqueries.find(table_id.table_name);
            if (subquery_it != data.subqueries.end())
            {
                auto old_alias = table.database_and_table_name->tryGetAlias();
                table.children.clear();
                table.database_and_table_name.reset();
                table.subquery = subquery_it->second->clone();
                table.subquery->as<ASTSubquery &>().cte_name = table_id.table_name;
                if (!old_alias.empty())
                    table.subquery->setAlias(old_alias);
                table.children.emplace_back(table.subquery);
            }
        }
    }
}

void ApplyWithSubqueryVisitor::visit(ASTFunction & func, const Data & data)
{
    /// Special CTE case, where the right argument of IN is alias (ASTIdentifier) from WITH clause.

    if (checkFunctionIsInOrGlobalInOperator(func))
    {
        auto & ast = func.arguments->children.at(1);
        if (const auto * identifier = ast->as<ASTIdentifier>())
        {
            if (identifier->isShort())
            {
                /// Clang-tidy is wrong on this line, because `func.arguments->children.at(1)` gets replaced before last use of `name`.
                auto name = identifier->shortName();  // NOLINT
                auto subquery_it = data.subqueries.find(name);
                if (subquery_it != data.subqueries.end())
                {
                    auto old_alias = func.arguments->children[1]->tryGetAlias();
                    func.arguments->children[1] = subquery_it->second->clone();
                    func.arguments->children[1]->as<ASTSubquery &>().cte_name = name;
                    if (!old_alias.empty())
                        func.arguments->children[1]->setAlias(old_alias);
                }
            }
        }
    }

    /// proton: starts. replace table identifier with subquery defined by WITH for tumble/hop/session/dedup table
    /// function. For example,
    /// "WITH transformed AS (...)
    /// SELECT ...
    /// FROM
    ///    tumble(transformed, ...)
    /// GROUP BY ..."
    /// it will find 'transformed' argument in tumble function and replace with the subquery
    /// defined by WITH and set its cte_name to 'transformed'
    if (TableFunctionFactory::instance().isSupportSubqueryTableFunctionName(func.name))
    {
        auto & ast = func.arguments->children.at(0);
        if (const auto * identifier = ast->as<ASTIdentifier>())
        {
            if (identifier->isShort())
            {
                /// Clang-tidy is wrong on this line, because `func.arguments->children.at(1)` gets replaced before last use of `name`.
                auto name = identifier->shortName(); // NOLINT
                auto subquery_it = data.subqueries.find(name);
                if (subquery_it != data.subqueries.end())
                {
                    auto old_alias = func.arguments->children[0]->tryGetAlias();
                    func.arguments->children[0] = subquery_it->second->clone();
                    func.arguments->children[0]->as<ASTSubquery &>().cte_name = name;
                    if (!old_alias.empty())
                        func.arguments->children[0]->setAlias(old_alias);
                }
            }
        }
    }
    /// proton: ends
}

}
