#include <AggregateFunctions/AggregateFunctionFactory.h>
#include <AggregateFunctions/HelpersMinMaxAny.h>


namespace DB
{
struct Settings;

namespace
{

AggregateFunctionPtr createAggregateFunctionAny(const std::string & name, const DataTypes & argument_types, const Array & parameters, const Settings * settings)
{
    return AggregateFunctionPtr(createAggregateFunctionSingleValue<AggregateFunctionsSingleValue, AggregateFunctionAnyData>(name, argument_types, parameters, settings));
}

AggregateFunctionPtr createAggregateFunctionAnyLast(const std::string & name, const DataTypes & argument_types, const Array & parameters, const Settings * settings)
{
    return AggregateFunctionPtr(createAggregateFunctionSingleValue<AggregateFunctionsSingleValue, AggregateFunctionAnyLastData>(name, argument_types, parameters, settings));
}

AggregateFunctionPtr createAggregateFunctionAnyHeavy(const std::string & name, const DataTypes & argument_types, const Array & parameters, const Settings * settings)
{
    return AggregateFunctionPtr(createAggregateFunctionSingleValue<AggregateFunctionsSingleValue, AggregateFunctionAnyHeavyData>(name, argument_types, parameters, settings));
}

}

void registerAggregateFunctionsAny(AggregateFunctionFactory & factory)
{
    AggregateFunctionProperties properties = { .returns_default_when_only_null = false, .is_order_dependent = true };

    factory.registerFunction("any", { createAggregateFunctionAny, properties });
    factory.registerFunction("any_last", { createAggregateFunctionAnyLast, properties });
    factory.registerFunction("any_heavy", { createAggregateFunctionAnyHeavy, properties });

    // Synonyms for use as window functions.
    factory.registerFunction("first_value",
        { createAggregateFunctionAny, properties },
        AggregateFunctionFactory::CaseSensitive);
    factory.registerFunction("last_value",
        { createAggregateFunctionAnyLast, properties },
        AggregateFunctionFactory::CaseSensitive);
    factory.registerFunction("earliest",
        { createAggregateFunctionAny, properties },
        AggregateFunctionFactory::CaseSensitive);
    factory.registerFunction("latest",
        { createAggregateFunctionAnyLast, properties },
        AggregateFunctionFactory::CaseSensitive);
}

}
