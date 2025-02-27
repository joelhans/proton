#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include <Common/NamePrompter.h>
#include <Interpreters/Context_fwd.h>
#include <Functions/IFunction.h>

/// proton: starts.
#include <AggregateFunctions/IAggregateFunction.h>
/// proton: ends

namespace DB
{

class UserDefinedFunctionFactory
{
public:
    using Creator = std::function<FunctionOverloadResolverPtr(ContextPtr)>;

    static UserDefinedFunctionFactory & instance();

    static FunctionOverloadResolverPtr get(const String & function_name, ContextPtr context);

    /// proton: starts. For User Defined Aggregation functions
    static AggregateFunctionPtr
    getAggregateFunction(const String & name, const DataTypes & types, const Array & parameters, AggregateFunctionProperties & properties);

    static bool isAggregateFunctionName(const String & function_name);

    static bool isOrdinaryFunctionName(const String & function_name);
    /// proton: ends

    static FunctionOverloadResolverPtr tryGet(const String & function_name, ContextPtr context);

    static bool has(const String & function_name, ContextPtr context);

    static std::vector<String> getRegisteredNames(ContextPtr context);
};

}
