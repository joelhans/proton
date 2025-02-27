#include <Parsers/ASTDropFunctionQuery.h>

#include <Access/ContextAccess.h>
#include <Functions/UserDefined/IUserDefinedSQLObjectsLoader.h>
#include <Functions/UserDefined/UserDefinedSQLFunctionFactory.h>
#include <Interpreters/Context.h>
#include <Interpreters/FunctionNameNormalizer.h>
#include <Interpreters/InterpreterDropFunctionQuery.h>
/// proton: starts
//#include <Interpreters/executeDDLQueryOnCluster.h>
/// proton: ends


namespace DB
{

namespace ErrorCodes
{
extern const int INCORRECT_QUERY;
}

BlockIO InterpreterDropFunctionQuery::execute()
{
    FunctionNameNormalizer().visit(query_ptr.get());
    ASTDropFunctionQuery & drop_function_query = query_ptr->as<ASTDropFunctionQuery &>();

    AccessRightsElements access_rights_elements;
    access_rights_elements.emplace_back(AccessType::DROP_FUNCTION);

    auto current_context = getContext();

    /// proton: starts. Not support clickhouse cluster
    //    if (!drop_function_query.cluster.empty())
    //    {
    //        if (current_context->getUserDefinedSQLObjectsLoader().isReplicated())
    //            throw Exception(ErrorCodes::INCORRECT_QUERY, "ON CLUSTER is not allowed because used-defined functions are replicated automatically");
    //
    //        DDLQueryOnClusterParams params;
    //        params.access_to_check = std::move(access_rights_elements);
    //        return executeDDLQueryOnCluster(query_ptr, current_context, params);
    //    }
    /// proton: ends

    current_context->checkAccess(access_rights_elements);

    bool throw_if_not_exists = !drop_function_query.if_exists;

    UserDefinedSQLFunctionFactory::instance().unregisterFunction(current_context, drop_function_query.function_name, throw_if_not_exists);

    return {};
}

}