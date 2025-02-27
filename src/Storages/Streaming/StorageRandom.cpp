#include <Columns/ColumnArray.h>
#include <Columns/ColumnFixedString.h>
#include <Columns/ColumnNullable.h>
#include <Columns/ColumnString.h>
#include <Columns/ColumnTuple.h>
#include <Columns/ColumnVector.h>
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypeDateTime64.h>
#include <DataTypes/DataTypeDecimalBase.h>
#include <DataTypes/DataTypeEnum.h>
#include <DataTypes/DataTypeFixedString.h>
#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/DataTypeTuple.h>
#include <DataTypes/NestedUtils.h>
#include <Functions/FunctionFactory.h>
#include <Interpreters/inplaceBlockConversions.h>
#include <Parsers/ASTCreateQuery.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTSetQuery.h>
#include <Processors/ISource.h>
#include <Processors/QueryPlan/QueryPlan.h>
#include <Processors/QueryPlan/ReadFromPreparedSource.h>
#include <Processors/Sources/SourceFromSingleChunk.h>
#include <QueryPipeline/Pipe.h>
#include <Storages/ColumnsDescription.h>
#include <Storages/IStorage.h>
#include <Storages/SelectQueryInfo.h>
#include <Storages/StorageFactory.h>
#include <Storages/Streaming/StorageRandom.h>
#include <base/ClockUtils.h>
#include <base/unaligned.h>
#include <Common/ProtonCommon.h>
#include <Common/SipHash.h>
#include <Common/randomSeed.h>


namespace DB
{

namespace ErrorCodes
{
extern const int NOT_IMPLEMENTED;
extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
extern const int TOO_LARGE_ARRAY_SIZE;
extern const int TOO_LARGE_STRING_SIZE;
extern const int UNKNOWN_SETTING;
}


namespace
{

void fillBufferWithRandomData(char * __restrict data, size_t size, pcg64 & rng)
{
    char * __restrict end = data + size;
    while (data < end)
    {
        /// The loop can be further optimized.
        UInt64 number = rng();
        unalignedStore<UInt64>(data, number);
        data += sizeof(UInt64); /// We assume that data has at least 7-byte padding (see PaddedPODArray)
    }
}


ColumnPtr fillColumnWithRandomData(const DataTypePtr type, UInt64 limit, pcg64 & rng, ContextPtr context)
{
    TypeIndex idx = type->getTypeId();
    int max_string_length = 20;
    int max_array_length = 20;
    switch (idx)
    {
        case TypeIndex::String: {
            /// Mostly the same as the implementation of randomPrintableASCII function.

            auto column = ColumnString::create();
            ColumnString::Chars & data_to = column->getChars();
            ColumnString::Offsets & offsets_to = column->getOffsets();
            offsets_to.resize(limit);

            IColumn::Offset offset = 0;
            for (size_t row_num = 0; row_num < limit; ++row_num)
            {
                size_t length = rng() % (max_string_length + 1); /// Slow

                IColumn::Offset next_offset = offset + length + 1;
                data_to.resize(next_offset);
                offsets_to[row_num] = next_offset;

                auto * data_to_ptr = data_to.data(); /// avoid assert on array indexing after end
                for (size_t pos = offset, end = offset + length; pos < end;
                     pos += 4) /// We have padding in column buffers that we can overwrite.
                {
                    UInt64 rand = rng();

                    UInt16 rand1 = rand;
                    UInt16 rand2 = rand >> 16;
                    UInt16 rand3 = rand >> 32;
                    UInt16 rand4 = rand >> 48;

                    /// Printable characters are from range [32; 126].
                    /// https://lemire.me/blog/2016/06/27/a-fast-alternative-to-the-modulo-reduction/

                    data_to_ptr[pos + 0] = 32 + ((rand1 * 95) >> 16);
                    data_to_ptr[pos + 1] = 32 + ((rand2 * 95) >> 16);
                    data_to_ptr[pos + 2] = 32 + ((rand3 * 95) >> 16);
                    data_to_ptr[pos + 3] = 32 + ((rand4 * 95) >> 16);

                    /// NOTE gcc failed to vectorize this code (aliasing of char?)
                    /// TODO Implement SIMD optimizations from Danila Kutenin.
                }

                data_to[offset + length] = 0;

                offset = next_offset;
            }

            return column;
        }

        case TypeIndex::Enum8: {
            auto column = ColumnVector<Int8>::create();
            auto values = typeid_cast<const DataTypeEnum<Int8> *>(type.get())->getValues();
            auto & data = column->getData();
            data.resize(limit);

            UInt8 size = values.size();
            UInt8 off;
            for (UInt64 i = 0; i < limit; ++i)
            {
                off = static_cast<UInt8>(rng()) % size;
                data[i] = values[off].second;
            }

            return column;
        }

        case TypeIndex::Enum16: {
            auto column = ColumnVector<Int16>::create();
            auto values = typeid_cast<const DataTypeEnum<Int16> *>(type.get())->getValues();
            auto & data = column->getData();
            data.resize(limit);

            UInt16 size = values.size();
            UInt8 off;
            for (UInt64 i = 0; i < limit; ++i)
            {
                off = static_cast<UInt16>(rng()) % size;
                data[i] = values[off].second;
            }

            return column;
        }

        case TypeIndex::Array: {
            auto nested_type = typeid_cast<const DataTypeArray *>(type.get())->getNestedType();

            auto offsets_column = ColumnVector<ColumnArray::Offset>::create();
            auto & offsets = offsets_column->getData();

            UInt64 offset = 0;
            offsets.resize(limit);
            for (UInt64 i = 0; i < limit; ++i)
            {
                offset += static_cast<UInt64>(rng()) % (max_array_length + 1);
                offsets[i] = offset;
            }

            auto data_column = fillColumnWithRandomData(nested_type, offset, rng, context);

            return ColumnArray::create(std::move(data_column), std::move(offsets_column)); /// NOLINT(performance-move-const-arg)
        }

        case TypeIndex::Tuple: {
            auto elements = typeid_cast<const DataTypeTuple *>(type.get())->getElements();
            const size_t tuple_size = elements.size();
            Columns tuple_columns(tuple_size);

            for (size_t i = 0; i < tuple_size; ++i)
                tuple_columns[i] = fillColumnWithRandomData(elements[i], limit, rng, context);

            return ColumnTuple::create(std::move(tuple_columns)); /// NOLINT(performance-move-const-arg)
        }

        case TypeIndex::Nullable: {
            auto nested_type = typeid_cast<const DataTypeNullable *>(type.get())->getNestedType();
            auto nested_column = fillColumnWithRandomData(nested_type, limit, rng, context);

            auto null_map_column = ColumnUInt8::create();
            auto & null_map = null_map_column->getData();
            null_map.resize(limit);
            for (UInt64 i = 0; i < limit; ++i)
                null_map[i] = rng() % 16 == 0; /// No real motivation for this.

            return ColumnNullable::create(std::move(nested_column), std::move(null_map_column)); /// NOLINT(performance-move-const-arg)
        }
        case TypeIndex::UInt8: {
            auto column = ColumnUInt8::create();
            column->getData().resize(limit);
            fillBufferWithRandomData(reinterpret_cast<char *>(column->getData().data()), limit * sizeof(UInt8), rng);
            return column;
        }
        case TypeIndex::UInt16:
            [[fallthrough]];
        case TypeIndex::Date: {
            auto column = ColumnUInt16::create();
            column->getData().resize(limit);
            fillBufferWithRandomData(reinterpret_cast<char *>(column->getData().data()), limit * sizeof(UInt16), rng);
            return column;
        }
        case TypeIndex::Date32: {
            auto column = ColumnInt32::create();
            column->getData().resize(limit);
            fillBufferWithRandomData(reinterpret_cast<char *>(column->getData().data()), limit * sizeof(Int32), rng);
            return column;
        }
        case TypeIndex::UInt32:
            [[fallthrough]];
        case TypeIndex::DateTime: {
            auto column = ColumnUInt32::create();
            column->getData().resize(limit);
            fillBufferWithRandomData(reinterpret_cast<char *>(column->getData().data()), limit * sizeof(UInt32), rng);
            return column;
        }
        case TypeIndex::UInt64: {
            auto column = ColumnUInt64::create();
            column->getData().resize(limit);
            fillBufferWithRandomData(reinterpret_cast<char *>(column->getData().data()), limit * sizeof(UInt64), rng);
            return column;
        }
        case TypeIndex::UInt128: {
            auto column = ColumnUInt128::create();
            column->getData().resize(limit);
            fillBufferWithRandomData(reinterpret_cast<char *>(column->getData().data()), limit * sizeof(UInt128), rng);
            return column;
        }
        case TypeIndex::UInt256: {
            auto column = ColumnUInt256::create();
            column->getData().resize(limit);
            fillBufferWithRandomData(reinterpret_cast<char *>(column->getData().data()), limit * sizeof(UInt256), rng);
            return column;
        }
        case TypeIndex::UUID: {
            auto column = ColumnUUID::create();
            column->getData().resize(limit);
            /// NOTE This is slightly incorrect as random UUIDs should have fixed version 4.
            fillBufferWithRandomData(reinterpret_cast<char *>(column->getData().data()), limit * sizeof(UUID), rng);
            return column;
        }
        case TypeIndex::Int8: {
            auto column = ColumnInt8::create();
            column->getData().resize(limit);
            fillBufferWithRandomData(reinterpret_cast<char *>(column->getData().data()), limit * sizeof(Int8), rng);
            return column;
        }
        case TypeIndex::Int16: {
            auto column = ColumnInt16::create();
            column->getData().resize(limit);
            fillBufferWithRandomData(reinterpret_cast<char *>(column->getData().data()), limit * sizeof(Int16), rng);
            return column;
        }
        case TypeIndex::Int32: {
            auto column = ColumnInt32::create();
            column->getData().resize(limit);
            fillBufferWithRandomData(reinterpret_cast<char *>(column->getData().data()), limit * sizeof(Int32), rng);
            return column;
        }
        case TypeIndex::Int64: {
            auto column = ColumnInt64::create();
            column->getData().resize(limit);
            fillBufferWithRandomData(reinterpret_cast<char *>(column->getData().data()), limit * sizeof(Int64), rng);
            return column;
        }
        case TypeIndex::Int128: {
            auto column = ColumnInt128::create();
            column->getData().resize(limit);
            fillBufferWithRandomData(reinterpret_cast<char *>(column->getData().data()), limit * sizeof(Int128), rng);
            return column;
        }
        case TypeIndex::Int256: {
            auto column = ColumnInt256::create();
            column->getData().resize(limit);
            fillBufferWithRandomData(reinterpret_cast<char *>(column->getData().data()), limit * sizeof(Int256), rng);
            return column;
        }
        case TypeIndex::Float32: {
            auto column = ColumnFloat32::create();
            column->getData().resize(limit);
            fillBufferWithRandomData(reinterpret_cast<char *>(column->getData().data()), limit * sizeof(Float32), rng);
            return column;
        }
        case TypeIndex::Float64: {
            auto column = ColumnFloat64::create();
            column->getData().resize(limit);
            fillBufferWithRandomData(reinterpret_cast<char *>(column->getData().data()), limit * sizeof(Float64), rng);
            return column;
        }
        case TypeIndex::Decimal32: {
            auto column = type->createColumn();
            auto & column_concrete = typeid_cast<ColumnDecimal<Decimal32> &>(*column);
            column_concrete.getData().resize(limit);
            fillBufferWithRandomData(reinterpret_cast<char *>(column_concrete.getData().data()), limit * sizeof(Decimal32), rng);
            return column;
        }
        case TypeIndex::Decimal64: /// TODO Decimal may be generated out of range.
        {
            auto column = type->createColumn();
            auto & column_concrete = typeid_cast<ColumnDecimal<Decimal64> &>(*column);
            column_concrete.getData().resize(limit);
            fillBufferWithRandomData(reinterpret_cast<char *>(column_concrete.getData().data()), limit * sizeof(Decimal64), rng);
            return column;
        }
        case TypeIndex::Decimal128: {
            auto column = type->createColumn();
            auto & column_concrete = typeid_cast<ColumnDecimal<Decimal128> &>(*column);
            column_concrete.getData().resize(limit);
            fillBufferWithRandomData(reinterpret_cast<char *>(column_concrete.getData().data()), limit * sizeof(Decimal128), rng);
            return column;
        }
        case TypeIndex::Decimal256: {
            auto column = type->createColumn();
            auto & column_concrete = typeid_cast<ColumnDecimal<Decimal256> &>(*column);
            column_concrete.getData().resize(limit);
            fillBufferWithRandomData(reinterpret_cast<char *>(column_concrete.getData().data()), limit * sizeof(Decimal256), rng);
            return column;
        }
        case TypeIndex::FixedString: {
            size_t n = typeid_cast<const DataTypeFixedString &>(*type).getN();
            auto column = ColumnFixedString::create(n);
            column->getChars().resize(limit * n);
            fillBufferWithRandomData(reinterpret_cast<char *>(column->getChars().data()), limit * n, rng);
            return column;
        }
        case TypeIndex::DateTime64: {
            auto column = type->createColumn();
            auto & column_concrete = typeid_cast<ColumnDecimal<DateTime64> &>(*column);
            column_concrete.getData().resize(limit);

            UInt64 range = (1ULL << 32) * intExp10(typeid_cast<const DataTypeDateTime64 &>(*type).getScale());

            for (size_t i = 0; i < limit; ++i)
                column_concrete.getData()[i] = rng() % range; /// Slow

            return column;
        }

        default:
            throw Exception("The 'Generating' is not implemented for type " + type->getName(), ErrorCodes::NOT_IMPLEMENTED);
    }
}


class GenerateRandomSource final : public ISource
{
public:
    GenerateRandomSource(
        UInt64 block_size_,
        UInt64 random_seed_,
        Block block_header_,
        const ColumnsDescription our_columns_,
        ContextPtr context_,
        UInt64 events_per_second_)
        : ISource(Nested::flatten(prepareBlockToFill(block_header_)), true, ProcessorID::GenerateRandomSourceID)
        , block_size(block_size_)
        , block_full(std::move(block_header_))
        , our_columns(our_columns_)
        , rng(random_seed_)
        , context(context_)
        , events_per_second(events_per_second_)
        , header_chunk(Nested::flatten(block_full.cloneEmpty()).getColumns(), 0)
    {
        is_streaming = true;

        boundary_time = MonotonicMilliseconds::now() + generate_interval;
        block_idx_in_window = 0;
        max_full_block_count = events_per_second_ / block_size;
        partial_size = events_per_second_ % block_size;

        for (const auto & elem : block_full)
        {
            bool is_reserved_column
                = std::find(ProtonConsts::RESERVED_COLUMN_NAMES.begin(), ProtonConsts::RESERVED_COLUMN_NAMES.end(), elem.name)
                != ProtonConsts::RESERVED_COLUMN_NAMES.end();
            if (is_reserved_column || our_columns.hasDefault(elem.name))
                continue;
            block_to_fill.insert(elem);
        }
    }

    String getName() const override { return "Random"; }

protected:
    Chunk generate() override
    {
        if (events_per_second != 0)
        {
            auto now_time = MonotonicMilliseconds::now();

            if (now_time >= boundary_time)
            {
                boundary_time += generate_interval;
                block_idx_in_window = 0;
            }

            UInt64 batch_size = 0;

            if (block_idx_in_window == 0) // The size of the first generated chunk is partial_size (events_per_second % block_size).
                batch_size = partial_size;
            else if (block_idx_in_window <= max_full_block_count) // The remaining chunk size is block size.
                batch_size = block_size;

            block_idx_in_window++;

            return doGenerate(batch_size);
        }
        else
            return doGenerate(block_size);
    }

    Chunk doGenerate(UInt64 block_size)
    {
        if (block_size == 0)
            return header_chunk.clone();

        Columns columns;
        columns.reserve(block_full.columns());

        Block block_to_fill_as_result(block_to_fill.cloneEmpty());

        for (const auto & elem : block_to_fill_as_result)
            columns.emplace_back(fillColumnWithRandomData(elem.type, block_size, rng, context));

        block_to_fill_as_result.setColumns(columns);

        //TODO:be careful when a default column is named _dummy
        //e.g. create stream test(_dummy string default  rand_string());

        if (block_to_fill_as_result.columns() == 0)
            block_to_fill_as_result.insert(
                {ColumnConst::create(ColumnUInt8::create(1, 0), block_size), std::make_shared<DataTypeUInt8>(), "_dummy"});

        auto dag = evaluateMissingDefaults(block_to_fill_as_result, block_full.getNamesAndTypesList(), our_columns, context);
        if (dag)
        {
            auto actions = std::make_shared<ExpressionActions>(
                std::move(dag), ExpressionActionsSettings::fromContext(context, CompileExpressions::yes));
            actions->execute(block_to_fill_as_result);
        }

        if (block_to_fill_as_result.has(ProtonConsts::RESERVED_COLUMN_NAMES[0])
            && block_to_fill_as_result.has(ProtonConsts::RESERVED_COLUMN_NAMES[1]))
            block_to_fill_as_result.getByName(ProtonConsts::RESERVED_COLUMN_NAMES[1]).column
                = block_to_fill_as_result.getByName(ProtonConsts::RESERVED_COLUMN_NAMES[0]).column->cloneResized(block_size);
        if (block_to_fill_as_result.has("_dummy"))
            block_to_fill_as_result.erase("_dummy");

        columns = Nested::flatten(block_to_fill_as_result).getColumns();
        return {std::move(columns), block_size};
    }

private:
    UInt64 block_size;
    Block block_full;
    Block block_to_fill;
    const ColumnsDescription our_columns;
    pcg64 rng;
    ContextPtr context;
    UInt64 boundary_time;
    UInt64 block_idx_in_window;
    UInt64 events_per_second;
    UInt64 max_full_block_count;
    UInt64 partial_size;
    Chunk header_chunk;
    // Set the size of a window for random storages to generate data, measured in milliseconds.
    static constexpr UInt64 generate_interval = 1000; 

    static Block & prepareBlockToFill(Block & block)
    {
        /// To support Nested types, we will collect them to single Array of Tuple.
        auto names_and_types = Nested::collect(block.getNamesAndTypesList());
        block.clear();

        for (auto & column : names_and_types)
            block.insert(ColumnWithTypeAndName(column.type, column.name));
        return block;
    }
};

}

IMPLEMENT_SETTINGS_TRAITS(StorageRandomSettingsTraits, LIST_OF_STORAGE_RANDOM_SETTINGS)

void StorageRandomSettings::loadFromQuery(ASTStorage & storage_def)
{
    if (storage_def.settings)
    {
        try
        {
            applyChanges(storage_def.settings->changes);
        }
        catch (Exception & e)
        {
            if (e.code() == ErrorCodes::UNKNOWN_SETTING)
                e.addMessage("for storage " + storage_def.engine->name);
            throw;
        }
    }
    else
    {
        auto settings_ast = std::make_shared<ASTSetQuery>();
        settings_ast->is_standalone = false;
        storage_def.set(storage_def.settings, settings_ast);
    }
}


StorageRandom::StorageRandom(
    const StorageID & table_id_,
    const ColumnsDescription & columns_,
    const String & comment,
    std::optional<UInt64> random_seed_,
    UInt64 events_per_second_)
    : IStorage(table_id_), events_per_second(events_per_second_)
{
    random_seed = random_seed_ ? sipHash64(*random_seed_) : randomSeed();
    StorageInMemoryMetadata storage_metadata;
    storage_metadata.setColumns(columns_);
    storage_metadata.setComment(comment);
    setInMemoryMetadata(storage_metadata);
}


void registerStorageRandom(StorageFactory & factory)
{
    auto creator_fn = [](const StorageFactory::Arguments & args) {
        ASTs & engine_args = args.engine_args;

        if (engine_args.size() > 3)
            throw Exception(
                "Storage Random requires at most three arguments: "
                "random_seed, max_string_length, max_array_length.",
                ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

        std::optional<UInt64> random_seed;
        if (!engine_args.empty())
        {
            const Field & value = engine_args[0]->as<const ASTLiteral &>().value;
            if (!value.isNull())
                random_seed = value.safeGet<UInt64>();
        }

        auto storage_random_settings = std::make_unique<StorageRandomSettings>();
        if (args.storage_def->settings)
        {
            storage_random_settings->loadFromQuery(*args.storage_def);
        }

        return StorageRandom::create(
            args.table_id, args.columns, args.comment, random_seed, storage_random_settings->eps.value);
    };

    factory.registerStorage(
        "Random",
        creator_fn,
        StorageFactory::StorageFeatures{
            .supports_settings = true,
        });
}

void StorageRandom::read(
    QueryPlan & query_plan,
    const Names & column_names,
    const StorageSnapshotPtr & storage_snapshot,
    SelectQueryInfo & query_info,
    ContextPtr context_,
    QueryProcessingStage::Enum processed_stage,
    size_t max_block_size,
    size_t num_streams)
{
    Pipe pipe = read(column_names, storage_snapshot, query_info, context_, processed_stage, max_block_size, num_streams);

    auto read_step = std::make_unique<ReadFromStorageStep>(std::move(pipe), getName(), query_info.storage_limits);
    query_plan.addStep(std::move(read_step));
}

Pipe StorageRandom::read(
    const Names & column_names,
    const StorageSnapshotPtr & storage_snapshot,
    SelectQueryInfo & /*query_info*/,
    ContextPtr context,
    QueryProcessingStage::Enum /*processed_stage*/,
    size_t max_block_size,
    size_t num_streams)
{
    storage_snapshot->check(column_names);

    Pipes pipes;
    pipes.reserve(num_streams);

    Block block_header;
    const ColumnsDescription & our_columns = storage_snapshot->metadata->getColumns();
    for (const auto & name : column_names)
    {
        const auto & name_type = our_columns.get(name);
        MutableColumnPtr column = name_type.type->createColumn();
        block_header.insert({std::move(column), name_type.type, name_type.name});
    }
    /// Will create more seed values for each source from initial seed.
    pcg64 generate(random_seed);
    
    if (events_per_second < num_streams)
    {
        /// number of datas generated per second is less than the number of thread;
        for (size_t i = 0; i < events_per_second; i++) {
            pipes.emplace_back(
                std::make_shared<GenerateRandomSource>(max_block_size, generate(), block_header, our_columns, context, 1));
        }
        
    }
    else
    {   
        /// the number of datas that each thread should generate
        size_t count_per_thread = events_per_second / num_streams;
        size_t remainder = events_per_second % num_streams;
        /// number of datas generated per second is bigger than the number of thread;
        for (size_t i = 0; i < num_streams - 1; i++) {
            pipes.emplace_back(
                std::make_shared<GenerateRandomSource>(max_block_size, generate(), block_header, our_columns, context, count_per_thread));
        }
        /// The last thread will do the remaining work
        pipes.emplace_back(
            std::make_shared<GenerateRandomSource>(max_block_size, generate(), block_header, our_columns, context, count_per_thread + remainder));

    }
    return Pipe::unitePipes(std::move(pipes));
}

}
