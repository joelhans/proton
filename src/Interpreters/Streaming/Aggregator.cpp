#include "Aggregator.h"

#include <future>
#include <Poco/Util/Application.h>

#include <AggregateFunctions/AggregateFunctionArray.h>
#include <AggregateFunctions/AggregateFunctionState.h>
#include <Columns/ColumnTuple.h>
#include <Compression/CompressedWriteBuffer.h>
#include <DataTypes/DataTypeAggregateFunction.h>
#include <DataTypes/DataTypeLowCardinality.h>
#include <DataTypes/DataTypeNullable.h>
#include <Formats/NativeWriter.h>
#include <IO/Operators.h>
#include <IO/WriteBufferFromFile.h>
#include <Interpreters/JIT/CompiledExpressionCache.h>
#include <Interpreters/JIT/compileFunction.h>
#include <Common/CurrentThread.h>
#include <Common/JSONBuilder.h>
#include <Common/Stopwatch.h>
#include <Common/assert_cast.h>
#include <Common/formatReadable.h>
#include <Common/scope_guard_safe.h>
#include <Common/setThreadName.h>
#include <Common/typeid_cast.h>

/// proton: starts
#include <Columns/ColumnDecimal.h>
#include <Core/LightChunk.h>
#include <DataTypes/DataTypeDateTime.h>
#include <DataTypes/DataTypeFactory.h>
#include <Formats/SimpleNativeReader.h>
#include <Formats/SimpleNativeWriter.h>
#include <Interpreters/CompiledAggregateFunctionsHolder.h>
#include <Common/ProtonCommon.h>
#include <Common/VersionRevision.h>
#include <Common/logger_useful.h>
/// proton: ends


namespace ProfileEvents
{
    extern const Event ExternalAggregationWritePart;
    extern const Event ExternalAggregationCompressedBytes;
    extern const Event ExternalAggregationUncompressedBytes;
}

namespace DB
{

namespace ErrorCodes
{
    extern const int UNKNOWN_AGGREGATED_DATA_VARIANT;
    extern const int NOT_ENOUGH_SPACE;
    extern const int TOO_MANY_ROWS;
    extern const int EMPTY_DATA_PASSED;
    extern const int CANNOT_MERGE_DIFFERENT_AGGREGATED_DATA_VARIANTS;
    extern const int LOGICAL_ERROR;
    extern const int RECOVER_CHECKPOINT_FAILED;
    extern const int AGGREGATE_FUNCTION_NOT_APPLICABLE;
}

namespace Streaming
{
inline bool worthConvertToTwoLevel(
    size_t group_by_two_level_threshold, size_t result_size, size_t group_by_two_level_threshold_bytes, auto result_size_bytes)
{
    /// params.group_by_two_level_threshold will be equal to 0 if we have only one thread to execute aggregation (refer to AggregatingStep::transformPipeline).
    /// We like the unique key and state bytes both reach the threshold and then consider converting to two levels
    /// For `partition by` streaming case, there may be lots of partitions and each partition may have only one key
    /// but with more than `group_by_two_level_threshold_bytes` states.
    return (group_by_two_level_threshold && result_size >= group_by_two_level_threshold)
        && (group_by_two_level_threshold_bytes && result_size_bytes >= static_cast<Int64>(group_by_two_level_threshold_bytes));
}

AggregatedDataVariants::~AggregatedDataVariants()
{
    if (aggregator && !aggregator->all_aggregates_has_trivial_destructor)
    {
        try
        {
            aggregator->destroyAllAggregateStates(*this);
        }
        catch (...)
        {
            tryLogCurrentException(__PRETTY_FUNCTION__);
        }
    }
}

void AggregatedDataVariants::convertToTwoLevel()
{
    if (aggregator)
        LOG_TRACE(aggregator->log, "Converting aggregation data to two-level.");

    switch (type)
    {
    #define M(NAME) \
        case Type::NAME: \
            NAME ## _two_level = std::make_unique<decltype(NAME ## _two_level)::element_type>(*(NAME)); \
            (NAME).reset(); \
            type = Type::NAME ## _two_level; \
            break;

        APPLY_FOR_VARIANTS_CONVERTIBLE_TO_STATIC_BUCKET_TWO_LEVEL(M)

    #undef M

        default:
            throw Exception("Wrong data variant passed.", ErrorCodes::LOGICAL_ERROR);
    }
}

Block Aggregator::getHeader(bool final) const
{
    return params.getHeader(final);
}

Block Aggregator::Params::getHeader(
    const Block & src_header,
    const Block & intermediate_header,
    const ColumnNumbers & keys,
    const AggregateDescriptions & aggregates,
    bool final)
{
    Block res;

    if (intermediate_header)
    {
        res = intermediate_header.cloneEmpty();

        if (final)
        {
            for (const auto & aggregate : aggregates)
            {
                auto & elem = res.getByName(aggregate.column_name);

                elem.type = aggregate.function->getReturnType();
                elem.column = elem.type->createColumn();
            }
        }
    }
    else
    {
        for (const auto & key : keys)
            res.insert(src_header.safeGetByPosition(key).cloneEmpty());

        for (const auto & aggregate : aggregates)
        {
            size_t arguments_size = aggregate.arguments.size();
            DataTypes argument_types(arguments_size);
            for (size_t j = 0; j < arguments_size; ++j)
                argument_types[j] = src_header.safeGetByPosition(aggregate.arguments[j]).type;

            DataTypePtr type;
            if (final)
                type = aggregate.function->getReturnType();
            else
                type = std::make_shared<DataTypeAggregateFunction>(aggregate.function, argument_types, aggregate.parameters);

            res.insert({ type, aggregate.column_name });
        }
    }

    return materializeBlock(res);
}

void Aggregator::Params::explain(WriteBuffer & out, size_t indent) const
{
    Strings res;
    const auto & header = src_header ? src_header
                                     : intermediate_header;

    String prefix(indent, ' ');

    {
        /// Dump keys.
        out << prefix << "Keys: ";

        bool first = true;
        for (auto key : keys)
        {
            if (!first)
                out << ", ";
            first = false;

            if (key >= header.columns())
                out << "unknown position " << key;
            else
                out << header.getByPosition(key).name;
        }

        out << '\n';
    }

    if (!aggregates.empty())
    {
        out << prefix << "Aggregates:\n";

        for (const auto & aggregate : aggregates)
            aggregate.explain(out, indent + 4);
    }
}

void Aggregator::Params::explain(JSONBuilder::JSONMap & map) const
{
    const auto & header = src_header ? src_header
                                     : intermediate_header;

    auto keys_array = std::make_unique<JSONBuilder::JSONArray>();

    for (auto key : keys)
    {
        if (key >= header.columns())
            keys_array->add("");
        else
            keys_array->add(header.getByPosition(key).name);
    }

    map.add("Keys", std::move(keys_array));

    if (!aggregates.empty())
    {
        auto aggregates_array = std::make_unique<JSONBuilder::JSONArray>();

        for (const auto & aggregate : aggregates)
        {
            auto aggregate_map = std::make_unique<JSONBuilder::JSONMap>();
            aggregate.explain(*aggregate_map);
            aggregates_array->add(std::move(aggregate_map));
        }

        map.add("Aggregates", std::move(aggregates_array));
    }
}

Aggregator::Aggregator(const Params & params_) : params(params_),  log(&Poco::Logger::get("StreamingAggregator"))
{
    /// Use query-level memory tracker
    if (auto * memory_tracker_child = CurrentThread::getMemoryTracker())
        if (auto * memory_tracker = memory_tracker_child->getParent())
            memory_usage_before_aggregation = memory_tracker->get();

    aggregate_functions.resize(params.aggregates_size);
    for (size_t i = 0; i < params.aggregates_size; ++i)
        aggregate_functions[i] = params.aggregates[i].function.get();

    /// Initialize sizes of aggregation states and its offsets.
    offsets_of_aggregate_states.resize(params.aggregates_size);
    total_size_of_aggregate_states = 0;
    all_aggregates_has_trivial_destructor = true;

    // aggregate_states will be aligned as below:
    // |<-- state_1 -->|<-- pad_1 -->|<-- state_2 -->|<-- pad_2 -->| .....
    //
    // pad_N will be used to match alignment requirement for each next state.
    // The address of state_1 is aligned based on maximum alignment requirements in states
    for (size_t i = 0; i < params.aggregates_size; ++i)
    {
        offsets_of_aggregate_states[i] = total_size_of_aggregate_states;

        total_size_of_aggregate_states += params.aggregates[i].function->sizeOfData();

        // aggregate states are aligned based on maximum requirement
        align_aggregate_states = std::max(align_aggregate_states, params.aggregates[i].function->alignOfData());

        // If not the last aggregate_state, we need pad it so that next aggregate_state will be aligned.
        if (i + 1 < params.aggregates_size)
        {
            size_t alignment_of_next_state = params.aggregates[i + 1].function->alignOfData();
            if ((alignment_of_next_state & (alignment_of_next_state - 1)) != 0)
                throw Exception("Logical error: alignOfData is not 2^N", ErrorCodes::LOGICAL_ERROR);

            /// Extend total_size to next alignment requirement
            /// Add padding by rounding up 'total_size_of_aggregate_states' to be a multiplier of alignment_of_next_state.
            total_size_of_aggregate_states = (total_size_of_aggregate_states + alignment_of_next_state - 1) / alignment_of_next_state * alignment_of_next_state;
        }

        if (!params.aggregates[i].function->hasTrivialDestructor())
            all_aggregates_has_trivial_destructor = false;
    }

    method_chosen = chooseAggregationMethod();
    HashMethodContext::Settings cache_settings;
    cache_settings.max_threads = params.max_threads;
    aggregation_state_cache = AggregatedDataVariants::createCache(method_chosen, cache_settings);

#if USE_EMBEDDED_COMPILER
    compileAggregateFunctionsIfNeeded();
#endif
}

#if USE_EMBEDDED_COMPILER

void Aggregator::compileAggregateFunctionsIfNeeded()
{
    static std::unordered_map<UInt128, UInt64, UInt128Hash> aggregate_functions_description_to_count;
    static std::mutex mtx;

    if (!params.compile_aggregate_expressions)
        return;

    std::vector<AggregateFunctionWithOffset> functions_to_compile;
    String functions_description;

    is_aggregate_function_compiled.resize(aggregate_functions.size());

    /// Add values to the aggregate functions.
    for (size_t i = 0; i < aggregate_functions.size(); ++i)
    {
        const auto * function = aggregate_functions[i];
        size_t offset_of_aggregate_function = offsets_of_aggregate_states[i];

        if (function->isCompilable())
        {
            AggregateFunctionWithOffset function_to_compile
            {
                .function = function,
                .aggregate_data_offset = offset_of_aggregate_function
            };

            functions_to_compile.emplace_back(std::move(function_to_compile));

            functions_description += function->getDescription();
            functions_description += ' ';

            functions_description += std::to_string(offset_of_aggregate_function);
            functions_description += ' ';
        }

        is_aggregate_function_compiled[i] = function->isCompilable();
    }

    if (functions_to_compile.empty())
        return;

    SipHash aggregate_functions_description_hash;
    aggregate_functions_description_hash.update(functions_description);

    UInt128 aggregate_functions_description_hash_key;
    aggregate_functions_description_hash.get128(aggregate_functions_description_hash_key);

    {
        std::lock_guard<std::mutex> lock(mtx);

        if (aggregate_functions_description_to_count[aggregate_functions_description_hash_key]++ < params.min_count_to_compile_aggregate_expression)
            return;

        if (auto * compilation_cache = CompiledExpressionCacheFactory::instance().tryGetCache())
        {
            auto [compiled_function_cache_entry, _] = compilation_cache->getOrSet(aggregate_functions_description_hash_key, [&] ()
            {
                LOG_TRACE(log, "Compile expression {}", functions_description);

                auto compiled_aggregate_functions = compileAggregateFunctions(getJITInstance(), functions_to_compile, functions_description);
                return std::make_shared<CompiledAggregateFunctionsHolder>(std::move(compiled_aggregate_functions));
            });

            compiled_aggregate_functions_holder = std::static_pointer_cast<CompiledAggregateFunctionsHolder>(compiled_function_cache_entry);
        }
        else
        {
            LOG_TRACE(log, "Compile expression {}", functions_description);
            auto compiled_aggregate_functions = compileAggregateFunctions(getJITInstance(), functions_to_compile, functions_description);
            compiled_aggregate_functions_holder = std::make_shared<CompiledAggregateFunctionsHolder>(std::move(compiled_aggregate_functions));
        }
    }
}

#endif

AggregatedDataVariants::Type Aggregator::chooseAggregationMethod()
{
    /// If no keys. All aggregating to single row.
    if (params.keys_size == 0)
        return AggregatedDataVariants::Type::without_key;

    /// Check if at least one of the specified keys is nullable.
    DataTypes types_removed_nullable;
    types_removed_nullable.reserve(params.keys.size());
    bool has_nullable_key = false;
    bool has_low_cardinality = false;

    for (const auto & pos : params.keys)
    {
        DataTypePtr type = (params.src_header ? params.src_header : params.intermediate_header).safeGetByPosition(pos).type;

        if (type->lowCardinality())
        {
            has_low_cardinality = true;
            type = removeLowCardinality(type);
        }

        if (type->isNullable())
        {
            has_nullable_key = true;
            type = removeNullable(type);
        }

        types_removed_nullable.push_back(type);
    }

    /** Returns ordinary (not two-level) methods, because we start from them.
      * Later, during aggregation process, data may be converted (partitioned) to two-level structure, if cardinality is high.
      */

    size_t keys_bytes = 0;
    size_t num_fixed_contiguous_keys = 0;

    key_sizes.resize(params.keys_size);
    for (size_t j = 0; j < params.keys_size; ++j)
    {
        if (types_removed_nullable[j]->isValueUnambiguouslyRepresentedInContiguousMemoryRegion())
        {
            if (types_removed_nullable[j]->isValueUnambiguouslyRepresentedInFixedSizeContiguousMemoryRegion())
            {
                ++num_fixed_contiguous_keys;
                key_sizes[j] = types_removed_nullable[j]->getSizeOfValueInMemory();
                keys_bytes += key_sizes[j];
            }
        }
    }

    /// proton: starts
    auto method_type = chooseAggregationMethodTimeBucketTwoLevel(
        types_removed_nullable, has_nullable_key, has_low_cardinality, num_fixed_contiguous_keys, keys_bytes);
    if (method_type != AggregatedDataVariants::Type::EMPTY)
        return method_type;
    /// proton: ends

    if (has_nullable_key)
    {
        if (params.keys_size == num_fixed_contiguous_keys && !has_low_cardinality)
        {
            /// Pack if possible all the keys along with information about which key values are nulls
            /// into a fixed 16- or 32-byte blob.
            if (std::tuple_size<KeysNullMap<UInt128>>::value + keys_bytes <= 16)
                return AggregatedDataVariants::Type::nullable_keys128;
            if (std::tuple_size<KeysNullMap<UInt256>>::value + keys_bytes <= 32)
                return AggregatedDataVariants::Type::nullable_keys256;
        }

        if (has_low_cardinality && params.keys_size == 1)
        {
            if (types_removed_nullable[0]->isValueRepresentedByNumber())
            {
                size_t size_of_field = types_removed_nullable[0]->getSizeOfValueInMemory();

                if (size_of_field == 1)
                    return AggregatedDataVariants::Type::low_cardinality_key8;
                if (size_of_field == 2)
                    return AggregatedDataVariants::Type::low_cardinality_key16;
                if (size_of_field == 4)
                    return AggregatedDataVariants::Type::low_cardinality_key32;
                if (size_of_field == 8)
                    return AggregatedDataVariants::Type::low_cardinality_key64;
            }
            else if (isString(types_removed_nullable[0]))
                return AggregatedDataVariants::Type::low_cardinality_key_string;
            else if (isFixedString(types_removed_nullable[0]))
                return AggregatedDataVariants::Type::low_cardinality_key_fixed_string;
        }

        /// Fallback case.
        return AggregatedDataVariants::Type::serialized;
    }

    /// No key has been found to be nullable.

    /// Single numeric key.
    if (params.keys_size == 1 && types_removed_nullable[0]->isValueRepresentedByNumber())
    {
        size_t size_of_field = types_removed_nullable[0]->getSizeOfValueInMemory();

        if (has_low_cardinality)
        {
            if (size_of_field == 1)
                return AggregatedDataVariants::Type::low_cardinality_key8;
            if (size_of_field == 2)
                return AggregatedDataVariants::Type::low_cardinality_key16;
            if (size_of_field == 4)
                return AggregatedDataVariants::Type::low_cardinality_key32;
            if (size_of_field == 8)
                return AggregatedDataVariants::Type::low_cardinality_key64;
            if (size_of_field == 16)
                return AggregatedDataVariants::Type::low_cardinality_keys128;
            if (size_of_field == 32)
                return AggregatedDataVariants::Type::low_cardinality_keys256;
            throw Exception("Logical error: low cardinality numeric column has sizeOfField not in 1, 2, 4, 8, 16, 32.", ErrorCodes::LOGICAL_ERROR);
        }

        if (size_of_field == 1)
            return AggregatedDataVariants::Type::key8;
        if (size_of_field == 2)
            return AggregatedDataVariants::Type::key16;
        if (size_of_field == 4)
            return AggregatedDataVariants::Type::key32;
        if (size_of_field == 8)
            return AggregatedDataVariants::Type::key64;
        if (size_of_field == 16)
            return AggregatedDataVariants::Type::keys128;
        if (size_of_field == 32)
            return AggregatedDataVariants::Type::keys256;
        throw Exception("Logical error: numeric column has sizeOfField not in 1, 2, 4, 8, 16, 32.", ErrorCodes::LOGICAL_ERROR);
    }

    if (params.keys_size == 1 && isFixedString(types_removed_nullable[0]))
    {
        if (has_low_cardinality)
            return AggregatedDataVariants::Type::low_cardinality_key_fixed_string;
        else
            return AggregatedDataVariants::Type::key_fixed_string;
    }

    /// If all keys fits in N bits, will use hash table with all keys packed (placed contiguously) to single N-bit key.
    if (params.keys_size == num_fixed_contiguous_keys)
    {
        if (has_low_cardinality)
        {
            if (keys_bytes <= 16)
                return AggregatedDataVariants::Type::low_cardinality_keys128;
            if (keys_bytes <= 32)
                return AggregatedDataVariants::Type::low_cardinality_keys256;
        }

        if (keys_bytes <= 2)
            return AggregatedDataVariants::Type::keys16;
        if (keys_bytes <= 4)
            return AggregatedDataVariants::Type::keys32;
        if (keys_bytes <= 8)
            return AggregatedDataVariants::Type::keys64;
        if (keys_bytes <= 16)
            return AggregatedDataVariants::Type::keys128;
        if (keys_bytes <= 32)
            return AggregatedDataVariants::Type::keys256;
    }

    /// If single string key - will use hash table with references to it. Strings itself are stored separately in Arena.
    if (params.keys_size == 1 && isString(types_removed_nullable[0]))
    {
        if (has_low_cardinality)
            return AggregatedDataVariants::Type::low_cardinality_key_string;
        else
            return AggregatedDataVariants::Type::key_string;
    }

    return AggregatedDataVariants::Type::serialized;
}

/// proton: starts
AggregatedDataVariants::Type Aggregator::chooseAggregationMethodTimeBucketTwoLevel(
    const DataTypes & types_removed_nullable, bool has_nullable_key,
    bool has_low_cardinality, size_t num_fixed_contiguous_keys, size_t keys_bytes) const
{
    if (params.group_by != Params::GroupBy::WINDOW_END
        && params.group_by != Params::GroupBy::WINDOW_START)
        return AggregatedDataVariants::Type::EMPTY;

    if (has_nullable_key)
    {
        if (params.keys_size == num_fixed_contiguous_keys && !has_low_cardinality)
        {
            /// Pack if possible all the keys along with information about which key values are nulls
            /// into a fixed 16- or 32-byte blob.
            if (std::tuple_size<KeysNullMap<UInt128>>::value + keys_bytes <= 16)
                return AggregatedDataVariants::Type::time_bucket_nullable_keys128_two_level;
            if (std::tuple_size<KeysNullMap<UInt256>>::value + keys_bytes <= 32)
                return AggregatedDataVariants::Type::time_bucket_nullable_keys256_two_level;
        }

        /// Fallback case.
        return AggregatedDataVariants::Type::serialized;
    }

    /// No key has been found to be nullable.

    /// Single numeric key.
    if (params.keys_size == 1)
    {
        assert(types_removed_nullable[0]->isValueRepresentedByNumber());

        size_t size_of_field = types_removed_nullable[0]->getSizeOfValueInMemory();

        if (size_of_field == 2)
            return AggregatedDataVariants::Type::time_bucket_key16_two_level;
        if (size_of_field == 4)
            return AggregatedDataVariants::Type::time_bucket_key32_two_level;
        if (size_of_field == 8)
            return AggregatedDataVariants::Type::time_bucket_key64_two_level;

        throw Exception("Logical error: the first streaming aggregation column has sizeOfField not in 2, 4, 8.", ErrorCodes::LOGICAL_ERROR);
    }

    /// If all keys fits in N bits, will use hash table with all keys packed (placed contiguously) to single N-bit key.
    if (params.keys_size == num_fixed_contiguous_keys)
    {
        assert(keys_bytes > 2);

        if (has_low_cardinality)
        {
            if (keys_bytes <= 16)
                return AggregatedDataVariants::Type::time_bucket_low_cardinality_keys128_two_level;
            if (keys_bytes <= 32)
                return AggregatedDataVariants::Type::time_bucket_low_cardinality_keys256_two_level;
        }

        if (keys_bytes <= 4)
            return AggregatedDataVariants::Type::time_bucket_keys32_two_level;
        if (keys_bytes <= 8)
            return AggregatedDataVariants::Type::time_bucket_keys64_two_level;
        if (keys_bytes <= 16)
            return AggregatedDataVariants::Type::time_bucket_keys128_two_level;
        if (keys_bytes <= 32)
            return AggregatedDataVariants::Type::time_bucket_keys256_two_level;
    }

    return AggregatedDataVariants::Type::time_bucket_serialized_two_level;
}
/// proton: ends

template <bool skip_compiled_aggregate_functions>
void Aggregator::createAggregateStates(AggregateDataPtr & aggregate_data) const
{
    for (size_t j = 0; j < params.aggregates_size; ++j)
    {
        if constexpr (skip_compiled_aggregate_functions)
            if (is_aggregate_function_compiled[j])
                continue;

        try
        {
            /** An exception may occur if there is a shortage of memory.
              * In order that then everything is properly destroyed, we "roll back" some of the created states.
              * The code is not very convenient.
              */
            aggregate_functions[j]->create(aggregate_data + offsets_of_aggregate_states[j]);
        }
        catch (...)
        {
            for (size_t rollback_j = 0; rollback_j < j; ++rollback_j)
            {
                if constexpr (skip_compiled_aggregate_functions)
                    if (is_aggregate_function_compiled[j])
                        continue;

                aggregate_functions[rollback_j]->destroy(aggregate_data + offsets_of_aggregate_states[rollback_j]);
            }

            throw;
        }
    }
}

[[nodiscard]] bool Aggregator::executeImpl(
    AggregatedDataVariants & result,
    size_t row_begin,
    size_t row_end,
    ColumnRawPtrs & key_columns,
    AggregateFunctionInstruction * aggregate_instructions,
    bool no_more_keys,
    AggregateDataPtr overflow_row) const
{
    #define M(NAME, IS_TWO_LEVEL) \
        else if (result.type == AggregatedDataVariants::Type::NAME) \
            return executeImpl(*result.NAME, result.aggregates_pool, row_begin, row_end, key_columns, aggregate_instructions, no_more_keys, overflow_row);

    if (false) {} // NOLINT
    APPLY_FOR_AGGREGATED_VARIANTS_STREAMING(M)
    #undef M

    return false;
}

/** It's interesting - if you remove `noinline`, then gcc for some reason will inline this function, and the performance decreases (~ 10%).
  * (Probably because after the inline of this function, more internal functions no longer be inlined.)
  * Inline does not make sense, since the inner loop is entirely inside this function.
  */
template <typename Method>
[[nodiscard]] bool NO_INLINE Aggregator::executeImpl(
    Method & method,
    Arena * aggregates_pool,
    size_t row_begin,
    size_t row_end,
    ColumnRawPtrs & key_columns,
    AggregateFunctionInstruction * aggregate_instructions,
    bool no_more_keys,
    AggregateDataPtr overflow_row) const
{
    typename Method::State state(key_columns, key_sizes, aggregation_state_cache);

    if (!no_more_keys)
    {
#if USE_EMBEDDED_COMPILER
        if (compiled_aggregate_functions_holder)
        {
            return executeImplBatch<false, true>(method, state, aggregates_pool, row_begin, row_end, aggregate_instructions, overflow_row);
        }
        else
#endif
        {
            return executeImplBatch<false, false>(method, state, aggregates_pool, row_begin, row_end, aggregate_instructions, overflow_row);
        }
    }
    else
    {
        return executeImplBatch<true, false>(method, state, aggregates_pool, row_begin, row_end, aggregate_instructions, overflow_row);
    }
}

template <bool no_more_keys, bool use_compiled_functions, typename Method>
[[nodiscard]] bool NO_INLINE Aggregator::executeImplBatch(
    Method & method,
    typename Method::State & state,
    Arena * aggregates_pool,
    size_t row_begin,
    size_t row_end,
    AggregateFunctionInstruction * aggregate_instructions,
    AggregateDataPtr overflow_row) const
{
    /// Optimization for special case when there are no aggregate functions.
    if (params.aggregates_size == 0)
    {
        if constexpr (no_more_keys)
            return false;

        /// For all rows.
        AggregateDataPtr place = aggregates_pool->alloc(0);
        for (size_t i = row_begin; i < row_end; ++i)
            state.emplaceKey(method.data, i, *aggregates_pool).setMapped(place);
        return false;
    }

    bool need_finalization = false;

    /// Optimization for special case when aggregating by 8bit key.
    if constexpr (!no_more_keys && std::is_same_v<Method, typename decltype(AggregatedDataVariants::key8)::element_type>)
    {
        /// We use another method if there are aggregate functions with -Array combinator.
        bool has_arrays = false;
        for (AggregateFunctionInstruction * inst = aggregate_instructions; inst->that; ++inst)
        {
            if (inst->offsets)
            {
                has_arrays = true;
                break;
            }
        }

        if (!has_arrays)
        {
            for (AggregateFunctionInstruction * inst = aggregate_instructions; inst->that; ++inst)
            {
                inst->batch_that->addBatchLookupTable8(
                    row_begin,
                    row_end,
                    reinterpret_cast<AggregateDataPtr *>(method.data.data()),
                    inst->state_offset,
                    [&](AggregateDataPtr & aggregate_data)
                    {
                        aggregate_data = aggregates_pool->alignedAlloc(total_size_of_aggregate_states, align_aggregate_states);
                        createAggregateStates(aggregate_data);
                    },
                    state.getKeyData(),
                    inst->batch_arguments,
                    aggregates_pool);

                /// Calculate if we need finalization
                if (!need_finalization && inst->batch_that->isUserDefined())
                {
                    auto * map = reinterpret_cast<AggregateDataPtr *>(method.data.data());
                    const auto * key = state.getKeyData();
                    for (size_t cur = row_begin; !need_finalization && cur < row_end; ++cur)
                        need_finalization = (inst->batch_that->getEmitTimes(map[key[cur]] + inst->state_offset) > 0);
                }
            }
            return need_finalization;
        }
    }

    /// NOTE: only row_end-row_start is required, but:
    /// - this affects only optimize_aggregation_in_order,
    /// - this is just a pointer, so it should not be significant,
    /// - and plus this will require other changes in the interface.
    std::unique_ptr<AggregateDataPtr[]> places(new AggregateDataPtr[row_end]);

    /// For all rows.
    for (size_t i = row_begin; i < row_end; ++i)
    {
        AggregateDataPtr aggregate_data = nullptr;

        if constexpr (!no_more_keys)
        {
            auto emplace_result = state.emplaceKey(method.data, i, *aggregates_pool);

            /// If a new key is inserted, initialize the states of the aggregate functions, and possibly something related to the key.
            if (emplace_result.isInserted())
            {
                /// exception-safety - if you can not allocate memory or create states, then destructors will not be called.
                emplace_result.setMapped(nullptr);

                aggregate_data = aggregates_pool->alignedAlloc(total_size_of_aggregate_states, align_aggregate_states);

#if USE_EMBEDDED_COMPILER
                if constexpr (use_compiled_functions)
                {
                    const auto & compiled_aggregate_functions = compiled_aggregate_functions_holder->compiled_aggregate_functions;
                    compiled_aggregate_functions.create_aggregate_states_function(aggregate_data);
                    if (compiled_aggregate_functions.functions_count != aggregate_functions.size())
                    {
                        static constexpr bool skip_compiled_aggregate_functions = true;
                        createAggregateStates<skip_compiled_aggregate_functions>(aggregate_data);
                    }

#if defined(MEMORY_SANITIZER)

                    /// We compile only functions that do not allocate some data in Arena. Only store necessary state in AggregateData place.
                    for (size_t aggregate_function_index = 0; aggregate_function_index < aggregate_functions.size(); ++aggregate_function_index)
                    {
                        if (!is_aggregate_function_compiled[aggregate_function_index])
                            continue;

                        auto aggregate_data_with_offset = aggregate_data + offsets_of_aggregate_states[aggregate_function_index];
                        auto data_size = params.aggregates[aggregate_function_index].function->sizeOfData();
                        __msan_unpoison(aggregate_data_with_offset, data_size);
                    }
#endif
                }
                else
#endif
                {
                    createAggregateStates(aggregate_data);
                }

                emplace_result.setMapped(aggregate_data);
            }
            else
                aggregate_data = emplace_result.getMapped();

            assert(aggregate_data != nullptr);
        }
        else
        {
            /// Add only if the key already exists.
            auto find_result = state.findKey(method.data, i, *aggregates_pool);
            if (find_result.isFound())
                aggregate_data = find_result.getMapped();
            else
                aggregate_data = overflow_row;
        }

        places[i] = aggregate_data;
    }

#if USE_EMBEDDED_COMPILER
    if constexpr (use_compiled_functions)
    {
        std::vector<ColumnData> columns_data;

        for (size_t i = 0; i < aggregate_functions.size(); ++i)
        {
            if (!is_aggregate_function_compiled[i])
                continue;

            AggregateFunctionInstruction * inst = aggregate_instructions + i;
            size_t arguments_size = inst->that->getArgumentTypes().size();

            for (size_t argument_index = 0; argument_index < arguments_size; ++argument_index)
                columns_data.emplace_back(getColumnData(inst->batch_arguments[argument_index]));
        }

        auto add_into_aggregate_states_function = compiled_aggregate_functions_holder->compiled_aggregate_functions.add_into_aggregate_states_function;
        add_into_aggregate_states_function(row_begin, row_end, columns_data.data(), places.get());
    }
#endif

    /// Add values to the aggregate functions.
    for (size_t i = 0; i < aggregate_functions.size(); ++i)
    {
#if USE_EMBEDDED_COMPILER
        if constexpr (use_compiled_functions)
            if (is_aggregate_function_compiled[i])
                continue;
#endif

        AggregateFunctionInstruction * inst = aggregate_instructions + i;

        if (inst->offsets)
            inst->batch_that->addBatchArray(row_begin, row_end, places.get(), inst->state_offset, inst->batch_arguments, inst->offsets, aggregates_pool);
        else
            inst->batch_that->addBatch(row_begin, row_end, places.get(), inst->state_offset, inst->batch_arguments, aggregates_pool, -1, inst->delta_column);

        if (inst->batch_that->isUserDefined())
        {
            AggregateDataPtr * places_ptr = places.get();
            /// It is ok to re-flush if it is flush already, then we don't need maintain a map to check if it is ready flushed
            for (size_t j = row_begin; j < row_end; ++j)
            {
                if (places_ptr[j])
                {
                    inst->batch_that->flush(places_ptr[j] + inst->state_offset);
                    if (!need_finalization)
                        need_finalization = (inst->batch_that->getEmitTimes(places_ptr[j] + inst->state_offset) > 0);
                }
            }
        }
    }

    return need_finalization;
}

template <bool use_compiled_functions>
[[nodiscard]] bool NO_INLINE Aggregator::executeWithoutKeyImpl(
    AggregatedDataWithoutKey & res,
    size_t row_begin,
    size_t row_end,
    AggregateFunctionInstruction * aggregate_instructions,
    Arena * arena) const
{
#if USE_EMBEDDED_COMPILER
    if constexpr (use_compiled_functions)
    {
        std::vector<ColumnData> columns_data;

        for (size_t i = 0; i < aggregate_functions.size(); ++i)
        {
            if (!is_aggregate_function_compiled[i])
                continue;

            AggregateFunctionInstruction * inst = aggregate_instructions + i;
            size_t arguments_size = inst->that->getArgumentTypes().size();

            for (size_t argument_index = 0; argument_index < arguments_size; ++argument_index)
            {
                columns_data.emplace_back(getColumnData(inst->batch_arguments[argument_index]));
            }
        }

        auto add_into_aggregate_states_function_single_place = compiled_aggregate_functions_holder->compiled_aggregate_functions.add_into_aggregate_states_function_single_place;
        add_into_aggregate_states_function_single_place(row_begin, row_end, columns_data.data(), res);

#if defined(MEMORY_SANITIZER)

        /// We compile only functions that do not allocate some data in Arena. Only store necessary state in AggregateData place.
        for (size_t aggregate_function_index = 0; aggregate_function_index < aggregate_functions.size(); ++aggregate_function_index)
        {
            if (!is_aggregate_function_compiled[aggregate_function_index])
                continue;

            auto aggregate_data_with_offset = res + offsets_of_aggregate_states[aggregate_function_index];
            auto data_size = params.aggregates[aggregate_function_index].function->sizeOfData();
            __msan_unpoison(aggregate_data_with_offset, data_size);
        }
#endif
    }
#endif

    /// Adding values
    bool should_finalize = false;
    for (size_t i = 0; i < aggregate_functions.size(); ++i)
    {
        AggregateFunctionInstruction * inst = aggregate_instructions + i;

#if USE_EMBEDDED_COMPILER
        if constexpr (use_compiled_functions)
            if (is_aggregate_function_compiled[i])
                continue;
#endif
        if (inst->offsets)
            inst->batch_that->addBatchSinglePlace(
                inst->offsets[static_cast<ssize_t>(row_begin) - 1],
                inst->offsets[row_end - 1],
                res + inst->state_offset,
                inst->batch_arguments,
                arena,
                -1,
                inst->delta_column);
        else
            inst->batch_that->addBatchSinglePlace(
                row_begin,
                row_end,
                res + inst->state_offset,
                inst->batch_arguments,
                arena,
                -1,
                inst->delta_column);

        if (inst->batch_that->isUserDefined())
        {
            inst->batch_that->flush(res + inst->state_offset);

            if (!should_finalize)
                should_finalize = (inst->batch_that->getEmitTimes(res + inst->state_offset) > 0);
        }
    }

    return should_finalize;
}

void NO_INLINE Aggregator::executeOnIntervalWithoutKeyImpl(
    AggregatedDataWithoutKey & res,
    size_t row_begin,
    size_t row_end,
    AggregateFunctionInstruction * aggregate_instructions,
    Arena * arena,
    const IColumn * delta_col)
{
    /// Adding values
    for (AggregateFunctionInstruction * inst = aggregate_instructions; inst->that; ++inst)
    {
        if (inst->offsets)
            inst->batch_that->addBatchSinglePlaceFromInterval(inst->offsets[row_begin], inst->offsets[row_end - 1], res + inst->state_offset, inst->batch_arguments, arena, -1, delta_col);
        else
            inst->batch_that->addBatchSinglePlaceFromInterval(row_begin, row_end, res + inst->state_offset, inst->batch_arguments, arena, -1, delta_col);
    }
}


void Aggregator::prepareAggregateInstructions(Columns columns, AggregateColumns & aggregate_columns, Columns & materialized_columns,
     AggregateFunctionInstructions & aggregate_functions_instructions, NestedColumnsHolder & nested_columns_holder) const
{
    for (size_t i = 0; i < params.aggregates_size; ++i)
        aggregate_columns[i].resize(params.aggregates[i].arguments.size());

    aggregate_functions_instructions.resize(params.aggregates_size + 1);
    aggregate_functions_instructions[params.aggregates_size].that = nullptr;

    for (size_t i = 0; i < params.aggregates_size; ++i)
    {
        for (size_t j = 0; j < aggregate_columns[i].size(); ++j)
        {
            materialized_columns.push_back(columns.at(params.aggregates[i].arguments[j])->convertToFullColumnIfConst());
            aggregate_columns[i][j] = materialized_columns.back().get();

            auto column_no_lc = recursiveRemoveLowCardinality(aggregate_columns[i][j]->getPtr());
            if (column_no_lc.get() != aggregate_columns[i][j])
            {
                materialized_columns.emplace_back(std::move(column_no_lc));
                aggregate_columns[i][j] = materialized_columns.back().get();
            }
        }

        aggregate_functions_instructions[i].arguments = aggregate_columns[i].data();
        aggregate_functions_instructions[i].state_offset = offsets_of_aggregate_states[i];

        const auto * that = aggregate_functions[i];
        /// Unnest consecutive trailing -State combinators
        while (const auto * func = typeid_cast<const AggregateFunctionState *>(that))
            that = func->getNestedFunction().get();
        aggregate_functions_instructions[i].that = that;

        if (const auto * func = typeid_cast<const AggregateFunctionArray *>(that))
        {
            /// Unnest consecutive -State combinators before -Array
            that = func->getNestedFunction().get();
            while (const auto * nested_func = typeid_cast<const AggregateFunctionState *>(that))
                that = nested_func->getNestedFunction().get();
            auto [nested_columns, offsets] = checkAndGetNestedArrayOffset(aggregate_columns[i].data(), that->getArgumentTypes().size());
            nested_columns_holder.push_back(std::move(nested_columns));
            aggregate_functions_instructions[i].batch_arguments = nested_columns_holder.back().data();
            aggregate_functions_instructions[i].offsets = offsets;
        }
        else
            aggregate_functions_instructions[i].batch_arguments = aggregate_columns[i].data();

        aggregate_functions_instructions[i].batch_that = that;

        /// proton : starts
        if (params.delta_col_pos >= 0)
            aggregate_functions_instructions[i].delta_column = columns.at(params.delta_col_pos).get(); /// point to the column point is ok
        /// proton : ends
    }
}

/// return {should_abort, need_finalization}
std::pair<bool, bool> Aggregator::executeOnBlock(
    const Block & block,
    AggregatedDataVariants & result,
    ColumnRawPtrs & key_columns,
    AggregateColumns & aggregate_columns,
    bool & no_more_keys) const
{
    return executeOnBlock(
        block.getColumns(),
        /* row_begin= */ 0,
        block.rows(),
        result,
        key_columns,
        aggregate_columns,
        no_more_keys);
}

/// return {should_abort, need_finalization}
std::pair<bool, bool> Aggregator::executeOnBlock(
    Columns columns,
    size_t row_begin,
    size_t row_end,
    AggregatedDataVariants & result,
    ColumnRawPtrs & key_columns,
    AggregateColumns & aggregate_columns,
    bool & no_more_keys) const
{
    std::pair<bool, bool> return_result = {false, false};
    auto & need_abort = return_result.first;
    auto & need_finalization = return_result.second;

    /// `result will destroy the states of aggregate functions in the destructor
    result.aggregator = this;

    /// How to perform the aggregation?
    if (result.empty())
    {
        /// proton: starts. First init the key_sizes, and then the method since method init
        /// may depend on the key_sizes
        result.keys_size = params.keys_size;
        result.key_sizes = key_sizes;
        result.init(method_chosen);
        /// proton: ends
        LOG_TRACE(log, "Aggregation method: {}", result.getMethodName());
    }

    /** Constant columns are not supported directly during aggregation.
      * To make them work anyway, we materialize them.
      */
    Columns materialized_columns;

    /// Remember the columns we will work with
    for (size_t i = 0; i < params.keys_size; ++i)
    {
        materialized_columns.push_back(columns.at(params.keys[i])->convertToFullColumnIfConst());
        key_columns[i] = materialized_columns.back().get();

        if (!result.isLowCardinality())
        {
            auto column_no_lc = recursiveRemoveLowCardinality(key_columns[i]->getPtr());
            if (column_no_lc.get() != key_columns[i])
            {
                materialized_columns.emplace_back(std::move(column_no_lc));
                key_columns[i] = materialized_columns.back().get();
            }
        }
    }

    /// proton: starts. For window start/end aggregation, we will need setup timestamp of the aggr to
    /// enable memory recycling.
    setupAggregatesPoolTimestamps(row_begin, row_end, key_columns, result.aggregates_pool);
    /// proton: ends

    NestedColumnsHolder nested_columns_holder;
    AggregateFunctionInstructions aggregate_functions_instructions;
    prepareAggregateInstructions(columns, aggregate_columns, materialized_columns, aggregate_functions_instructions, nested_columns_holder);

    initStatesForWithoutKeyOrOverflow(result);

    /// We select one of the aggregation methods and call it.

    /// For the case when there are no keys (all aggregate into one row).
    if (result.type == AggregatedDataVariants::Type::without_key)
    {
        /// TODO: Enable compilation after investigation
// #if USE_EMBEDDED_COMPILER
//         if (compiled_aggregate_functions_holder)
//         {
//             executeWithoutKeyImpl<true>(result.without_key, row_begin, row_end, aggregate_functions_instructions.data(), result.aggregates_pool);
//         }
//         else
// #endif
        {
            need_finalization = executeWithoutKeyImpl<false>(result.without_key, row_begin, row_end, aggregate_functions_instructions.data(), result.aggregates_pool);
        }
    }
    else
    {
        /// This is where data is written that does not fit in `max_rows_to_group_by` with `group_by_overflow_mode = any`.
        AggregateDataPtr overflow_row_ptr = params.overflow_row ? result.without_key : nullptr;
        need_finalization = executeImpl(result, row_begin, row_end, key_columns, aggregate_functions_instructions.data(), no_more_keys, overflow_row_ptr);
    }

    size_t result_size = result.sizeWithoutOverflowRow();
    Int64 current_memory_usage = 0;
    if (auto * memory_tracker_child = CurrentThread::getMemoryTracker())
        if (auto * memory_tracker = memory_tracker_child->getParent())
            current_memory_usage = memory_tracker->get();

    /// Here all the results in the sum are taken into account, from different threads.
    auto result_size_bytes = current_memory_usage - memory_usage_before_aggregation;

    bool worth_convert_to_two_level = worthConvertToTwoLevel(
        params.group_by_two_level_threshold, result_size, params.group_by_two_level_threshold_bytes, result_size_bytes);

    /** Converting to a two-level data structure.
      * It allows you to make, in the subsequent, an effective merge - either economical from memory or parallel.
      */
    if (result.isConvertibleToTwoLevel() && worth_convert_to_two_level)
        result.convertToTwoLevel();

    /// Checking the constraints.
    if (!checkLimits(result_size, no_more_keys))
    {
        need_abort = true;
        return return_result;
    }

    /** Flush data to disk if too much RAM is consumed.
      * Data can only be flushed to disk if a two-level aggregation structure is used.
      */
    if (params.max_bytes_before_external_group_by
        && result.isTwoLevel()
        && current_memory_usage > static_cast<Int64>(params.max_bytes_before_external_group_by)
        && worth_convert_to_two_level)
    {
        size_t size = current_memory_usage + params.min_free_disk_space;

        std::string tmp_path = params.tmp_volume->getDisk()->getPath();

        // enoughSpaceInDirectory() is not enough to make it right, since
        // another process (or another thread of aggregator) can consume all
        // space.
        //
        // But true reservation (IVolume::reserve()) cannot be used here since
        // current_memory_usage does not take compression into account and
        // will reserve way more that actually will be used.
        //
        // Hence, let's do a simple check.
        if (!enoughSpaceInDirectory(tmp_path, size))
            throw Exception("Not enough space for external aggregation in " + tmp_path, ErrorCodes::NOT_ENOUGH_SPACE);

        writeToTemporaryFile(result, tmp_path);
    }

    return return_result;
}

void Aggregator::writeToTemporaryFile(AggregatedDataVariants & data_variants, const String & tmp_path) const
{
    Stopwatch watch;
    size_t rows = data_variants.size();

    auto file = createTemporaryFile(tmp_path);
    const std::string & path = file->path();
    WriteBufferFromFile file_buf(path);
    CompressedWriteBuffer compressed_buf(file_buf);
    NativeWriter block_out(compressed_buf, getHeader(false), ProtonRevision::getVersionRevision());

    LOG_DEBUG(log, "Writing part of aggregation data into temporary file {}.", path);
    ProfileEvents::increment(ProfileEvents::ExternalAggregationWritePart);

    /// Flush only two-level data and possibly overflow data.

#define M(NAME) \
    else if (data_variants.type == AggregatedDataVariants::Type::NAME) \
        writeToTemporaryFileImpl(data_variants, *data_variants.NAME, block_out);

    if (false) {} // NOLINT
    APPLY_FOR_VARIANTS_ALL_TWO_LEVEL(M)
#undef M
    else
        throw Exception("Unknown aggregated data variant.", ErrorCodes::UNKNOWN_AGGREGATED_DATA_VARIANT);

    /// NOTE Instead of freeing up memory and creating new hash tables and arenas, you can re-use the old ones.
    data_variants.init(data_variants.type);
    data_variants.aggregates_pools = Arenas(1, std::make_shared<Arena>());
    data_variants.aggregates_pool = data_variants.aggregates_pools.back().get();
    initStatesForWithoutKeyOrOverflow(data_variants);

    block_out.flush();
    compressed_buf.next();
    file_buf.next();

    double elapsed_seconds = watch.elapsedSeconds();
    size_t compressed_bytes = file_buf.count();
    size_t uncompressed_bytes = compressed_buf.count();

    {
        std::lock_guard lock(temporary_files.mutex);
        temporary_files.files.emplace_back(std::move(file));
        temporary_files.sum_size_uncompressed += uncompressed_bytes;
        temporary_files.sum_size_compressed += compressed_bytes;
    }

    ProfileEvents::increment(ProfileEvents::ExternalAggregationCompressedBytes, compressed_bytes);
    ProfileEvents::increment(ProfileEvents::ExternalAggregationUncompressedBytes, uncompressed_bytes);

    LOG_DEBUG(log,
        "Written part in {:.3f} sec., {} rows, {} uncompressed, {} compressed,"
        " {:.3f} uncompressed bytes per row, {:.3f} compressed bytes per row, compression rate: {:.3f}"
        " ({:.3f} rows/sec., {}/sec. uncompressed, {}/sec. compressed)",
        elapsed_seconds,
        rows,
        ReadableSize(uncompressed_bytes),
        ReadableSize(compressed_bytes),
        static_cast<double>(uncompressed_bytes) / rows,
        static_cast<double>(compressed_bytes) / rows,
        static_cast<double>(uncompressed_bytes) / compressed_bytes,
        rows / elapsed_seconds,
        ReadableSize(static_cast<double>(uncompressed_bytes) / elapsed_seconds),
        ReadableSize(static_cast<double>(compressed_bytes) / elapsed_seconds));
}


void Aggregator::writeToTemporaryFile(AggregatedDataVariants & data_variants) const
{
    String tmp_path = params.tmp_volume->getDisk()->getPath();
    return writeToTemporaryFile(data_variants, tmp_path);
}


template <typename Method>
Block Aggregator::convertOneBucketToBlock(
    AggregatedDataVariants & data_variants,
    Method & method,
    Arena * arena,
    bool final,
    ConvertAction action,
    size_t bucket) const
{
    Block block = prepareBlockAndFill(data_variants, final, action, method.data.impls[bucket].size(),
        [bucket, &method, arena, this] (
            MutableColumns & key_columns,
            AggregateColumnsData & aggregate_columns,
            MutableColumns & final_aggregate_columns,
            bool final_,
            ConvertAction action_)
        {
            convertToBlockImpl(method, method.data.impls[bucket],
                key_columns, aggregate_columns, final_aggregate_columns, arena, final_, action_);
        });

    block.info.bucket_num = static_cast<Int32>(bucket);
    return block;
}

Block Aggregator::convertOneBucketToBlockFinal(AggregatedDataVariants & variants, ConvertAction action, size_t bucket) const
{
    auto method = variants.type;
    Block block;

    if (false) {} // NOLINT
#define M(NAME) \
    else if (method == AggregatedDataVariants::Type::NAME) \
        block = convertOneBucketToBlock(variants, *variants.NAME, variants.aggregates_pool, true, action, bucket);

    APPLY_FOR_VARIANTS_TIME_BUCKET_TWO_LEVEL(M)
#undef M
    else
        throw Exception("Unknown aggregated data variant.", ErrorCodes::UNKNOWN_AGGREGATED_DATA_VARIANT);

    return block;
}

Block Aggregator::convertOneBucketToBlockIntermediate(AggregatedDataVariants & variants, ConvertAction action, size_t bucket) const
{
    auto method = variants.type;
    Block block;

    if (false) {} // NOLINT
#define M(NAME) \
    else if (method == AggregatedDataVariants::Type::NAME) \
        block = convertOneBucketToBlock(variants, *variants.NAME, variants.aggregates_pool, false, action, bucket);

    APPLY_FOR_VARIANTS_TIME_BUCKET_TWO_LEVEL(M)
#undef M
    else
        throw Exception("Unknown aggregated data variant.", ErrorCodes::UNKNOWN_AGGREGATED_DATA_VARIANT);

    return block;
}

Block Aggregator::mergeAndConvertOneBucketToBlock(
    ManyAggregatedDataVariants & variants,
    Arena * arena,
    bool final,
    ConvertAction action,
    size_t bucket,
    std::atomic<bool> * is_cancelled) const
{
    auto & merged_data = *variants[0];
    auto method = merged_data.type;
    Block block;

    if (false) {} // NOLINT
#define M(NAME) \
    else if (method == AggregatedDataVariants::Type::NAME) \
    { \
        mergeBucketImpl<decltype(merged_data.NAME)::element_type>(variants, final, action, bucket, arena); \
        if (is_cancelled && is_cancelled->load(std::memory_order_seq_cst)) \
            return {}; \
        block = convertOneBucketToBlock(merged_data, *merged_data.NAME, arena, final, action, bucket); \
    }

    APPLY_FOR_VARIANTS_ALL_TWO_LEVEL(M)
#undef M

    return block;
}


template <typename Method>
void Aggregator::writeToTemporaryFileImpl(
    AggregatedDataVariants & data_variants,
    Method & method,
    NativeWriter & out) const
{
    size_t max_temporary_block_size_rows = 0;
    size_t max_temporary_block_size_bytes = 0;

    auto update_max_sizes = [&](const Block & block)
    {
        size_t block_size_rows = block.rows();
        size_t block_size_bytes = block.bytes();

        if (block_size_rows > max_temporary_block_size_rows)
            max_temporary_block_size_rows = block_size_rows;
        if (block_size_bytes > max_temporary_block_size_bytes)
            max_temporary_block_size_bytes = block_size_bytes;
    };

    for (auto bucket : method.data.buckets())
    {
        Block block = convertOneBucketToBlock(data_variants, method, data_variants.aggregates_pool, false, ConvertAction::WRITE_TO_TEMP_FS, bucket);
        out.write(block);
        update_max_sizes(block);
    }

    if (params.overflow_row)
    {
        Block block = prepareBlockAndFillWithoutKey(data_variants, false, true, ConvertAction::WRITE_TO_TEMP_FS);
        out.write(block);
        update_max_sizes(block);
    }

    /// Pass ownership of the aggregate functions states:
    /// `data_variants` will not destroy them in the destructor, they are now owned by ColumnAggregateFunction objects.
    data_variants.aggregator = nullptr;

    LOG_DEBUG(log, "Max size of temporary block: {} rows, {}.", max_temporary_block_size_rows, ReadableSize(max_temporary_block_size_bytes));
}


bool Aggregator::checkLimits(size_t result_size, bool & no_more_keys) const
{
    if (!no_more_keys && params.max_rows_to_group_by && result_size > params.max_rows_to_group_by)
    {
        switch (params.group_by_overflow_mode)
        {
            case OverflowMode::THROW:
                throw Exception("Limit for rows to GROUP BY exceeded: has " + toString(result_size)
                    + " rows, maximum: " + toString(params.max_rows_to_group_by),
                    ErrorCodes::TOO_MANY_ROWS);

            case OverflowMode::BREAK:
                return false;

            case OverflowMode::ANY:
                no_more_keys = true;
                break;
        }
    }

    /// Some aggregate functions cannot throw exceptions on allocations (e.g. from C malloc)
    /// but still tracks memory. Check it here.
    CurrentMemoryTracker::check();
    return true;
}


template <typename Method, typename Table>
void Aggregator::convertToBlockImpl(
    Method & method,
    Table & data,
    MutableColumns & key_columns,
    AggregateColumnsData & aggregate_columns,
    MutableColumns & final_aggregate_columns,
    Arena * arena,
    bool final,
    ConvertAction action) const
{
    if (data.empty())
        return;

    if (key_columns.size() != params.keys_size)
        throw Exception{"Aggregate. Unexpected key columns size.", ErrorCodes::LOGICAL_ERROR};

    std::vector<IColumn *> raw_key_columns;
    raw_key_columns.reserve(key_columns.size());
    for (auto & column : key_columns)
        raw_key_columns.push_back(column.get());

    if (final)
    {
#if USE_EMBEDDED_COMPILER
        if (compiled_aggregate_functions_holder)
        {
            static constexpr bool use_compiled_functions = !Method::low_cardinality_optimization;
            convertToBlockImplFinal<Method, use_compiled_functions>(method, data, std::move(raw_key_columns), final_aggregate_columns, arena, action);
        }
        else
#endif
        {
            convertToBlockImplFinal<Method, false>(method, data, std::move(raw_key_columns), final_aggregate_columns, arena, action);
        }
    }
    else
    {
        convertToBlockImplNotFinal(method, data, std::move(raw_key_columns), aggregate_columns);
    }

    /// In order to release memory early.
    /// proton: starts. For streaming aggr, we hold on to the states
    if (shouldClearStates(action, final))
        data.clearAndShrink();
    /// proton: ends
}


template <typename Mapped>
inline void Aggregator::insertAggregatesIntoColumns(
    Mapped & mapped,
    MutableColumns & final_aggregate_columns,
    Arena * arena) const
{
    /** Final values of aggregate functions are inserted to columns.
      * Then states of aggregate functions, that are not longer needed, are destroyed.
      *
      * We mark already destroyed states with "nullptr" in data,
      *  so they will not be destroyed in destructor of Aggregator
      * (other values will be destroyed in destructor in case of exception).
      *
      * But it becomes tricky, because we have multiple aggregate states pointed by a single pointer in data.
      * So, if exception is thrown in the middle of moving states for different aggregate functions,
      *  we have to catch exceptions and destroy all the states that are no longer needed,
      *  to keep the data in consistent state.
      *
      * It is also tricky, because there are aggregate functions with "-State" modifier.
      * When we call "insertResultInto" for them, they insert a pointer to the state to ColumnAggregateFunction
      *  and ColumnAggregateFunction will take ownership of this state.
      * So, for aggregate functions with "-State" modifier, the state must not be destroyed
      *  after it has been transferred to ColumnAggregateFunction.
      * But we should mark that the data no longer owns these states.
      */

    size_t insert_i = 0;
    std::exception_ptr exception;

    try
    {
        /// Insert final values of aggregate functions into columns.
        for (; insert_i < params.aggregates_size; ++insert_i)
            aggregate_functions[insert_i]->insertResultInto(
                mapped + offsets_of_aggregate_states[insert_i], *final_aggregate_columns[insert_i], arena);
    }
    catch (...)
    {
        exception = std::current_exception();
    }

    /// proton: starts
    /// For streaming aggregation, we hold up to the states
    if (params.keep_state)
    {
        if (exception)
            std::rethrow_exception(exception);

        return;
    }
    /// proton: ends

    /** Destroy states that are no longer needed. This loop does not throw.
        *
        * Don't destroy states for "-State" aggregate functions,
        *  because the ownership of this state is transferred to ColumnAggregateFunction
        *  and ColumnAggregateFunction will take care.
        *
        * But it's only for states that has been transferred to ColumnAggregateFunction
        *  before exception has been thrown;
        */
    for (size_t destroy_i = 0; destroy_i < params.aggregates_size; ++destroy_i)
    {
        /// If ownership was not transferred to ColumnAggregateFunction.
        if (!(destroy_i < insert_i && aggregate_functions[destroy_i]->isState()))
            aggregate_functions[destroy_i]->destroy(
                mapped + offsets_of_aggregate_states[destroy_i]);
    }

    /// Mark the cell as destroyed so it will not be destroyed in destructor.
    mapped = nullptr;

    if (exception)
        std::rethrow_exception(exception);
}


template <typename Method, bool use_compiled_functions, typename Table>
void NO_INLINE Aggregator::convertToBlockImplFinal(
    Method & method,
    Table & data,
    std::vector<IColumn *>  key_columns,
    MutableColumns & final_aggregate_columns,
    Arena * arena,
    ConvertAction action) const
{
    if constexpr (Method::low_cardinality_optimization)
    {
        if (data.hasNullKeyData())
        {
            key_columns[0]->insertDefault();
            insertAggregatesIntoColumns(data.getNullKeyData(), final_aggregate_columns, arena);
        }
    }

    auto shuffled_key_sizes = method.shuffleKeyColumns(key_columns, key_sizes);
    const auto & key_sizes_ref = shuffled_key_sizes ? *shuffled_key_sizes :  key_sizes;

    PaddedPODArray<AggregateDataPtr> places;
    places.reserve(data.size());

    auto clear_states = shouldClearStates(action, true);
    data.forEachValue([&](const auto & key, auto & mapped)
    {
        /// For UDA with own emit strategy, there are two special cases to be handled:
        /// 1. not all groups need to  be emitted. therefore proton needs to pick groups
        /// that should emits, and only emit those groups while keep other groups unchanged.
        /// 2. a single block trigger multiple emits. In this case, proton need insert the
        /// same key multiple times for each emit result of this group.

        /// for non-UDA or UDA without emit strategy, 'should_emit' is always true.
        /// For UDA with emit strategy, it is true only if the group should emit.
        size_t emit_times = 1;
        if (params.group_by == Params::GroupBy::USER_DEFINED)
        {
            assert(aggregate_functions.size() == 1);
            emit_times = aggregate_functions[0]->getEmitTimes(mapped + offsets_of_aggregate_states[0]);
        }

        if (emit_times > 0)
        {
            /// duplicate key for each emit
            for (size_t i = 0; i < emit_times; i++)
                method.insertKeyIntoColumns(key, key_columns, key_sizes_ref);
            places.emplace_back(mapped);

            /// Mark the cell as destroyed so it will not be destroyed in destructor.
            /// proton: starts. Here we push the `mapped` to `places`, for streaming
            /// case, we don't want aggregate function to destroy the places
            if (clear_states)
                mapped = nullptr;
        }
    });

    std::exception_ptr exception;
    size_t aggregate_functions_destroy_index = 0;

    try
    {
#if USE_EMBEDDED_COMPILER
        if constexpr (use_compiled_functions)
        {
            /** For JIT compiled functions we need to resize columns before pass them into compiled code.
              * insert_aggregates_into_columns_function function does not throw exception.
              */
            std::vector<ColumnData> columns_data;

            auto compiled_functions = compiled_aggregate_functions_holder->compiled_aggregate_functions;

            for (size_t i = 0; i < params.aggregates_size; ++i)
            {
                if (!is_aggregate_function_compiled[i])
                    continue;

                auto & final_aggregate_column = final_aggregate_columns[i];
                final_aggregate_column = final_aggregate_column->cloneResized(places.size());
                columns_data.emplace_back(getColumnData(final_aggregate_column.get()));
            }

            auto insert_aggregates_into_columns_function = compiled_functions.insert_aggregates_into_columns_function;
            insert_aggregates_into_columns_function(0, places.size(), columns_data.data(), places.data());
        }
#endif

        for (; aggregate_functions_destroy_index < params.aggregates_size;)
        {
            if constexpr (use_compiled_functions)
            {
                if (is_aggregate_function_compiled[aggregate_functions_destroy_index])
                {
                    ++aggregate_functions_destroy_index;
                    continue;
                }
            }

            auto & final_aggregate_column = final_aggregate_columns[aggregate_functions_destroy_index];
            size_t offset = offsets_of_aggregate_states[aggregate_functions_destroy_index];

            /** We increase aggregate_functions_destroy_index because by function contract if insertResultIntoBatch
              * throws exception, it also must destroy all necessary states.
              * Then code need to continue to destroy other aggregate function states with next function index.
              */
            size_t destroy_index = aggregate_functions_destroy_index;
            ++aggregate_functions_destroy_index;

            /// For State AggregateFunction ownership of aggregate place is passed to result column after insert
            /// proton: starts.
            /// For non-streaming cases, after calling `insertResultIntoBatch`, aggregator / data should not maintain the places
            /// anymore (to avoid double free). We have 2 cases here:
            /// 1. For non `-State` aggregations, the ownership of places doesn't get transferred to result column after insert.
            ///    So we need a way to clean the places up: either the `insertResultIntoBatch` function destroys them or
            ///    the dtor of `data` destroys them. Here we ask `insertResultIntoBatch` to destroy them by setting .
            ///    `destroy_place_after_insert` to true. The behaviors of `insertResultIntoBatch` are:
            ///    If `DB::IAggregateFunctionHelper::insertResultIntoBatch` doesn't throw, it destroys to free dtor the places since
            ///    we set `destroy_place_after_insert` to true to tell it to destroy the places. Otherwise if it throws,
            ///    it also destroys the places no matter `destroy_place_after_insert` is set or not.
            ///    This is correct behavior for non-streaming non-State aggregations.
            /// 2. For `-State` aggregations, the ownership of places gets transferred to result column after insert. So aggregator / data doesn't
            ///    need to maintain them anymore. If `insertResultIntoBatch` throws, it cleans up the places which are not inserted yet
            ///
            /// For streaming cases (FIXME window recycling / projection):
            /// 1. For non `-State` aggregations, there are different cases
            ///    a. aggregator / data still need maintain the places if it is doing non-window based incremental aggregation
            ///       and periodical projection
            ///       For example: `SELECT avg(n) FROM STREAM(table) GROUP BY xxx`.
            ///    b. aggregator / data don't need maintain the places if it is doing window-based aggregation and when windows get sealed
            /// 2. For `-State` aggregations, since the ownership of the places are transferred to the result column, aggregator / data
            ///    don't need maintain them anymore after insert.
            bool is_state = aggregate_functions[destroy_index]->isState();
            bool destroy_place_after_insert = !is_state && clear_states;
            /// proton: ends

            aggregate_functions[destroy_index]->insertResultIntoBatch(0, places.size(), places.data(), offset, *final_aggregate_column, arena, destroy_place_after_insert);
        }
    }
    catch (...)
    {
        exception = std::current_exception();
    }

    for (; aggregate_functions_destroy_index < params.aggregates_size; ++aggregate_functions_destroy_index)
    {
        if constexpr (use_compiled_functions)
        {
            if (is_aggregate_function_compiled[aggregate_functions_destroy_index])
            {
                ++aggregate_functions_destroy_index;
                continue;
            }
        }

        bool is_state = aggregate_functions[aggregate_functions_destroy_index]->isState();
        bool destroy_place_after_insert = !is_state && clear_states;
        if (destroy_place_after_insert)
        {
            /// When aggregate_functions[aggregate_functions_destroy_index-1] throws Exception, there are two cases:
            /// Case 1: destroy_place_after_insert == true
            /// In this case, if a exception throws, delete the state of aggregate function with exception and aggregate function executed
            /// before this function in 'insertResultIntoBatch' and delete the states of the rest aggregate functions in 'convertToBlockImplFinal'.
            /// Because in this case, place' has been set to 'nullptr' in 'convertToBlockImplFinal' already, the 'destroyImpl' method will not
            /// destroy the states twice.
            ///
            /// Case 2: destroy_place_after_insert == false
            /// in this case, do not delete the state of any aggregate function neither in 'insertResultIntoBatch' nor in 'convertToBlockImplFinal'
            /// even if a exception throws.
            /// Because in this case (place != nullptr) the 'destroyImpl' method will destroy all the states of all aggregate functions together
            /// and it will not be destroyed the states twice neither.

            /// To avoid delete the state twice, only when destroy_place_after_insert == true, it should destroy
            /// the states of the rest aggregate functions (not executed after the aggregate function with exception below.
            size_t offset = offsets_of_aggregate_states[aggregate_functions_destroy_index];
            aggregate_functions[aggregate_functions_destroy_index]->destroyBatch(0, places.size(), places.data(), offset);
        }
    }

    if (exception)
        std::rethrow_exception(exception);
}

template <typename Method, typename Table>
void NO_INLINE Aggregator::convertToBlockImplNotFinal(
    Method & method,
    Table & data,
    std::vector<IColumn *>  key_columns,
    AggregateColumnsData & aggregate_columns) const
{
    if constexpr (Method::low_cardinality_optimization)
    {
        if (data.hasNullKeyData())
        {
            key_columns[0]->insertDefault();

            for (size_t i = 0; i < params.aggregates_size; ++i)
                aggregate_columns[i]->push_back(data.getNullKeyData() + offsets_of_aggregate_states[i]);

            data.getNullKeyData() = nullptr;
        }
    }

    auto shuffled_key_sizes = method.shuffleKeyColumns(key_columns, key_sizes);
    const auto & key_sizes_ref = shuffled_key_sizes ? *shuffled_key_sizes :  key_sizes;

    data.forEachValue([&](const auto & key, auto & mapped)
    {
        method.insertKeyIntoColumns(key, key_columns, key_sizes_ref);

        /// reserved, so push_back does not throw exceptions
        for (size_t i = 0; i < params.aggregates_size; ++i)
            aggregate_columns[i]->push_back(mapped + offsets_of_aggregate_states[i]);

        /// proton: starts. For streaming aggr, we hold on to the states
        /// Since it is not final, we shall never clear the state
        /// if (!params.keep_state)
        ///    mapped = nullptr;
        /// proton: ends.
    });
}


template <typename Filler>
Block Aggregator::prepareBlockAndFill(
    AggregatedDataVariants & data_variants,
    bool final,
    ConvertAction action,
    size_t rows,
    Filler && filler) const
{
    MutableColumns key_columns(params.keys_size);
    MutableColumns aggregate_columns(params.aggregates_size);
    MutableColumns final_aggregate_columns(params.aggregates_size);
    AggregateColumnsData aggregate_columns_data(params.aggregates_size);

    Block header = getHeader(final);

    for (size_t i = 0; i < params.keys_size; ++i)
    {
        key_columns[i] = header.safeGetByPosition(i).type->createColumn();
        key_columns[i]->reserve(rows);
    }

    for (size_t i = 0; i < params.aggregates_size; ++i)
    {
        if (!final)
        {
            const auto & aggregate_column_name = params.aggregates[i].column_name;
            aggregate_columns[i] = header.getByName(aggregate_column_name).type->createColumn();

            /// The ColumnAggregateFunction column captures the shared ownership of the arena with the aggregate function states.
            ColumnAggregateFunction & column_aggregate_func = assert_cast<ColumnAggregateFunction &>(*aggregate_columns[i]);

            /// proton: starts
            column_aggregate_func.setStreaming(params.keep_state);
            /// proton: ends

            /// Add arenas to ColumnAggregateFunction, which can result in moving ownership to it if reference count
            /// get dropped in other places
            for (auto & pool : data_variants.aggregates_pools)
                column_aggregate_func.addArena(pool);

            aggregate_columns_data[i] = &column_aggregate_func.getData();
            aggregate_columns_data[i]->reserve(rows);
        }
        else
        {
            final_aggregate_columns[i] = aggregate_functions[i]->getReturnType()->createColumn();
            final_aggregate_columns[i]->reserve(rows);

            if (aggregate_functions[i]->isState())
            {
                /// The ColumnAggregateFunction column captures the shared ownership of the arena with aggregate function states.
                if (auto * column_aggregate_func = typeid_cast<ColumnAggregateFunction *>(final_aggregate_columns[i].get()))
                    for (auto & pool : data_variants.aggregates_pools)
                        column_aggregate_func->addArena(pool);

                /// Aggregate state can be wrapped into array if aggregate function ends with -Resample combinator.
                final_aggregate_columns[i]->forEachSubcolumn([&data_variants](IColumn::WrappedPtr & subcolumn)
                {
                    if (auto * column_aggregate_func = typeid_cast<ColumnAggregateFunction *>(subcolumn.get()))
                        for (auto & pool : data_variants.aggregates_pools)
                            column_aggregate_func->addArena(pool);
                });
            }
        }
    }

    filler(key_columns, aggregate_columns_data, final_aggregate_columns, final, action);

    Block res = header.cloneEmpty();

    for (size_t i = 0; i < params.keys_size; ++i)
        res.getByPosition(i).column = std::move(key_columns[i]);

    for (size_t i = 0; i < params.aggregates_size; ++i)
    {
        const auto & aggregate_column_name = params.aggregates[i].column_name;
        if (final)
            res.getByName(aggregate_column_name).column = std::move(final_aggregate_columns[i]);
        else
            res.getByName(aggregate_column_name).column = std::move(aggregate_columns[i]);
    }

    /// Change the size of the columns-constants in the block.
    size_t columns = header.columns();
    for (size_t i = 0; i < columns; ++i)
        if (isColumnConst(*res.getByPosition(i).column))
            res.getByPosition(i).column = res.getByPosition(i).column->cut(0, rows);

    /// proton: starts
    if (shouldClearStates(action, final))
        data_variants.init(method_chosen);
    /// proton: ends

    return res;
}

void Aggregator::addSingleKeyToAggregateColumns(
    const AggregatedDataVariants & data_variants,
    MutableColumns & aggregate_columns) const
{
    const auto & data = data_variants.without_key;
    for (size_t i = 0; i < params.aggregates_size; ++i)
    {
        auto & column_aggregate_func = assert_cast<ColumnAggregateFunction &>(*aggregate_columns[i]);
        column_aggregate_func.getData().push_back(data + offsets_of_aggregate_states[i]);
    }
}

void Aggregator::addArenasToAggregateColumns(
    const AggregatedDataVariants & data_variants,
    MutableColumns & aggregate_columns) const
{
    for (size_t i = 0; i < params.aggregates_size; ++i)
    {
        auto & column_aggregate_func = assert_cast<ColumnAggregateFunction &>(*aggregate_columns[i]);
        for (const auto & pool : data_variants.aggregates_pools)
            column_aggregate_func.addArena(pool);
    }
}

void Aggregator::createStatesAndFillKeyColumnsWithSingleKey(
    AggregatedDataVariants & data_variants,
    Columns & key_columns,
    size_t key_row,
    MutableColumns & final_key_columns) const
{
    AggregateDataPtr place = data_variants.aggregates_pool->alignedAlloc(total_size_of_aggregate_states, align_aggregate_states);
    createAggregateStates(place);
    data_variants.without_key = place;

    for (size_t i = 0; i < params.keys_size; ++i)
    {
        final_key_columns[i]->insertFrom(*key_columns[i].get(), key_row);
    }
}

Block Aggregator::prepareBlockAndFillWithoutKey(AggregatedDataVariants & data_variants, bool final, bool is_overflows, ConvertAction action) const
{
    size_t rows = 1;

    auto filler = [&data_variants, this](
        MutableColumns & key_columns,
        AggregateColumnsData & aggregate_columns,
        MutableColumns & final_aggregate_columns,
        bool final_,
        ConvertAction action_)
    {
        if (data_variants.type == AggregatedDataVariants::Type::without_key || params.overflow_row)
        {
            AggregatedDataWithoutKey & data = data_variants.without_key;

            if (!data)
                throw Exception("Wrong data variant passed.", ErrorCodes::LOGICAL_ERROR);

            if (!final_)
            {
                for (size_t i = 0; i < params.aggregates_size; ++i)
                    aggregate_columns[i]->push_back(data + offsets_of_aggregate_states[i]);

                /// proton: starts
                if (shouldClearStates(action_, final_))
                    data = nullptr;
                /// proton: ends
            }
            else
            {
                /// Always single-thread. It's safe to pass current arena from 'aggregates_pool'.
                insertAggregatesIntoColumns(data, final_aggregate_columns, data_variants.aggregates_pool);
            }

            if (params.overflow_row)
                for (size_t i = 0; i < params.keys_size; ++i)
                    key_columns[i]->insertDefault();
        }
    };

    Block block = prepareBlockAndFill(data_variants, final, action, rows, filler);

    if (is_overflows)
        block.info.is_overflows = true;

    /// proton: starts
    if (shouldClearStates(action, final))
    {
        /// for nested global aggregation, the outer aggregation does not need to keep state. Therefore after
        /// emit aggregating result, reinitialize state 'without_key' for next round aggregation.
        /// Example:
        /// SELECT
        //    count_distinct(lpn)
        //  FROM
        //  (
        //    SELECT
        //      lpn, max(_tp_time) AS lastSeen
        //    FROM
        //      ttp_0
        //    GROUP BY
        //      lpn
        //    HAVING
        //      lastSeen > date_sub(now(), 10s)
        //    EMIT PERIODIC 5s
        //    SETTINGS
        //      seek_to = '-24h'
        //  )
        destroyWithoutKey(data_variants);
        initStatesForWithoutKeyOrOverflow(data_variants);
    }
    /// proton: ends

    return block;
}

Block Aggregator::prepareBlockAndFillSingleLevel(AggregatedDataVariants & data_variants, bool final, ConvertAction action) const
{
    size_t rows = data_variants.sizeWithoutOverflowRow();

    auto filler = [&data_variants, this](
        MutableColumns & key_columns,
        AggregateColumnsData & aggregate_columns,
        MutableColumns & final_aggregate_columns,
        bool final_,
        ConvertAction action_)
    {
    #define M(NAME) \
        else if (data_variants.type == AggregatedDataVariants::Type::NAME) \
            convertToBlockImpl(*data_variants.NAME, data_variants.NAME->data, \
                key_columns, aggregate_columns, final_aggregate_columns, data_variants.aggregates_pool, final_, action_);

        if (false) {} // NOLINT
        APPLY_FOR_VARIANTS_SINGLE_LEVEL_STREAMING(M)
    #undef M
        else
            throw Exception("Unknown aggregated data variant.", ErrorCodes::UNKNOWN_AGGREGATED_DATA_VARIANT);
    };

    return prepareBlockAndFill(data_variants, final, action, rows, filler);
}


BlocksList Aggregator::prepareBlocksAndFillTwoLevel(AggregatedDataVariants & data_variants, bool final, size_t max_threads, ConvertAction action) const
{
    std::unique_ptr<ThreadPool> thread_pool;
    if (max_threads > 1 && data_variants.sizeWithoutOverflowRow() > 100000 /// TODO Make a custom threshold.
        && data_variants.isStaticBucketTwoLevel()) /// TODO Use the shared thread pool with the `merge` function.
        thread_pool = std::make_unique<ThreadPool>(max_threads);

#define M(NAME) \
    else if (data_variants.type == AggregatedDataVariants::Type::NAME) \
        return prepareBlocksAndFillTwoLevelImpl(data_variants, *data_variants.NAME, final, action, thread_pool.get());

    if (false) {} // NOLINT
    APPLY_FOR_VARIANTS_ALL_TWO_LEVEL(M)
#undef M
    else
        throw Exception("Unknown aggregated data variant.", ErrorCodes::UNKNOWN_AGGREGATED_DATA_VARIANT);
}


template <typename Method>
BlocksList Aggregator::prepareBlocksAndFillTwoLevelImpl(
    AggregatedDataVariants & data_variants,
    Method & method,
    bool final,
    ConvertAction action,
    ThreadPool * thread_pool) const
{
    size_t max_threads = thread_pool ? thread_pool->getMaxThreads() : 1;
    /// proton FIXME : separate final vs non-final converting. For non-final converting, we don't need
    /// each arena for each thread.
    for (size_t i = data_variants.aggregates_pools.size(); i < max_threads; ++i)
        data_variants.aggregates_pools.push_back(std::make_shared<Arena>());

    auto buckets = method.data.buckets();
    std::atomic<UInt32> next_bucket_idx_to_merge = 0;

    auto converter = [&](size_t thread_id, ThreadGroupStatusPtr thread_group)
    {
        SCOPE_EXIT_SAFE(
            if (thread_group)
                CurrentThread::detachQueryIfNotDetached();
        );
        if (thread_group)
            CurrentThread::attachToIfDetached(thread_group);

        BlocksList blocks;
        while (true)
        {
            UInt32 bucket_idx = next_bucket_idx_to_merge.fetch_add(1);

            if (bucket_idx >= buckets.size())
                break;

            auto bucket = buckets[bucket_idx];
            if (method.data.impls[bucket].empty())
                continue;

            /// Select Arena to avoid race conditions
            Arena * arena = data_variants.aggregates_pools.at(thread_id).get();
            blocks.emplace_back(convertOneBucketToBlock(data_variants, method, arena, final, action, bucket));
        }
        return blocks;
    };

    /// packaged_task is used to ensure that exceptions are automatically thrown into the main stream.

    std::vector<std::packaged_task<BlocksList()>> tasks(max_threads);

    try
    {
        for (size_t thread_id = 0; thread_id < max_threads; ++thread_id)
        {
            tasks[thread_id] = std::packaged_task<BlocksList()>(
                [group = CurrentThread::getGroup(), thread_id, &converter] { return converter(thread_id, group); });

            if (thread_pool)
                thread_pool->scheduleOrThrowOnError([thread_id, &tasks] { tasks[thread_id](); });
            else
                tasks[thread_id]();
        }
    }
    catch (...)
    {
        /// If this is not done, then in case of an exception, tasks will be destroyed before the threads are completed, and it will be bad.
        if (thread_pool)
            thread_pool->wait();

        throw;
    }

    if (thread_pool)
        thread_pool->wait();

    BlocksList blocks;

    for (auto & task : tasks)
    {
        if (!task.valid())
            continue;

        blocks.splice(blocks.end(), task.get_future().get());
    }

    return blocks;
}


BlocksList Aggregator::convertToBlocks(AggregatedDataVariants & data_variants, bool final, ConvertAction action, size_t max_threads) const
{
    LOG_TRACE(log, "Converting aggregated data to blocks");

    Stopwatch watch;

    BlocksList blocks;

    /// In what data structure is the data aggregated?
    if (data_variants.empty())
        return blocks;

    if (data_variants.without_key)
        /// When without_key is setup, it doesn't necessary mean no GROUP BY keys, it may be overflow
        blocks.emplace_back(prepareBlockAndFillWithoutKey(
            data_variants, final, data_variants.type != AggregatedDataVariants::Type::without_key, action));

    if (data_variants.type != AggregatedDataVariants::Type::without_key)
    {
        if (data_variants.isTwoLevel())
            blocks.splice(blocks.end(), prepareBlocksAndFillTwoLevel(data_variants, final, max_threads, action));
        else
            blocks.emplace_back(prepareBlockAndFillSingleLevel(data_variants, final, action));
    }

    /// proton: starts.
    if (shouldClearStates(action, final))
        data_variants.aggregator = nullptr;
    /// proton: ends

    size_t rows = 0;
    size_t bytes = 0;

    for (const auto & block : blocks)
    {
        rows += block.rows();
        bytes += block.bytes();
    }

    double elapsed_seconds = watch.elapsedSeconds();
    LOG_INFO(log,
        "Converted aggregated data to blocks. {} rows, {} in {} sec. ({:.3f} rows/sec., {}/sec.)",
        rows, ReadableSize(bytes),
        elapsed_seconds, rows / elapsed_seconds,
        ReadableSize(bytes / elapsed_seconds));

    return blocks;
}


template <typename Method, typename Table>
void NO_INLINE Aggregator::mergeDataNullKey(
    Table & table_dst,
    Table & table_src,
    Arena * arena,
    bool clear_states) const
{
    if constexpr (Method::low_cardinality_optimization)
    {
        if (table_src.hasNullKeyData())
        {
            if (!table_dst.hasNullKeyData())
            {
                table_dst.hasNullKeyData() = true;
                table_dst.getNullKeyData() = table_src.getNullKeyData();
            }
            else
            {
                for (size_t i = 0; i < params.aggregates_size; ++i)
                    aggregate_functions[i]->merge(
                            table_dst.getNullKeyData() + offsets_of_aggregate_states[i],
                            table_src.getNullKeyData() + offsets_of_aggregate_states[i],
                            arena);

                /// proton : starts
                if (clear_states)
                    for (size_t i = 0; i < params.aggregates_size; ++i)
                        aggregate_functions[i]->destroy(
                                table_src.getNullKeyData() + offsets_of_aggregate_states[i]);
            }

            if (clear_states)
            {
                table_src.hasNullKeyData() = false;
                table_src.getNullKeyData() = nullptr;
            }
            /// proton : ends
        }
    }
}


template <typename Method, bool use_compiled_functions, typename Table, typename KeyHandler>
void NO_INLINE Aggregator::mergeDataImpl(
    Table & table_dst,
    Table & table_src,
    Arena * arena,
    bool clear_states,
    KeyHandler && key_handler) const
{
    if constexpr (Method::low_cardinality_optimization)
        mergeDataNullKey<Method, Table>(table_dst, table_src, arena, clear_states);

    auto func = [&](AggregateDataPtr & __restrict dst, AggregateDataPtr & __restrict src, bool inserted)
    {
        /// proton: starts
        if (inserted)
        {
            /// If there are multiple sources, there are more than one AggregatedDataVariant. Aggregator always creates a new AggregatedDataVariant and merge all other
            /// AggregatedDataVariants to the new created one. After finalize(), it does not clean up aggregate state except the new create AggregatedDataVariant.
            /// If it does not alloc new memory for the 'dst' (i.e. aggregate state of the new AggregatedDataVariant which get destoryed after finalize()) but reuse
            /// that from the 'src' to store the final aggregated result, it will cause the data from other AggregatedDataVariant will be merged multiple times and
            /// generate incorrect aggregated result.
            dst = arena->alignedAlloc(total_size_of_aggregate_states, align_aggregate_states);
#if USE_EMBEDDED_COMPILER
            if constexpr (use_compiled_functions)
            {
                const auto & compiled_aggregate_functions = compiled_aggregate_functions_holder->compiled_aggregate_functions;
                compiled_aggregate_functions.create_aggregate_states_function(dst);
                if (compiled_aggregate_functions.functions_count != aggregate_functions.size())
                {
                    static constexpr bool skip_compiled_aggregate_functions = true;
                    createAggregateStates<skip_compiled_aggregate_functions>(dst);
                }

#if defined(MEMORY_SANITIZER)

                /// We compile only functions that do not allocate some data in Arena. Only store necessary state in AggregateData place.
                for (size_t aggregate_function_index = 0; aggregate_function_index < aggregate_functions.size(); ++aggregate_function_index)
                {
                    if (!is_aggregate_function_compiled[aggregate_function_index])
                        continue;

                    auto aggregate_data_with_offset = dst + offsets_of_aggregate_states[aggregate_function_index];
                    auto data_size = params.aggregates[aggregate_function_index].function->sizeOfData();
                    __msan_unpoison(aggregate_data_with_offset, data_size);
                }
#endif
            }
            else
#endif
            {
                createAggregateStates(dst);
            }
        }
        /// proton: ends

#if USE_EMBEDDED_COMPILER
        if constexpr (use_compiled_functions)
        {
            const auto & compiled_functions = compiled_aggregate_functions_holder->compiled_aggregate_functions;
            compiled_functions.merge_aggregate_states_function(dst, src);

            if (compiled_aggregate_functions_holder->compiled_aggregate_functions.functions_count != params.aggregates_size)
            {
                for (size_t i = 0; i < params.aggregates_size; ++i)
                {
                    if (!is_aggregate_function_compiled[i])
                        aggregate_functions[i]->merge(dst + offsets_of_aggregate_states[i], src + offsets_of_aggregate_states[i], arena);
                }

//                    for (size_t i = 0; i < params.aggregates_size; ++i)
//                    {
//                        /// proton: starts
//                        if (!is_aggregate_function_compiled[i] && !params.streaming)
//                            aggregate_functions[i]->destroy(src + offsets_of_aggregate_states[i]);
//                        /// proton: ends
//                    }
            }
        }
        else
#endif
        {
            for (size_t i = 0; i < params.aggregates_size; ++i)
                aggregate_functions[i]->merge(dst + offsets_of_aggregate_states[i], src + offsets_of_aggregate_states[i], arena);

            /// proton: starts
            if (clear_states)
                for (size_t i = 0; i < params.aggregates_size; ++i)
                    aggregate_functions[i]->destroy(src + offsets_of_aggregate_states[i]);
        }

        if (clear_states)
            src = nullptr;
    };

    if constexpr (std::is_same_v<KeyHandler, EmptyKeyHandler>)
        table_src.mergeToViaEmplace(table_dst, func);
    else
        table_src.mergeToViaEmplace(table_dst, func, std::move(key_handler));

    if (clear_states)
        table_src.clearAndShrink();
    /// proton: ends
}


template <typename Method, typename Table>
void NO_INLINE Aggregator::mergeDataNoMoreKeysImpl(
    Table & table_dst,
    AggregatedDataWithoutKey & overflows,
    Table & table_src,
    Arena * arena,
    bool clear_states) const
{
    /// Note : will create data for NULL key if not exist
    if constexpr (Method::low_cardinality_optimization)
        mergeDataNullKey<Method, Table>(table_dst, table_src, arena, clear_states);

    table_src.mergeToViaFind(table_dst, [&](AggregateDataPtr dst, AggregateDataPtr & src, bool found)
    {
        AggregateDataPtr res_data = found ? dst : overflows;

        for (size_t i = 0; i < params.aggregates_size; ++i)
            aggregate_functions[i]->merge(
                res_data + offsets_of_aggregate_states[i],
                src + offsets_of_aggregate_states[i],
                arena);

        /// proton : starts
        if (clear_states)
        {
            for (size_t i = 0; i < params.aggregates_size; ++i)
                aggregate_functions[i]->destroy(src + offsets_of_aggregate_states[i]);

            src = nullptr;
        }
        /// proton : ends
    });

    if (clear_states)
        table_src.clearAndShrink();
}

template <typename Method, typename Table>
void NO_INLINE Aggregator::mergeDataOnlyExistingKeysImpl(
    Table & table_dst,
    Table & table_src,
    Arena * arena,
    bool clear_states) const
{
    /// Note : will create data for NULL key if not exist
    if constexpr (Method::low_cardinality_optimization)
        mergeDataNullKey<Method, Table>(table_dst, table_src, arena, clear_states);

    table_src.mergeToViaFind(table_dst,
        [&](AggregateDataPtr dst, AggregateDataPtr & src, bool found)
    {
        if (!found)
            return;

        for (size_t i = 0; i < params.aggregates_size; ++i)
            aggregate_functions[i]->merge(
                dst + offsets_of_aggregate_states[i],
                src + offsets_of_aggregate_states[i],
                arena);

        /// proton : starts
        if (clear_states)
        {
            for (size_t i = 0; i < params.aggregates_size; ++i)
                aggregate_functions[i]->destroy(src + offsets_of_aggregate_states[i]);

            src = nullptr;
        }
    });

    if (clear_states)
        table_src.clearAndShrink();
    /// proton : ends
}


void NO_INLINE Aggregator::mergeWithoutKeyDataImpl(
    ManyAggregatedDataVariants & non_empty_data) const
{
    AggregatedDataVariantsPtr & res = non_empty_data[0];

    /// We merge all aggregation results to the first.
    for (size_t result_num = 1, size = non_empty_data.size(); result_num < size; ++result_num)
    {
        AggregatedDataWithoutKey & res_data = res->without_key;
        AggregatedDataWithoutKey & current_data = non_empty_data[result_num]->without_key;

        for (size_t i = 0; i < params.aggregates_size; ++i)
            aggregate_functions[i]->merge(res_data + offsets_of_aggregate_states[i], current_data + offsets_of_aggregate_states[i], res->aggregates_pool);

        if (!params.keep_state)
        {
            for (size_t i = 0; i < params.aggregates_size; ++i)
                aggregate_functions[i]->destroy(current_data + offsets_of_aggregate_states[i]);

            current_data = nullptr;
        }
    }
}


template <typename Method>
void NO_INLINE Aggregator::mergeSingleLevelDataImpl(
    ManyAggregatedDataVariants & non_empty_data, ConvertAction action) const
{
    AggregatedDataVariantsPtr & res = non_empty_data[0];
    bool no_more_keys = false;

    auto clear_states = shouldClearStates(action, true);

    /// We merge all aggregation results to the first.
    for (size_t result_num = 1, size = non_empty_data.size(); result_num < size; ++result_num)
    {
        if (!checkLimits(res->sizeWithoutOverflowRow(), no_more_keys))
            break;

        AggregatedDataVariants & current = *non_empty_data[result_num];

        if (!no_more_keys)
        {
#if USE_EMBEDDED_COMPILER
            if (compiled_aggregate_functions_holder)
            {
                mergeDataImpl<Method, true>(
                    getDataVariant<Method>(*res).data,
                    getDataVariant<Method>(current).data,
                    res->aggregates_pool,
                    clear_states);
            }
            else
#endif
            {
                mergeDataImpl<Method, false>(
                    getDataVariant<Method>(*res).data,
                    getDataVariant<Method>(current).data,
                    res->aggregates_pool,
                    clear_states);
            }
        }
        else if (res->without_key)
        {
            mergeDataNoMoreKeysImpl<Method>(
                getDataVariant<Method>(*res).data,
                res->without_key,
                getDataVariant<Method>(current).data,
                res->aggregates_pool,
                clear_states);
        }
        else
        {
            mergeDataOnlyExistingKeysImpl<Method>(
                getDataVariant<Method>(*res).data,
                getDataVariant<Method>(current).data,
                res->aggregates_pool,
                clear_states);
        }

        /// `current` will not destroy the states of aggregate functions in the destructor
        if (clear_states)
            current.aggregator = nullptr;
    }
}

#define M(NAME) \
    template void NO_INLINE Aggregator::mergeSingleLevelDataImpl<decltype(AggregatedDataVariants::NAME)::element_type>( \
        ManyAggregatedDataVariants & non_empty_data, ConvertAction action) const;
    APPLY_FOR_VARIANTS_SINGLE_LEVEL_STREAMING(M)
#undef M

template <typename Method>
void NO_INLINE Aggregator::mergeBucketImpl(
    ManyAggregatedDataVariants & data, bool final, ConvertAction action, size_t bucket, Arena * arena, std::atomic<bool> * is_cancelled) const
{
    auto clear_states = shouldClearStates(action, final);

    /// We merge all aggregation results to the first.
    AggregatedDataVariantsPtr & res = data[0];
    for (size_t result_num = 1, size = data.size(); result_num < size; ++result_num)
    {
        if (is_cancelled && is_cancelled->load(std::memory_order_seq_cst))
            return;

        AggregatedDataVariants & current = *data[result_num];
#if USE_EMBEDDED_COMPILER
        if (compiled_aggregate_functions_holder)
        {
            mergeDataImpl<Method, true>(
                getDataVariant<Method>(*res).data.impls[bucket],
                getDataVariant<Method>(current).data.impls[bucket],
                arena,
                clear_states);
        }
        else
#endif
        {
            mergeDataImpl<Method, false>(
                getDataVariant<Method>(*res).data.impls[bucket],
                getDataVariant<Method>(current).data.impls[bucket],
                arena,
                clear_states);
        }
    }
}

ManyAggregatedDataVariantsPtr Aggregator::prepareVariantsToMerge(ManyAggregatedDataVariants & data_variants) const
{
    if (data_variants.empty())
        throw Exception("Empty data passed to Aggregator::mergeAndConvertToBlocks.", ErrorCodes::EMPTY_DATA_PASSED);

    LOG_TRACE(log, "Merging aggregated data");

    auto non_empty_data = std::make_shared<ManyAggregatedDataVariants>();

    /// proton: starts:
    for (auto & data : data_variants)
        if (!data->empty())
            non_empty_data->push_back(data);

    if (non_empty_data->empty())
        return non_empty_data;

    if (non_empty_data->size() > 1)
    {
        /// When do streaming merging, we shall not touch existing memory arenas and
        /// all memory arenas merge to the first empty one, so we need create a new resulting arena
        /// at position 0.
        auto result_variants = std::make_shared<AggregatedDataVariants>(false);
        result_variants->aggregator = this;
        result_variants->keys_size = params.keys_size;
        result_variants->key_sizes = key_sizes;
        result_variants->init(method_chosen);
        initStatesForWithoutKeyOrOverflow(*result_variants);
        non_empty_data->insert(non_empty_data->begin(), result_variants);
    }

    /// for streaming query, we don't need sort the arenas
//    if (non_empty_data.size() > 1)
//    {
//        /// Sort the states in descending order so that the merge is more efficient (since all states are merged into the first).
//        std::sort(non_empty_data.begin(), non_empty_data.end(),
//            [](const AggregatedDataVariantsPtr & lhs, const AggregatedDataVariantsPtr & rhs)
//            {
//                return lhs->sizeWithoutOverflowRow() > rhs->sizeWithoutOverflowRow();
//            });
//    }
    /// proton: ends

    /// If at least one of the options is two-level, then convert all the options into two-level ones, if there are not such.
    /// Note - perhaps it would be more optimal not to convert single-level versions before the merge, but merge them separately, at the end.

    /// proton: starts. The first variant is for result aggregating
    bool has_two_level
        = std::any_of(non_empty_data->begin(), non_empty_data->end(), [](const auto & variant) { return variant->isTwoLevel(); });

    if (has_two_level)
    {
        for (auto & variant : *non_empty_data)
            if (!variant->isTwoLevel())
                variant->convertToTwoLevel();
    }

    AggregatedDataVariantsPtr & first = non_empty_data->at(0);

    for (size_t i = 1, size = non_empty_data->size(); i < size; ++i)
    {
        if (first->type != non_empty_data->at(i)->type)
            throw Exception("Cannot merge different aggregated data variants.", ErrorCodes::CANNOT_MERGE_DIFFERENT_AGGREGATED_DATA_VARIANTS);

        /** Elements from the remaining sets can be moved to the first data set.
          * Therefore, it must own all the arenas of all other sets.
          */
        first->aggregates_pools.insert(first->aggregates_pools.end(),
            non_empty_data->at(i)->aggregates_pools.begin(), non_empty_data->at(i)->aggregates_pools.end());
    }

    assert(first->aggregates_pools.size() == non_empty_data->size());

    return non_empty_data;
}

template <bool no_more_keys, typename Method, typename Table>
void NO_INLINE Aggregator::mergeStreamsImplCase(
    Block & block,
    Arena * aggregates_pool,
    Method & method [[maybe_unused]],
    Table & data,
    AggregateDataPtr overflow_row) const
{
    ColumnRawPtrs key_columns(params.keys_size);
    AggregateColumnsConstData aggregate_columns(params.aggregates_size);

    /// Remember the columns we will work with
    for (size_t i = 0; i < params.keys_size; ++i)
        key_columns[i] = block.safeGetByPosition(i).column.get();

    for (size_t i = 0; i < params.aggregates_size; ++i)
    {
        const auto & aggregate_column_name = params.aggregates[i].column_name;
        aggregate_columns[i] = &typeid_cast<const ColumnAggregateFunction &>(*block.getByName(aggregate_column_name).column).getData();
    }

    typename Method::State state(key_columns, key_sizes, aggregation_state_cache);

    /// For all rows.
    size_t rows = block.rows();
    std::unique_ptr<AggregateDataPtr[]> places(new AggregateDataPtr[rows]);

    for (size_t i = 0; i < rows; ++i)
    {
        AggregateDataPtr aggregate_data = nullptr;

        if (!no_more_keys)
        {
            auto emplace_result = state.emplaceKey(data, i, *aggregates_pool);
            if (emplace_result.isInserted())
            {
                emplace_result.setMapped(nullptr);

                aggregate_data = aggregates_pool->alignedAlloc(total_size_of_aggregate_states, align_aggregate_states);
                createAggregateStates(aggregate_data);

                emplace_result.setMapped(aggregate_data);
            }
            else
                aggregate_data = emplace_result.getMapped();
        }
        else
        {
            auto find_result = state.findKey(data, i, *aggregates_pool);
            if (find_result.isFound())
                aggregate_data = find_result.getMapped();
        }

        /// aggregate_date == nullptr means that the new key did not fit in the hash table because of no_more_keys.

        AggregateDataPtr value = aggregate_data ? aggregate_data : overflow_row;
        places[i] = value;
    }

    for (size_t j = 0; j < params.aggregates_size; ++j)
    {
        /// Merge state of aggregate functions.
        aggregate_functions[j]->mergeBatch(
            0, rows,
            places.get(), offsets_of_aggregate_states[j],
            aggregate_columns[j]->data(),
            aggregates_pool);
    }

    /// Early release memory.
    block.clear();
}

template <typename Method, typename Table>
void NO_INLINE Aggregator::mergeStreamsImpl(
    Block & block,
    Arena * aggregates_pool,
    Method & method,
    Table & data,
    AggregateDataPtr overflow_row,
    bool no_more_keys) const
{
    if (!no_more_keys)
        mergeStreamsImplCase<false>(block, aggregates_pool, method, data, overflow_row);
    else
        mergeStreamsImplCase<true>(block, aggregates_pool, method, data, overflow_row);
}


void NO_INLINE Aggregator::mergeWithoutKeyStreamsImpl(
    Block & block,
    AggregatedDataVariants & result) const
{
    AggregateColumnsConstData aggregate_columns(params.aggregates_size);

    /// Remember the columns we will work with
    for (size_t i = 0; i < params.aggregates_size; ++i)
    {
        const auto & aggregate_column_name = params.aggregates[i].column_name;
        aggregate_columns[i] = &typeid_cast<const ColumnAggregateFunction &>(*block.getByName(aggregate_column_name).column).getData();
    }

    AggregatedDataWithoutKey & res = result.without_key;
    if (!res)
    {
        AggregateDataPtr place = result.aggregates_pool->alignedAlloc(total_size_of_aggregate_states, align_aggregate_states);
        createAggregateStates(place);
        res = place;
    }

    for (size_t row = 0, rows = block.rows(); row < rows; ++row)
    {
        /// Adding Values
        for (size_t i = 0; i < params.aggregates_size; ++i)
            aggregate_functions[i]->merge(res + offsets_of_aggregate_states[i], (*aggregate_columns[i])[row], result.aggregates_pool);
    }

    /// Early release memory.
    block.clear();
}

bool Aggregator::mergeOnBlock(Block block, AggregatedDataVariants & result, bool & no_more_keys) const
{
    /// `result` will destroy the states of aggregate functions in the destructor
    result.aggregator = this;

    /// How to perform the aggregation?
    if (result.empty())
    {
        result.init(method_chosen);
        result.keys_size = params.keys_size;
        result.key_sizes = key_sizes;
        LOG_TRACE(log, "Aggregation method: {}", result.getMethodName());
    }

    if (result.type == AggregatedDataVariants::Type::without_key || block.info.is_overflows)
        mergeWithoutKeyStreamsImpl(block, result);

#define M(NAME, IS_TWO_LEVEL) \
    else if (result.type == AggregatedDataVariants::Type::NAME) \
        mergeStreamsImpl(block, result.aggregates_pool, *result.NAME, result.NAME->data, result.without_key, no_more_keys);

    APPLY_FOR_AGGREGATED_VARIANTS_STREAMING(M)
#undef M
    else if (result.type != AggregatedDataVariants::Type::without_key)
        throw Exception("Unknown aggregated data variant.", ErrorCodes::UNKNOWN_AGGREGATED_DATA_VARIANT);

    size_t result_size = result.sizeWithoutOverflowRow();
    Int64 current_memory_usage = 0;
    if (auto * memory_tracker_child = CurrentThread::getMemoryTracker())
        if (auto * memory_tracker = memory_tracker_child->getParent())
            current_memory_usage = memory_tracker->get();

    /// Here all the results in the sum are taken into account, from different threads.
    auto result_size_bytes = current_memory_usage - memory_usage_before_aggregation;

    bool worth_convert_to_two_level
        = (params.group_by_two_level_threshold && result_size >= params.group_by_two_level_threshold)
        || (params.group_by_two_level_threshold_bytes && result_size_bytes >= static_cast<Int64>(params.group_by_two_level_threshold_bytes));

    /** Converting to a two-level data structure.
      * It allows you to make, in the subsequent, an effective merge - either economical from memory or parallel.
      */
    if (result.isConvertibleToTwoLevel() && worth_convert_to_two_level)
        result.convertToTwoLevel();

    /// Checking the constraints.
    if (!checkLimits(result_size, no_more_keys))
        return false;

    /** Flush data to disk if too much RAM is consumed.
      * Data can only be flushed to disk if a two-level aggregation structure is used.
      */
    if (params.max_bytes_before_external_group_by
        && result.isTwoLevel()
        && current_memory_usage > static_cast<Int64>(params.max_bytes_before_external_group_by)
        && worth_convert_to_two_level)
    {
        size_t size = current_memory_usage + params.min_free_disk_space;

        std::string tmp_path = params.tmp_volume->getDisk()->getPath();

        // enoughSpaceInDirectory() is not enough to make it right, since
        // another process (or another thread of aggregator) can consume all
        // space.
        //
        // But true reservation (IVolume::reserve()) cannot be used here since
        // current_memory_usage does not take compression into account and
        // will reserve way more that actually will be used.
        //
        // Hence, let's do a simple check.
        if (!enoughSpaceInDirectory(tmp_path, size))
            throw Exception("Not enough space for external aggregation in " + tmp_path, ErrorCodes::NOT_ENOUGH_SPACE);

        writeToTemporaryFile(result, tmp_path);
    }

    return true;
}


void Aggregator::mergeBlocks(BucketToBlocks bucket_to_blocks, AggregatedDataVariants & result, size_t max_threads)
{
    if (bucket_to_blocks.empty())
        return;

    UInt64 total_input_rows = 0;
    for (auto & bucket : bucket_to_blocks)
        for (auto & block : bucket.second)
            total_input_rows += block.rows();

    /** `minus one` means the absence of information about the bucket
      * - in the case of single-level aggregation, as well as for blocks with "overflowing" values.
      * If there is at least one block with a bucket number greater or equal than zero, then there was a two-level aggregation.
      */
    auto max_bucket = bucket_to_blocks.rbegin()->first;
    bool has_two_level = max_bucket >= 0;

    if (has_two_level)
    {
    #define M(NAME) \
        if (method_chosen == AggregatedDataVariants::Type::NAME) \
            method_chosen = AggregatedDataVariants::Type::NAME ## _two_level;

        APPLY_FOR_VARIANTS_CONVERTIBLE_TO_STATIC_BUCKET_TWO_LEVEL(M)

    #undef M
    }

    /// result will destroy the states of aggregate functions in the destructor
    result.aggregator = this;

    result.init(method_chosen);
    result.keys_size = params.keys_size;
    result.key_sizes = key_sizes;

    bool has_blocks_with_unknown_bucket = bucket_to_blocks.contains(-1);

    /// First, parallel the merge for the individual buckets. Then we continue merge the data not allocated to the buckets.
    if (has_two_level)
    {
        /** In this case, no_more_keys is not supported due to the fact that
          *  from different threads it is difficult to update the general state for "other" keys (overflows).
          * That is, the keys in the end can be significantly larger than max_rows_to_group_by.
          */

        LOG_TRACE(log, "Merging partially aggregated two-level data.");

        auto merge_bucket = [&bucket_to_blocks, &result, this](size_t bucket, Arena * aggregates_pool, ThreadGroupStatusPtr thread_group)
        {
            SCOPE_EXIT_SAFE(
                if (thread_group)
                    CurrentThread::detachQueryIfNotDetached();
            );
            if (thread_group)
                CurrentThread::attachToIfDetached(thread_group);

            for (Block & block : bucket_to_blocks[static_cast<int>(bucket)])
            {
            #define M(NAME) \
                else if (result.type == AggregatedDataVariants::Type::NAME) \
                    mergeStreamsImpl(block, aggregates_pool, *result.NAME, result.NAME->data.impls[bucket], nullptr, false);

                if (false) {} // NOLINT
                    APPLY_FOR_VARIANTS_ALL_TWO_LEVEL(M)
            #undef M
                else
                    throw Exception("Unknown aggregated data variant.", ErrorCodes::UNKNOWN_AGGREGATED_DATA_VARIANT);
            }
        };

        std::unique_ptr<ThreadPool> thread_pool;
        if (max_threads > 1 && total_input_rows > 100000)    /// TODO Make a custom threshold.
            thread_pool = std::make_unique<ThreadPool>(max_threads);

        for (const auto & bucket_blocks : bucket_to_blocks)
        {
            const auto bucket = bucket_blocks.first;

            if (bucket == -1)
                continue;

            result.aggregates_pools.push_back(std::make_shared<Arena>());
            Arena * aggregates_pool = result.aggregates_pools.back().get();

            auto task = [group = CurrentThread::getGroup(), bucket, &merge_bucket, aggregates_pool]{ return merge_bucket(bucket, aggregates_pool, group); };

            if (thread_pool)
                thread_pool->scheduleOrThrowOnError(task);
            else
                task();
        }

        if (thread_pool)
            thread_pool->wait();

        LOG_TRACE(log, "Merged partially aggregated two-level data.");
    }

    if (has_blocks_with_unknown_bucket)
    {
        LOG_TRACE(log, "Merging partially aggregated single-level data.");

        bool no_more_keys = false;

        BlocksList & blocks = bucket_to_blocks[-1];
        for (Block & block : blocks)
        {
            if (!checkLimits(result.sizeWithoutOverflowRow(), no_more_keys))
                break;

            if (result.type == AggregatedDataVariants::Type::without_key || block.info.is_overflows)
                mergeWithoutKeyStreamsImpl(block, result);

        #define M(NAME, IS_TWO_LEVEL) \
            else if (result.type == AggregatedDataVariants::Type::NAME) \
                mergeStreamsImpl(block, result.aggregates_pool, *result.NAME, result.NAME->data, result.without_key, no_more_keys);

            APPLY_FOR_AGGREGATED_VARIANTS_STREAMING(M)
        #undef M
            else if (result.type != AggregatedDataVariants::Type::without_key)
                throw Exception("Unknown aggregated data variant.", ErrorCodes::UNKNOWN_AGGREGATED_DATA_VARIANT);
        }

        LOG_TRACE(log, "Merged partially aggregated single-level data.");
    }
}


Block Aggregator::mergeBlocks(BlocksList & blocks, bool final, ConvertAction action)
{
    if (blocks.empty())
        return {};

    auto bucket_num = blocks.front().info.bucket_num;
    bool is_overflows = blocks.front().info.is_overflows;

    LOG_TRACE(log, "Merging partially aggregated blocks (bucket = {}).", bucket_num);
    Stopwatch watch;

    /** If possible, change 'method' to some_hash64. Otherwise, leave as is.
      * Better hash function is needed because during external aggregation,
      *  we may merge partitions of data with total number of keys far greater than 4 billion.
      */
    auto merge_method = method_chosen;

#define APPLY_FOR_VARIANTS_THAT_MAY_USE_BETTER_HASH_FUNCTION(M) \
        M(key64)            \
        M(key_string)       \
        M(key_fixed_string) \
        M(keys128)          \
        M(keys256)          \
        M(serialized)       \

#define M(NAME) \
    if (merge_method == AggregatedDataVariants::Type::NAME) \
        merge_method = AggregatedDataVariants::Type::NAME ## _hash64; \

    APPLY_FOR_VARIANTS_THAT_MAY_USE_BETTER_HASH_FUNCTION(M)
#undef M

#undef APPLY_FOR_VARIANTS_THAT_MAY_USE_BETTER_HASH_FUNCTION

    /// Temporary data for aggregation.
    AggregatedDataVariants result;

    /// result will destroy the states of aggregate functions in the destructor
    result.aggregator = this;

    /// proton: starts
    result.keys_size = params.keys_size;
    result.key_sizes = key_sizes;
    result.init(merge_method);
    /// proton: ends

    for (Block & block : blocks)
    {
        if (bucket_num >= 0 && block.info.bucket_num != bucket_num)
            bucket_num = -1;

        if (result.type == AggregatedDataVariants::Type::without_key || is_overflows)
            mergeWithoutKeyStreamsImpl(block, result);

    #define M(NAME, IS_TWO_LEVEL) \
        else if (result.type == AggregatedDataVariants::Type::NAME) \
            mergeStreamsImpl(block, result.aggregates_pool, *result.NAME, result.NAME->data, nullptr, false);

        APPLY_FOR_AGGREGATED_VARIANTS_STREAMING(M)
    #undef M
        else if (result.type != AggregatedDataVariants::Type::without_key)
            throw Exception("Unknown aggregated data variant.", ErrorCodes::UNKNOWN_AGGREGATED_DATA_VARIANT);
    }

    Block block;
    if (result.type == AggregatedDataVariants::Type::without_key || is_overflows)
        block = prepareBlockAndFillWithoutKey(result, final, is_overflows, action);
    else
        block = prepareBlockAndFillSingleLevel(result, final, action);
    /// NOTE: two-level data is not possible here - chooseAggregationMethod chooses only among single-level methods.

    /// proton : starts
    if (shouldClearStates(action, final))
        result.aggregator = nullptr;
    /// proton : ends

    size_t rows = block.rows();
    size_t bytes = block.bytes();
    double elapsed_seconds = watch.elapsedSeconds();
    LOG_DEBUG(log, "Merged partially aggregated blocks. {} rows, {}. in {} sec. ({:.3f} rows/sec., {}/sec.)",
        rows, ReadableSize(bytes),
        elapsed_seconds, rows / elapsed_seconds,
        ReadableSize(bytes / elapsed_seconds));

    block.info.bucket_num = bucket_num;
    return block;
}

template <typename Method>
void NO_INLINE Aggregator::convertBlockToTwoLevelImpl(
    Method & method,
    Arena * pool,
    ColumnRawPtrs & key_columns,
    const Block & source,
    std::vector<Block> & destinations) const
{
    typename Method::State state(key_columns, key_sizes, aggregation_state_cache);

    size_t rows = source.rows();
    size_t columns = source.columns();

    /// Create a 'selector' that will contain bucket index for every row. It will be used to scatter rows to buckets.
    IColumn::Selector selector(rows);

    /// For every row.
    for (size_t i = 0; i < rows; ++i)
    {
        if constexpr (Method::low_cardinality_optimization)
        {
            if (state.isNullAt(i))
            {
                selector[i] = 0;
                continue;
            }
        }

        /// Calculate bucket number from row hash.
        auto hash = state.getHash(method.data, i, *pool);
        auto bucket = method.data.getBucketFromHash(hash);

        selector[i] = bucket;
    }

    size_t num_buckets = destinations.size();

    for (size_t column_idx = 0; column_idx < columns; ++column_idx)
    {
        const ColumnWithTypeAndName & src_col = source.getByPosition(column_idx);
        MutableColumns scattered_columns = src_col.column->scatter(num_buckets, selector);

        for (size_t bucket = 0, size = num_buckets; bucket < size; ++bucket)
        {
            if (!scattered_columns[bucket]->empty())
            {
                Block & dst = destinations[bucket];
                dst.info.bucket_num = static_cast<Int32>(bucket);
                dst.insert({std::move(scattered_columns[bucket]), src_col.type, src_col.name});
            }

            /** Inserted columns of type ColumnAggregateFunction will own states of aggregate functions
              *  by holding shared_ptr to source column. See ColumnAggregateFunction.h
              */
        }
    }
}


std::vector<Block> Aggregator::convertBlockToTwoLevel(const Block & block) const
{
    if (!block)
        return {};

    AggregatedDataVariants data;
    data.aggregator = this;

    ColumnRawPtrs key_columns(params.keys_size);

    /// Remember the columns we will work with
    for (size_t i = 0; i < params.keys_size; ++i)
        key_columns[i] = block.safeGetByPosition(i).column.get();

    AggregatedDataVariants::Type type = method_chosen;
    data.keys_size = params.keys_size;
    data.key_sizes = key_sizes;

#define M(NAME) \
    else if (type == AggregatedDataVariants::Type::NAME) \
        type = AggregatedDataVariants::Type::NAME ## _two_level;

    if (false) {} // NOLINT
    APPLY_FOR_VARIANTS_CONVERTIBLE_TO_STATIC_BUCKET_TWO_LEVEL(M)
#undef M
    else
        throw Exception("Unknown aggregated data variant.", ErrorCodes::UNKNOWN_AGGREGATED_DATA_VARIANT);

    data.init(type);

    size_t num_buckets = 0;

#define M(NAME) \
    else if (data.type == AggregatedDataVariants::Type::NAME) \
        num_buckets = data.NAME->data.buckets().size();

    if (false) {} // NOLINT
    APPLY_FOR_VARIANTS_STATIC_BUCKET_TWO_LEVEL(M)
#undef M
    else
        throw Exception("Unknown aggregated data variant.", ErrorCodes::UNKNOWN_AGGREGATED_DATA_VARIANT);

    std::vector<Block> splitted_blocks(num_buckets);

#define M(NAME) \
    else if (data.type == AggregatedDataVariants::Type::NAME) \
        convertBlockToTwoLevelImpl(*data.NAME, data.aggregates_pool, \
            key_columns, block, splitted_blocks);

    if (false) {} // NOLINT
    APPLY_FOR_VARIANTS_STATIC_BUCKET_TWO_LEVEL(M)
#undef M
    else
        throw Exception("Unknown aggregated data variant.", ErrorCodes::UNKNOWN_AGGREGATED_DATA_VARIANT);

    return splitted_blocks;
}


template <typename Method, typename Table>
void NO_INLINE Aggregator::destroyImpl(Table & table) const
{
    table.forEachMapped([&](AggregateDataPtr & data)
    {
        /** If an exception (usually a lack of memory, the MemoryTracker throws) arose
          *  after inserting the key into a hash table, but before creating all states of aggregate functions,
          *  then data will be equal nullptr.
          */
        if (nullptr == data)
            return;

        for (size_t i = 0; i < params.aggregates_size; ++i)
            aggregate_functions[i]->destroy(data + offsets_of_aggregate_states[i]);

        data = nullptr;
    });
}


void Aggregator::destroyWithoutKey(AggregatedDataVariants & result) const
{
    AggregatedDataWithoutKey & res_data = result.without_key;

    if (nullptr != res_data)
    {
        for (size_t i = 0; i < params.aggregates_size; ++i)
            aggregate_functions[i]->destroy(res_data + offsets_of_aggregate_states[i]);

        res_data = nullptr;
    }
}


void Aggregator::destroyAllAggregateStates(AggregatedDataVariants & result) const
{
    if (result.empty())
        return;

    LOG_TRACE(log, "Destroying aggregate states");

    /// In what data structure is the data aggregated?
    if (result.type == AggregatedDataVariants::Type::without_key || params.overflow_row)
        destroyWithoutKey(result);

#define M(NAME, IS_TWO_LEVEL) \
    else if (result.type == AggregatedDataVariants::Type::NAME) \
        destroyImpl<decltype(result.NAME)::element_type>(result.NAME->data);

    if (false) {} // NOLINT
    APPLY_FOR_AGGREGATED_VARIANTS_STREAMING(M)
#undef M
    else if (result.type != AggregatedDataVariants::Type::without_key)
        throw Exception("Unknown aggregated data variant.", ErrorCodes::UNKNOWN_AGGREGATED_DATA_VARIANT);
}

/// proton: starts. for streaming processing
void Aggregator::initStatesForWithoutKeyOrOverflow(AggregatedDataVariants & data_variants) const
{
    if (!data_variants.without_key && (params.overflow_row || data_variants.type == AggregatedDataVariants::Type::without_key))
    {
        AggregateDataPtr place = data_variants.aggregates_pool->alignedAlloc(total_size_of_aggregate_states, align_aggregate_states);
        createAggregateStates(place);
        data_variants.without_key = place;
    }
}

/// Loop the window column to find out the lower bound and set this lower bound to aggregates pool
/// Any new memory allocation (MemoryChunk) will attach this lower bound timestamp which means
/// the MemoryChunk contains states which is at and beyond this lower bound timestamp
void Aggregator::setupAggregatesPoolTimestamps(size_t row_begin, size_t row_end, const ColumnRawPtrs & key_columns, Arena * aggregates_pool) const
{
    if (params.group_by != Params::GroupBy::WINDOW_START && params.group_by != Params::GroupBy::WINDOW_END)
        return;

    Int64 max_timestamp = std::numeric_limits<Int64>::min();

    /// FIXME, can we avoid this loop ?
    auto & window_col = *key_columns[0];
    for (size_t i = row_begin; i < row_end; ++i)
    {
        auto window = window_col.getInt(i);
        if (window > max_timestamp)
            max_timestamp = window;
    }
    aggregates_pool->setCurrentTimestamp(max_timestamp);
    LOG_DEBUG(log, "Set current pool timestamp watermark={}", max_timestamp);
}

void Aggregator::removeBucketsBefore(AggregatedDataVariants & result, Int64 max_bucket) const
{
    if (result.empty())
        return;

    auto destroy = [&](AggregateDataPtr & data)
    {
        if (nullptr == data)
            return;

        for (size_t i = 0; i < params.aggregates_size; ++i)
            aggregate_functions[i]->destroy(data + offsets_of_aggregate_states[i]);

        data = nullptr;
    };

    size_t removed = 0;
    Int64 last_removed_time_bukect = 0;
    size_t remaining = 0;

    switch (result.type)
    {
#define M(NAME) \
            case AggregatedDataVariants::Type::NAME: \
                std::tie(removed, last_removed_time_bukect, remaining) = result.NAME->data.removeBucketsBefore(max_bucket, destroy); break;
        APPLY_FOR_VARIANTS_TIME_BUCKET_TWO_LEVEL(M)
#undef M

        default:
            break;
    }

    Arena::Stats stats;

    if (removed)
        stats = result.aggregates_pool->free(last_removed_time_bukect);

    LOG_INFO(
        log,
        "Removed {} windows less or equal to {}={}, keeping window_count={}, remaining_windows={}. "
        "Arena: arena_chunks={}, arena_size={}, chunks_removed={}, bytes_removed={}. chunks_reused={}, bytes_reused={}, head_chunk_size={}, "
        "free_list_hits={}, free_list_missed={}",
        removed,
        params.group_by == Params::GroupBy::WINDOW_END ? "window_end" : "window_start",
        max_bucket,
        params.streaming_window_count,
        remaining,
        stats.chunks,
        stats.bytes,
        stats.chunks_removed,
        stats.bytes_removed,
        stats.chunks_reused,
        stats.bytes_reused,
        stats.head_chunk_size,
        stats.free_list_hits,
        stats.free_list_misses);
}

std::vector<Int64> Aggregator::bucketsBefore(const AggregatedDataVariants & result, Int64 max_bucket) const
{
    switch (result.type)
    {
#define M(NAME) \
            case AggregatedDataVariants::Type::NAME: return result.NAME->data.bucketsBefore(max_bucket);
        APPLY_FOR_VARIANTS_TIME_BUCKET_TWO_LEVEL(M)
#undef M

        default:
            break;
    }

    return {};
}

/// The complexity of checkpoint the state of Aggregator is a combination of the following 2 cases
/// 1) The keys can reside in hashmap or in arena
/// 2) The state can reside in arena or in the aggregation function
/// And there is a special one which is group without key
void Aggregator::checkpoint(const AggregatedDataVariants & data_variants, WriteBuffer & wb)
{
    /// Serialization layout
    /// [version][uint8][uint16][aggr-func-state-block]

    writeIntBinary(getVersion(), wb);

    UInt8 inited = 1;
    if (data_variants.empty())
    {
        /// No aggregated data yet
        inited = 0;
        writeIntBinary(inited, wb);
        return;
    }

    writeIntBinary(inited, wb);

    UInt16 num_aggr_funcs = aggregate_functions.size();
    writeIntBinary(num_aggr_funcs, wb);

    /// FIXME, set a good max_threads
    /// For ConvertAction::CHECKPOINT, don't clear state `data_variants`
    auto blocks = convertToBlocks(const_cast<AggregatedDataVariants &>(data_variants), false, ConvertAction::CHECKPOINT, 8);

    /// assert(!blocks.empty());

    UInt32 num_blocks = static_cast<UInt32>(blocks.size());
    writeIntBinary(num_blocks, wb);
    NativeLightChunkWriter writer(wb, getHeader(false), ProtonRevision::getVersionRevision());
    for (const auto & block : blocks)
        writer.write(block);
}

void Aggregator::recover(AggregatedDataVariants & data_variants, ReadBuffer & rb)
{
    version = 0;
    readIntBinary(*version, rb);

    UInt8 inited = 0;
    readIntBinary(inited, rb);

    if (!inited)
        return;

    UInt16 num_aggr_funcs = 0;
    readIntBinary(num_aggr_funcs, rb);

    if (num_aggr_funcs != aggregate_functions.size())
        throw Exception(
            ErrorCodes::RECOVER_CHECKPOINT_FAILED,
            "Failed to recover aggregation function checkpoint. Number of aggregation functions are not the same, checkpointed={}, "
            "current={}",
            num_aggr_funcs,
            aggregate_functions.size());

    UInt32 num_blocks = 0;
    readIntBinary(num_blocks, rb);
    BlocksList blocks;
    const auto & header = getHeader(false);
    NativeLightChunkReader reader(rb, header, ProtonRevision::getVersionRevision());
    for (size_t i = 0; i < num_blocks; ++i)
    {
        auto light_chunk = reader.read();
        blocks.emplace_back(header.cloneWithColumns(light_chunk.detachColumns()));
    }

    /// Restore data variants from blocks
    recoverStates(data_variants, blocks);
}

void Aggregator::recoverStates(AggregatedDataVariants & data_variants, BlocksList & blocks)
{
    data_variants.aggregator = this;
    if (data_variants.empty())
    {
        data_variants.keys_size = params.keys_size;
        data_variants.key_sizes = key_sizes;
        data_variants.init(method_chosen);
    }

    if (blocks.empty())
        return;

    if (data_variants.type == AggregatedDataVariants::Type::without_key)
    {
        recoverStatesWithoutKey(data_variants, blocks);
    }
    else
    {
        if (data_variants.isTwoLevel())
        {
            /// data variants is inited with two level hashmap =>
            /// windowed aggregation cases
            recoverStatesTwoLevel(data_variants, blocks);
        }
        else
        {
            /// There are 2 cases
            /// 1) data variants is inited with single level hashmap, however the checkpoint states are 2 levels
            ///    which means data variants was converted to two level
            /// 2) data variants is inited with single level and stayed as single level
            if (blocks.front().info.hasBucketNum())
            {
                /// We will need convert data_variants to two level
                data_variants.convertToTwoLevel();
                recoverStatesTwoLevel(data_variants, blocks);
            }
            else
                recoverStatesSingleLevel(data_variants, blocks);
        }
    }
}

void Aggregator::recoverStatesWithoutKey(AggregatedDataVariants & data_variants, BlocksList & blocks)
{
    /// Only one block
    assert(blocks.size() == 1);
    auto & block = blocks.front();
    /// Only one row
    assert(block.rows() == 1);
    /// Column size is same as number of aggregated functions
    assert(block.columns() == params.aggregates_size);

    /// SELECT max(i), min(i), avg(i) FROM t;
    /// States in Block is columnar
    /// s11 | s12 | s13
    /// Convert columnar to row based with padding.
    /// s11 s12 s13
    /// We also need move the aggregation functions over since they
    /// may have internal states as well

    assert(!data_variants.without_key);
    initStatesForWithoutKeyOrOverflow(data_variants);
    AggregatedDataWithoutKey & data = data_variants.without_key;

    AggregateColumnsData aggregate_columns(params.aggregates_size);

    for (size_t i = 0; i < params.aggregates_size; ++i)
    {
        const auto & aggregate_column_name = params.aggregates[i].column_name;
        auto & aggr_func_col = typeid_cast<ColumnAggregateFunction &>(block.getByName(aggregate_column_name).column->assumeMutableRef());

        /// Move over the function
        const_cast<AggregateDescription &>(params.aggregates[i]).function = aggr_func_col.getAggregateFunction();
        aggregate_functions[i] = params.aggregates[i].function.get();

        /// Remember the columns we will work with
        aggregate_columns[i] = &aggr_func_col.getData();
    }

    /// Move over the state
    for (size_t i = 0; i < params.aggregates_size; ++i)
        aggregate_functions[i]->move(data + offsets_of_aggregate_states[i], (*aggregate_columns[i])[0], data_variants.aggregates_pool);
}

void Aggregator::recoverStatesSingleLevel(AggregatedDataVariants & data_variants, BlocksList & blocks)
{
    /// SELECT max(i), min(i), avg(i) FROM t GROUP BY k1, k2;
    /// States in Block is columnar
    /// k11 | k12 | s11 | s12 | s13
    /// k21 | k22 | s21 | s22 | s23
    /// k31 | k32 | s31 | s32 | s33
    /// Convert columnar to row based. Basically we need re-insert keys to hashtable
    /// k11 k12 s11 s12 s13
    /// -------------------
    /// k21 k22 s21 s22 s23
    /// -------------------
    /// k31 k32 s31 s32 s33
    /// We also need move the aggregation functions over since they
    /// may have internal states as well
    /// Key columns + AggregateFunction columns

    {
        /// Move over the aggr function
        auto & block = blocks.front();

        for (size_t i = 0; i < params.aggregates_size; ++i)
        {
            const auto & aggregate_column_name = params.aggregates[i].column_name;
            auto & aggr_func_col = typeid_cast<ColumnAggregateFunction &>(block.getByName(aggregate_column_name).column->assumeMutableRef());

            /// Move over the function
            const_cast<AggregateDescription &>(params.aggregates[i]).function = aggr_func_col.getAggregateFunction();
            aggregate_functions[i] = params.aggregates[i].function.get();
        }
    }

    for (auto & block : blocks)
    {
       #define M(NAME, IS_TWO_LEVEL) \
            else if (data_variants.type == AggregatedDataVariants::Type::NAME) \
                doRecoverStates(*data_variants.NAME, data_variants.aggregates_pool, block);

        if (false) {} // NOLINT
        APPLY_FOR_AGGREGATED_VARIANTS_STREAMING(M)
        #undef M
    }
}

template <typename Method>
void NO_INLINE Aggregator::doRecoverStates(Method & method, Arena * aggregates_pool, Block & block)
{
    /// Remember the key columns
    ColumnRawPtrs key_columns;
    key_columns.resize(params.keys_size);

    for (size_t k = 0; k < params.keys_size; ++k)
    {
        /// const auto & key_column_name = params.src_header.safeGetByPosition(params.keys[k]).name;
        /// key_columns[k] = block.getByName(key_column_name).column.get();

        /// Since Aggregator::Params::getHeader create header in the order of its keys
        /// we can assume the first keys_size column of block are keys
        key_columns[k] = block.getByPosition(k).column.get();
    }

    typename Method::State state(key_columns, key_sizes, aggregation_state_cache);

    /// Reinsert the keys
    if (params.aggregates_size == 0)
    {
        /// SELECT k1, k2 FROM t GROUP BY k1, k2
        AggregateDataPtr place = aggregates_pool->alloc(0);
        for (size_t row = 0, rows = block.rows(); row < rows; ++row)
            state.emplaceKey(method.data, row, *aggregates_pool).setMapped(place);
        return;
    }

    /// Remember aggregate columns
    AggregateColumnsData aggregate_columns(params.aggregates_size);

    for (size_t i = 0; i < params.aggregates_size; ++i)
    {
        const auto & aggregate_column_name = params.aggregates[i].column_name;
        auto & aggr_func_col = typeid_cast<ColumnAggregateFunction &>(block.getByName(aggregate_column_name).column->assumeMutableRef());
        /// Remember the columns we will work with
        aggregate_columns[i] = &aggr_func_col.getData();
    }

    for (size_t row = 0, rows = block.rows(); row < rows; ++row)
    {
        auto emplace_result = state.emplaceKey(method.data, row, *aggregates_pool);
        /// Since we are recovering from states. Each row has unique keys
        assert (emplace_result.isInserted());
        emplace_result.setMapped(nullptr);

        /// Allocate states for all aggregate functions
        AggregateDataPtr aggregate_data = aggregates_pool->alignedAlloc(total_size_of_aggregate_states, align_aggregate_states);
        createAggregateStates(aggregate_data);

        /// Move over the state
        for (size_t i = 0; i < params.aggregates_size; ++i)
            aggregate_functions[i]->move(aggregate_data + offsets_of_aggregate_states[i], (*aggregate_columns[i])[row], aggregates_pool);

        emplace_result.setMapped(aggregate_data);
    }

    setupAggregatesPoolTimestamps(0, block.rows(), key_columns, aggregates_pool);
}


void Aggregator::recoverStatesTwoLevel(AggregatedDataVariants & data_variants, BlocksList & blocks)
{
    /// There are 2 cases
    /// 1. Global aggregation has 2 level
    /// 2. Windowed aggregation has 2 level (actually it may have 3 levels which can be supported in future)
    /// FIXME, concurrent recover
    recoverStatesSingleLevel(data_variants, blocks);
}

bool Aggregator::shouldClearStates(ConvertAction action, bool final_) const
{
    /// For streaming processing, data_variants.aggregator will never be nullptr once set
    /// and we will never move the ownership of the states to `ColumnAggregateFunction`
    /// unless we don't need keep the states

    switch (action)
    {
        case ConvertAction::DISTRIBUTED_MERGE:
            /// Distributed processing case. Only clear states on initiator
            return final_;
        case ConvertAction::WRITE_TO_TEMP_FS:
            /// We are dumping all states to file system in case of memory is not efficient
            /// In this case, we should not keep the states
            return true;
        case ConvertAction::CHECKPOINT:
            /// Checkpoint is snapshot of in-memory states, we shall not clear the states
            return false;
        case ConvertAction::INTERNAL_MERGE:
            return false;
        case ConvertAction::STREAMING_EMIT:
            [[fallthrough]];
        default:
            /// By default, streaming processing needs hold on to the states
            return !params.keep_state;
    }
}

VersionType Aggregator::getVersionFromRevision(UInt64 revision) const
{
    if (version)
        return *version;

    return static_cast<VersionType>(revision);
}

VersionType Aggregator::getVersion() const
{
    auto ver = getVersionFromRevision(ProtonRevision::getVersionRevision());

    if (!version)
        version = ver;

    return ver;
}

template <typename Method>
void NO_INLINE Aggregator::spliceBucketsImpl(
    AggregatedDataVariants & data_dest,
    AggregatedDataVariants & data_src,
    bool final,
    ConvertAction action,
    const std::vector<Int64> & gcd_buckets,
    Arena * arena) const
{
    /// In order to merge state with same other keys of different gcd buckets, reset the window group keys to zero
    /// create a new key, where the window key part is 0, and the other key parts are the same as the original value.
    /// For example:
    /// Key :           <window_start> +  <window_end> +  <other_keys...>
    /// New key :                0     +        0      +  <other_keys...>
    auto zero_out_window_keys_func = [&](const auto & key) {
        if constexpr (std::is_same_v<std::decay_t<decltype(key)>, StringRef>)
        {
            /// Case-1: Serialized key, the window time keys always are always lower bits
            auto * data = const_cast<char *>(arena->insert(key.data, key.size));
            assert(data != nullptr);
            auto window_keys_size = params.window_keys_num == 2 ? key_sizes[0] + key_sizes[1] : key_sizes[0];
            std::memset(data, 0, window_keys_size);

            return SerializedKeyHolder{StringRef(data, key.size), *arena};
        }
        else
        {
            /// Case-2: Only one window time key
            if (key_sizes.size() == 1)
                return static_cast<std::decay_t<decltype(key)>>(0);

            /// Case-3: Fixed key, the fixed window time keys always are always lower bits
            auto window_keys_size = params.window_keys_num == 2 ? key_sizes[0] + key_sizes[1] : key_sizes[0];
            auto bit_nums = window_keys_size << 3;
            return (key >> bit_nums) << bit_nums; /// reset lower bits to zero
        }
    };

    auto clear_states = shouldClearStates(action, final);
    auto & table_dest = getDataVariant<Method>(data_dest).data.impls;
    auto & table_src = getDataVariant<Method>(data_src).data.impls;

#if USE_EMBEDDED_COMPILER
    if (compiled_aggregate_functions_holder)
    {
        for (auto bucket : gcd_buckets)
            mergeDataImpl<Method, true>(table_dest[0], table_src[bucket], arena, clear_states, zero_out_window_keys_func);
    }
    else
#endif
    {
        for (auto bucket : gcd_buckets)
            mergeDataImpl<Method, false>(table_dest[0], table_src[bucket], arena, clear_states, zero_out_window_keys_func);
    }
}

Block Aggregator::spliceAndConvertBucketsToBlock(
    AggregatedDataVariants & variants, bool final, ConvertAction action, const std::vector<Int64> & gcd_buckets) const
{
    AggregatedDataVariants result_variants;
    result_variants.aggregator = this;
    result_variants.keys_size = params.keys_size;
    result_variants.key_sizes = key_sizes;
    result_variants.init(method_chosen);
    initStatesForWithoutKeyOrOverflow(result_variants);

    auto method = result_variants.type;
    Arena * arena = result_variants.aggregates_pool;

    if (false) {} // NOLINT
#define M(NAME) \
    else if (method == AggregatedDataVariants::Type::NAME) \
    { \
        spliceBucketsImpl<decltype(result_variants.NAME)::element_type>(result_variants, variants, final, action, gcd_buckets, arena); \
        return convertOneBucketToBlock(result_variants, *result_variants.NAME, arena, final, action, 0); \
    }

    APPLY_FOR_VARIANTS_TIME_BUCKET_TWO_LEVEL(M)
#undef M
    else
        throw Exception("Unknown aggregated data variant.", ErrorCodes::UNKNOWN_AGGREGATED_DATA_VARIANT);

    UNREACHABLE();
}

void Aggregator::mergeBuckets(
    ManyAggregatedDataVariants & variants, Arena * arena, bool final, ConvertAction action, const std::vector<Int64> & buckets) const
{
    auto & merge_data = *variants[0];

    if (false) {} // NOLINT
#define M(NAME) \
    else if (merge_data.type == AggregatedDataVariants::Type::NAME) \
    { \
        for (auto bucket : buckets) \
            mergeBucketImpl<decltype(merge_data.NAME)::element_type>(variants, final, action, bucket, arena); \
    }

    APPLY_FOR_VARIANTS_TIME_BUCKET_TWO_LEVEL(M)
    else
        throw Exception("Unknown aggregated data variant.", ErrorCodes::UNKNOWN_AGGREGATED_DATA_VARIANT);
#undef M
}
/// proton: ends
}
}
