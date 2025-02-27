#pragma once

#include <base/types.h>
#include <Common/Exception.h>

#include <magic_enum.hpp>

namespace DB
{

namespace ErrorCodes
{
extern const int INCORRECT_DATA;
}

/// Don't change ProcessorID value since they are used to persistent
/// DAG on storage, changing the ID value for a processor is breaking change
enum class ProcessorID : UInt32
{
    InvalidID = 0,
    /// Internal processor
    FilterTransformID = 1,
    ExpressionTransformID = 2,
    ResizeProcessorID = 3,
    StrictResizeProcessorID = 4,
    OffsetTransformID = 5,
    ForkProcessorID = 6,
    LimitTransformID = 7,
    ConcatProcessorID = 8,
    AddingSelectorTransformID = 9,
    AddingDefaultsTransformID = 10,
    ConvertingAggregatedToChunksTransformID = 11,
    QueueBufferID = 12,
    CreatingSetsTransformID = 13,
    CubeTransformID = 14,
    BufferingToFileTransformID = 15,
    MergingAggregatedTransformID = 16,
    RollupTransformID = 17,
    TTLCalcTransformID = 18,
    TTLTransformID = 19,
    DistinctTransformID = 20,
    DistinctSortedTransformID = 21,
    FinalizeAggregatedTransformID = 22,
    ExceptionKeepingTransformID = 23,
    FillingTransformID = 24,
    CopyTransformID = 25,
    ArrayJoinTransformID = 26,
    ExtremesTransformID = 27,
    CheckSortedTransformID = 28,
    TotalsHavingTransformID = 29,
    MergeSortingTransformID = 30,
    PartialSortingTransformID = 31,
    ReverseTransformID = 32,
    LimitByTransformID = 33,
    IntersectOrExceptTransformID = 34,
    JoiningTransformID = 35,
    MaterializingTransformID = 36,
    LimitsCheckingTransformID = 37,
    FinishSortingTransformID = 38,
    FillingRightJoinSideTransformID = 39,
    CopyingDataToViewsTransformID = 40,
    FinalizingViewsTransformID = 41,
    WindowTransformID = 42,
    WatermarkTransformID = 43,
    StreamingJoinTransformID = 44,
    DedupTransformID = 45,
    TimestampTransformID = 46,
    CheckMaterializedViewValidTransformID = 47,
    StreamingWindowTransformID = 48,
    WatermarkTransformWithSubstreamID = 49,
    SendingChunkHeaderTransformID = 50,
    AggregatingSortedTransformID = 51,
    CollapsingSortedTransformID = 52,
    FinishAggregatingInOrderTransformID = 53,
    GraphiteRollupSortedTransformID = 54,
    MergingSortedTransformID = 55,
    ReplacingSortedTransformID = 56,
    SummingSortedTransformID = 57,
    VersionedCollapsingTransformID = 58,
    ColumnGathererTransformID = 59,
    TransformWithAdditionalColumnsID = 60,
    ConvertingTransformID = 61,
    ExecutingInnerQueryFromViewTransformID = 62,
    CheckConstraintsTransformID = 63,
    CountingTransformID = 64,
    SquashingChunksTransformID = 65,
    RemoteSinkID = 66,
    BufferSinkID = 67,
    StorageFileSinkID = 68,
    MemorySinkID = 69,
    StorageS3SinkID = 70,
    StorageURLSinkID = 71,
    PartitionedStorageURLSinkID = 72,
    DistributedSinkID = 73,
    MergeTreeSinkID = 74,
    EmbeddedRocksDBSinkID = 75,
    StreamSinkID = 76,
    SetOrJoinSinkID = 77,
    NullSinkToStorageID = 78,
    PartitionedStorageFileSinkID = 79,
    ReplacingWindowColumnTransformID = 80,
    ShufflingTransformID = 81,
    MergeJoinTransformID = 82,
    FilterSortedStreamByRangeID = 83,
    DistinctSortedChunkTransformID = 84,
    TumbleWindowAssignmentTransformID = 85,
    HopWindowAssignmentTransformID = 86,
    SessionWindowAssignmentTransformID = 87,
    StreamingShrinkResizeProcessorID = 88,
    StreamingExpandResizeProcessorID = 89,
    StreamingStrictResizeProcessorID = 90,
    StreamingLimitTransformID = 91,
    StreamingOffsetTransformID = 92,
    ChangelogConvertTransformID = 93,
    ChangelogTransformID = 94,

    /// Aggregating transform
    AggregatingInOrderTransformID = 1'000,
    AggregatingTransformID = 1'001,
    GroupingAggregatedTransformID = 1'002,
    MergingAggregatedBucketTransformID = 1'003,
    SortingAggregatedTransformID = 1'004,
    GlobalAggregatingTransformID = 1'005,
    TumbleAggregatingTransformID = 1'006,
    TumbleAggregatingTransformWithSubstreamID = 1'007,
    HopAggregatingTransformID = 1'008,
    HopAggregatingTransformWithSubstreamID = 1'009,
    SessionAggregatingTransformID = 1'010,
    SessionAggregatingTransformWithSubstreamID = 1'011,
    GlobalAggregatingTransformWithSubstreamID = 1'012,
    UserDefinedEmitStrategyAggregatingTransformID = 1'013,
    UserDefinedEmitStrategyAggregatingTransformWithSubstreamID = 1'014,

    /// Format Input Processors
    ParallelParsingInputFormatID = 4'000,
    AvroConfluentRowInputFormatID = 4'001,
    AvroRowInputFormatID = 4'002,
    CapnProtoRowInputFormatID = 4'003,
    JSONAsRowInputFormatID = 4'004,
    JSONEachRowRowInputFormatID = 4'005,
    LineAsStringRowInputFormatID = 4'006,
    MsgPackRowInputFormatID = 4'007,
    ProtobufRowInputFormatID = 4'008,
    RawBLOBRowInputFormatID = 4'009,
    RawStoreInputFormatID = 4'010,
    RegexpRowInputFormatID = 4'011,
    TSKVRowInputFormatID = 4'012,
    JSONCompactRowOutputFormatID = 4'013,
    JSONColumnsBlockInputFormatBaseID = 4'014,
    JSONColumnsBlockOutputFormatID = 4'015,
    JSONColumnsWithMetadataBlockOutputFormatID = 4'016,
    ODBCDriver2BlockOutputFormatID = 4'017,
    JSONCompactEachRowRowInputFormatID = 4'018,
    CSVRowInputFormatID = 4'019,
    CustomSeparatedRowInputFormatID = 4'020,
    JSONAsObjectRowInputFormatID = 4'021,
    BinaryRowInputFormatID = 4'022,
    TemplateRowInputFormatID = 4'023,
    NativeInputFormatID = 4'024,
    TabSeparatedRowInputFormatID = 4'025,
    ValuesBlockInputFormatID = 4'026,
    ArrowBlockInputFormatID = 4'027,
    ORCBlockInputFormatID = 4'028,
    ParquetBlockInputFormatID = 4'029,

    /// Format Output Processors
    NullOutputFormatID = 5'000,
    AvroRowOutputFormatID = 5'001,
    BinaryRowOutputFormatID = 5'002,
    CapnProtoRowOutputFormatID = 5'003,
    CSVRowOutputFormatID = 5'004,
    CustomSeparatedRowOutputFormatID = 5'005,
    JSONCompactEachRowRowOutputFormatID = 5'006,
    JSONEachRowRowOutputFormatID = 5'007,
    JSONRowOutputFormatID = 5'008,
    MarkdownRowOutputFormatID = 5'009,
    MsgPackRowOutputFormatID = 5'010,
    ProtobufRowOutputFormatID = 5'011,
    RawBLOBRowOutputFormatID = 5'012,
    TabSeparatedRowOutputFormatID = 5'013,
    ValuesRowOutputFormatID = 5'014,
    VerticalRowOutputFormatID = 5'015,
    XMLRowOutputFormatID = 5'016,
    LazyOutputFormatID = 5'017,
    ParallelFormattingOutputFormatID = 5'018,
    PullingOutputFormatID = 5'019,
    JSONEachRowWithProgressRowOutputFormatID = 5'020,
    JSONColumnsBlockOutputFormatBaseID = 5'021,
    PrettyBlockOutputFormatID = 5'022,
    PostgreSQLOutputFormatID = 5'023,
    NativeOutputFormatID = 5'024,
    TSKVRowOutputFormatID = 5'025,
    TemplateBlockOutputFormatID = 5'026,
    ArrowBlockOutputFormatID = 5'027,
    ORCBlockOutputFormatID = 5'028,
    ParquetBlockOutputFormatID = 5'029,

    /// Source Processors
    NullSourceID = 10'000,
    KafkaSourceID = 10'001,
    EmbeddedRocksDBSourceID = 10'002,
    FileLogSourceID = 10'003,
    BlockListSourceID = 10'004,
    MergeSorterSourceID = 10'005,
    SyncKillQuerySourceID = 10'006,
    WaitForAsyncInsertSourceID = 10'007,
    JoinSourceID = 10'008,
    GenerateSourceID = 10'009,
    StorageInputSourceID = 10'010,
    StorageFileSourceID = 10'011,
    BufferSourceID = 10'012,
    MemorySourceID = 10'013,
    StorageURLSourceID = 10'014,
    DirectoryMonitorSourceID = 10'015,
    MergeTreeSequentialSourceID = 10'016,
    MergeTreeThreadSelectProcessorID = 10'017,
    MergeTreeSelectProcessorID = 10'018,
    DelayedPortsProcessorID = 10'019,
    PushingSourceID = 10'020,
    PushingAsyncSourceID = 10'021,
    SourceFromNativeStreamID = 10'022,
    ConvertingAggregatedToChunksSourceID = 10'023,
    StreamingSourceFromNativeStreamID = 10'024,
    StreamingConvertingAggregatedToChunksSourceID = 10'025,
    StreamingConvertingAggregatedToChunksTransformID = 10'026,
    StreamingStoreSourceID = 10'027,
    ShellCommandSourceID = 10'028,
    TemporaryFileLazySourceID = 10'029,
    SourceFromSingleChunkID = 10'030,
    DelayedSourceID = 10'031,
    StreamingStoreSourceChannelID = 10'032,
    RemoteSourceID = 10'033,
    RemoteTotalsSourceID = 10'034,
    RemoteExtremesSourceID = 10'035,
    DictionarySourceID = 10'036,
    MarkSourceID = 10'037,
    ColumnsSourceID = 10'038,
    DataSkippingIndicesSourceID = 10'039,
    NumbersMultiThreadedSourceID = 10'040,
    NumbersSourceID = 10'041,
    TablesBlockSourceID = 10'042,
    ZerosSourceID = 10'043,
    StorageS3SourceID = 10'044,
    GenerateRandomSourceID = 10'045,
    SourceFromQueryPipelineID = 10'046,

    /// Sink Processors
    EmptySinkID = 20'000,
    NullSinkID = 20'001,
    ExternalTableDataSinkID = 20'002,
};

inline ProcessorID toProcessID(UInt32 v)
{
    auto pid = magic_enum::enum_cast<ProcessorID>(v);
    if (pid)
        return *pid;

    throw Exception(ErrorCodes::INCORRECT_DATA, "Unknown processor id '{}'", v);
}
}
