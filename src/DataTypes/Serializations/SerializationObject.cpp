#include <DataTypes/Serializations/SerializationObject.h>
#include <DataTypes/Serializations/SerializationObjectTypedPath.h>

#include <Columns/ColumnObject.h>
#include <DataTypes/DataTypeObject.h>
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypeTuple.h>
#include <DataTypes/DataTypeString.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int INCORRECT_DATA;
    extern const int LOGICAL_ERROR;
}

SerializationObject::SerializationObject(
    std::unordered_map<String, SerializationPtr> typed_path_serializations_,
    const std::unordered_set<String> & paths_to_skip_,
    const std::vector<String> & path_regexps_to_skip_)
    : typed_path_serializations(std::move(typed_path_serializations_))
    , paths_to_skip(paths_to_skip_)
    , dynamic_serialization(std::make_shared<SerializationDynamic>())
    , shared_data_serialization(getTypeOfSharedData()->getDefaultSerialization())
{
    /// We will need sorted order of typed paths to serialize them in order for consistency.
    sorted_typed_paths.reserve(typed_path_serializations.size());
    for (const auto & [path, _] : typed_path_serializations)
        sorted_typed_paths.emplace_back(path);
    std::sort(sorted_typed_paths.begin(), sorted_typed_paths.end());
    sorted_paths_to_skip.assign(paths_to_skip.begin(), paths_to_skip.end());
    std::sort(sorted_paths_to_skip.begin(), sorted_paths_to_skip.end());
    for (const auto & regexp_str : path_regexps_to_skip_)
        path_regexps_to_skip.emplace_back(regexp_str);
}

const DataTypePtr & SerializationObject::getTypeOfSharedData()
{
    /// Array(Tuple(String, String))
    static const DataTypePtr type = std::make_shared<DataTypeArray>(std::make_shared<DataTypeTuple>(DataTypes{std::make_shared<DataTypeString>(), std::make_shared<DataTypeString>()}, Names{"paths", "values"}));
    return type;
}

bool SerializationObject::shouldSkipPath(const String & path) const
{
    if (paths_to_skip.contains(path))
        return true;

    auto it = std::lower_bound(sorted_typed_paths.begin(), sorted_typed_paths.end(), path);
    if (it != sorted_paths_to_skip.end() && it != sorted_paths_to_skip.begin() && path.starts_with(*std::prev(it)))
        return true;

    for (const auto & regexp : path_regexps_to_skip)
    {
        if (re2::RE2::FullMatch(path, regexp))
            return true;
    }

    return false;
}

SerializationObject::ObjectSerializationVersion::ObjectSerializationVersion(UInt64 version) : value(static_cast<Value>(version))
{
    checkVersion(version);
}

void SerializationObject::ObjectSerializationVersion::checkVersion(UInt64 version)
{
    if (version != BASIC)
        throw Exception(ErrorCodes::INCORRECT_DATA, "Invalid version for Object structure serialization.");
}

struct SerializeBinaryBulkStateObject: public ISerialization::SerializeBinaryBulkState
{
    SerializationObject::ObjectSerializationVersion serialization_version;
    std::vector<String> sorted_dynamic_paths;
    std::unordered_map<String, ISerialization::SerializeBinaryBulkStatePtr> typed_path_states;
    std::unordered_map<String, ISerialization::SerializeBinaryBulkStatePtr> dynamic_path_states;
    ISerialization::SerializeBinaryBulkStatePtr shared_data_state;
    /// Paths statistics. Map (dynamic path) -> (number of non-null values in this path).
    ColumnObject::Statistics statistics = { .source = ColumnObject::Statistics::Source::READ, .data = {} };

    explicit SerializeBinaryBulkStateObject(UInt64 serialization_version_) : serialization_version(serialization_version_) {}
};

struct DeserializeBinaryBulkStateObject : public ISerialization::DeserializeBinaryBulkState
{
    std::unordered_map<String, ISerialization::DeserializeBinaryBulkStatePtr> typed_path_states;
    std::unordered_map<String, ISerialization::DeserializeBinaryBulkStatePtr> dynamic_path_states;
    ISerialization::DeserializeBinaryBulkStatePtr shared_data_state;
    ISerialization::DeserializeBinaryBulkStatePtr structure_state;
};

void SerializationObject::enumerateStreams(EnumerateStreamsSettings & settings, const StreamCallback & callback, const SubstreamData & data) const
{
    settings.path.push_back(Substream::ObjectStructure);
    callback(settings.path);
    settings.path.pop_back();

    const auto * column_object = data.column ? &assert_cast<const ColumnObject &>(*data.column) : nullptr;
    const auto * type_object = data.type ? &assert_cast<const DataTypeObject &>(*data.type) : nullptr;
    const auto * deserialize_state = data.deserialize_state ? checkAndGetState<DeserializeBinaryBulkStateObject>(data.deserialize_state) : nullptr;
    const auto * structure_state = deserialize_state ? checkAndGetState<DeserializeBinaryBulkStateObjectStructure>(deserialize_state->structure_state) : nullptr;
    settings.path.push_back(Substream::ObjectData);

    /// First, iterate over typed paths in sorted order, we will always serialize them.
    for (const auto & path : sorted_typed_paths)
    {
        settings.path.back().creator = std::make_shared<TypedPathSubcolumnCreator>(path);
        settings.path.push_back(Substream::ObjectTypedPath);
        settings.path.back().object_path_name = path;
        const auto & serialization = typed_path_serializations.at(path);
        auto path_data = SubstreamData(serialization)
                                .withType(type_object ? type_object->getTypedPaths().at(path) : nullptr)
                                .withColumn(column_object ? column_object->getTypedPaths().at(path) : nullptr)
                                .withSerializationInfo(data.serialization_info)
                                .withDeserializeState(deserialize_state ? deserialize_state->typed_path_states.at(path) : nullptr);
        settings.path.back().data = path_data;
        serialization->enumerateStreams(settings, callback, path_data);
        settings.path.pop_back();
        settings.path.back().creator.reset();
    }

    /// If column or deserialization state was provided, iterate over dynamic paths,
    if (column_object || structure_state)
    {
        /// Enumerate dynamic paths in sorted order for consistency.
        const auto * dynamic_paths = column_object ? &column_object->getDynamicPaths() : nullptr;
        std::vector<String> sorted_dynamic_paths;
        /// If we have deserialize_state we can take sorted dynamic paths list from it.
        if (structure_state)
        {
            sorted_dynamic_paths = structure_state->sorted_dynamic_paths;
        }
        else
        {
            sorted_dynamic_paths.reserve(dynamic_paths->size());
            for (const auto & [path, _] : *dynamic_paths)
                sorted_dynamic_paths.push_back(path);
            std::sort(sorted_dynamic_paths.begin(), sorted_dynamic_paths.end());
        }

        DataTypePtr dynamic_type = std::make_shared<DataTypeDynamic>();
        for (const auto & path : sorted_dynamic_paths)
        {
            settings.path.push_back(Substream::ObjectDynamicPath);
            settings.path.back().object_path_name = path;
            auto path_data = SubstreamData(dynamic_serialization)
                                 .withType(dynamic_type)
                                 .withColumn(dynamic_paths ? dynamic_paths->at(path) : nullptr)
                                 .withSerializationInfo(data.serialization_info)
                                 .withDeserializeState(deserialize_state ? deserialize_state->dynamic_path_states.at(path) : nullptr);
            settings.path.back().data = path_data;
            dynamic_serialization->enumerateStreams(settings, callback, path_data);
            settings.path.pop_back();
        }
    }

    settings.path.push_back(Substream::ObjectSharedData);
    auto shared_data_substream_data = SubstreamData(shared_data_serialization)
                                          .withType(getTypeOfSharedData())
                                          .withColumn(column_object ? column_object->getSharedDataPtr() : nullptr)
                                          .withSerializationInfo(data.serialization_info)
                                          .withDeserializeState(deserialize_state ? deserialize_state->shared_data_state : nullptr);
    shared_data_serialization->enumerateStreams(settings, callback, shared_data_substream_data);
    settings.path.pop_back();
    settings.path.pop_back();
}

void SerializationObject::serializeBinaryBulkStatePrefix(
    const IColumn & column,
    SerializeBinaryBulkSettings & settings,
    SerializeBinaryBulkStatePtr & state) const
{
    const auto & column_object = assert_cast<const ColumnObject &>(column);
    const auto & typed_paths = column_object.getTypedPaths();
    const auto & dynamic_paths = column_object.getDynamicPaths();
    const auto & shared_data = column_object.getSharedDataPtr();

    settings.path.push_back(Substream::ObjectStructure);
    auto * stream = settings.getter(settings.path);
    settings.path.pop_back();

    if (!stream)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Missing stream for Object column structure during serialization of binary bulk state prefix");

    /// Write serialization version.
    UInt64 serialization_version = ObjectSerializationVersion::Value::BASIC;
    writeBinaryLittleEndian(serialization_version, *stream);

    /// Write all dynamic paths in sorted order.
    auto object_state = std::make_shared<SerializeBinaryBulkStateObject>(serialization_version);
    object_state->sorted_dynamic_paths.reserve(dynamic_paths.size());
    for (const auto & [path, _] : dynamic_paths)
        object_state->sorted_dynamic_paths.push_back(path);
    std::sort(object_state->sorted_dynamic_paths.begin(), object_state->sorted_dynamic_paths.end());
    writeVarUInt(object_state->sorted_dynamic_paths.size(), *stream);
    for (const auto & path : object_state->sorted_dynamic_paths)
        writeStringBinary(path, *stream);

    /// Write statistics in prefix if needed.
    if (settings.object_and_dynamic_write_statistics == SerializeBinaryBulkSettings::ObjectAndDynamicStatisticsMode::PREFIX)
    {
        const auto & statistics = column_object.getStatistics();
        for (const auto & path : object_state->sorted_dynamic_paths)
        {
            size_t number_of_non_null_values = 0;
            /// Check if we can use statistics stored in the column. There are 2 possible sources
            /// of this statistics:
            ///   - statistics calculated during merge of some data parts (Statistics::Source::MERGE)
            ///   - statistics read from the data part during deserialization of Object column (Statistics::Source::READ).
            /// We can rely only on statistics calculated during the merge, because column with statistics that was read
            /// during deserialization from some data part could be filtered/limited/transformed/etc and so the statistics can be outdated.
            if (!statistics.data.empty() && statistics.source == ColumnObject::Statistics::Source::MERGE)
                number_of_non_null_values = statistics.data.at(path);
            /// Otherwise we can use only path column from current object column.
            else
                number_of_non_null_values = (dynamic_paths.at(path)->size() - dynamic_paths.at(path)->getNumberOfDefaultRows());
            writeVarUInt(number_of_non_null_values, *stream);
        }
    }

    settings.path.push_back(Substream::ObjectData);

    for (const auto & path : sorted_typed_paths)
    {
        settings.path.push_back(Substream::ObjectTypedPath);
        settings.path.back().object_path_name = path;
        typed_path_serializations.at(path)->serializeBinaryBulkStatePrefix(*typed_paths.at(path), settings, object_state->typed_path_states[path]);
        settings.path.pop_back();
    }

    for (const auto & path : object_state->sorted_dynamic_paths)
    {
        settings.path.push_back(Substream::ObjectDynamicPath);
        settings.path.back().object_path_name = path;
        dynamic_serialization->serializeBinaryBulkStatePrefix(*dynamic_paths.at(path), settings, object_state->dynamic_path_states[path]);
        settings.path.pop_back();
    }

    settings.path.push_back(Substream::ObjectSharedData);
    shared_data_serialization->serializeBinaryBulkStatePrefix(*shared_data, settings, object_state->shared_data_state);
    settings.path.pop_back();
    settings.path.pop_back();

    state = std::move(object_state);
}

void SerializationObject::deserializeBinaryBulkStatePrefix(
    DeserializeBinaryBulkSettings & settings,
    DeserializeBinaryBulkStatePtr & state,
    SubstreamsDeserializeStatesCache * cache) const
{
    auto structure_state = deserializeObjectStructureStatePrefix(settings, cache);
    if (!structure_state)
        return;

    auto object_state = std::make_shared<DeserializeBinaryBulkStateObject>();
    object_state->structure_state = std::move(structure_state);

    settings.path.push_back(Substream::ObjectData);

    for (const auto & path : sorted_typed_paths)
    {
        settings.path.push_back(Substream::ObjectTypedPath);
        settings.path.back().object_path_name = path;
        typed_path_serializations.at(path)->deserializeBinaryBulkStatePrefix(settings, object_state->typed_path_states[path], cache);
        settings.path.pop_back();
    }

    const auto & sorted_dynamic_paths = checkAndGetState<DeserializeBinaryBulkStateObjectStructure>(object_state->structure_state)->sorted_dynamic_paths;
    for (const auto & path : sorted_dynamic_paths)
    {
        settings.path.push_back(Substream::ObjectDynamicPath);
        settings.path.back().object_path_name = path;
        dynamic_serialization->deserializeBinaryBulkStatePrefix(settings, object_state->dynamic_path_states[path], cache);
        settings.path.pop_back();
    }

    settings.path.push_back(Substream::ObjectSharedData);
    shared_data_serialization->deserializeBinaryBulkStatePrefix(settings, object_state->shared_data_state, cache);
    settings.path.pop_back();
    settings.path.pop_back();

    state = std::move(object_state);
}

ISerialization::DeserializeBinaryBulkStatePtr SerializationObject::deserializeObjectStructureStatePrefix(
    DeserializeBinaryBulkSettings & settings, SubstreamsDeserializeStatesCache * cache)
{
    settings.path.push_back(Substream::ObjectStructure);

    DeserializeBinaryBulkStatePtr state = nullptr;
    /// Check if we already deserialized this state. It can happen when we read both object column and its subcolumns.
    if (auto cached_state = getFromSubstreamsDeserializeStatesCache(cache, settings.path))
    {
        state = cached_state;
    }
    else if (auto * structure_stream = settings.getter(settings.path))
    {
        /// Read structure serialization version.
        UInt64 serialization_version;
        readBinaryLittleEndian(serialization_version, *structure_stream);
        auto structure_state = std::make_shared<DeserializeBinaryBulkStateObjectStructure>(serialization_version);
        /// Read the sorted list of dynamic paths.
        size_t dynamic_paths_size;
        readVarUInt(dynamic_paths_size, *structure_stream);
        structure_state->sorted_dynamic_paths.reserve(dynamic_paths_size);
        structure_state->dynamic_paths.reserve(dynamic_paths_size);
        for (size_t i = 0; i != dynamic_paths_size; ++i)
        {
            structure_state->sorted_dynamic_paths.emplace_back();
            readStringBinary(structure_state->sorted_dynamic_paths.back(), *structure_stream);
            structure_state->dynamic_paths.insert(structure_state->sorted_dynamic_paths.back());
        }

        /// Read statistics if needed.
        if (settings.object_and_dynamic_read_statistics)
        {
            for (const auto & path : structure_state->sorted_dynamic_paths)
                readVarUInt(structure_state->statistics.data[path], *structure_stream);
        }

        state = std::move(structure_state);
        addToSubstreamsDeserializeStatesCache(cache, settings.path, state);
    }

    settings.path.pop_back();
    return state;
}

void SerializationObject::serializeBinaryBulkWithMultipleStreams(
    const IColumn & column,
    size_t offset,
    size_t limit,
    SerializeBinaryBulkSettings & settings,
    SerializeBinaryBulkStatePtr & state) const
{
    const auto & column_object = assert_cast<const ColumnObject &>(column);
    const auto & typed_paths = column_object.getTypedPaths();
    const auto & dynamic_paths = column_object.getDynamicPaths();
    const auto & shared_data = column_object.getSharedDataPtr();
    auto * object_state = checkAndGetState<SerializeBinaryBulkStateObject>(state);

    settings.path.push_back(Substream::ObjectData);

    for (const auto & path : sorted_typed_paths)
    {
        settings.path.push_back(Substream::ObjectTypedPath);
        settings.path.back().object_path_name = path;
        typed_path_serializations.at(path)->serializeBinaryBulkWithMultipleStreams(*typed_paths.at(path), offset, limit, settings, object_state->typed_path_states[path]);
        settings.path.pop_back();
    }

    const auto * dynamic_serialization_typed = assert_cast<const SerializationDynamic *>(dynamic_serialization.get());
    for (const auto & path : object_state->sorted_dynamic_paths)
    {
        settings.path.push_back(Substream::ObjectDynamicPath);
        settings.path.back().object_path_name = path;
        auto it = dynamic_paths.find(path);
        if (it == dynamic_paths.end())
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Dynamic structure mismatch for Object column: dynamic path '{}' is not found in the column", path);
        size_t number_of_non_null_values = 0;
        dynamic_serialization_typed->serializeBinaryBulkWithMultipleStreamsAndCountTotalSizeOfVariants(*it->second, offset, limit, settings, object_state->dynamic_path_states[path], number_of_non_null_values);
        object_state->statistics.data[path] += number_of_non_null_values;
        settings.path.pop_back();
    }

    settings.path.push_back(Substream::ObjectSharedData);
    shared_data_serialization->serializeBinaryBulkWithMultipleStreams(*shared_data, offset, limit, settings, object_state->shared_data_state);
    settings.path.pop_back();
    settings.path.pop_back();
}

void SerializationObject::serializeBinaryBulkStateSuffix(
    SerializeBinaryBulkSettings & settings, SerializeBinaryBulkStatePtr & state) const
{
    auto * object_state = checkAndGetState<SerializeBinaryBulkStateObject>(state);
    settings.path.push_back(Substream::ObjectStructure);
    auto * stream = settings.getter(settings.path);
    settings.path.pop_back();

    if (!stream)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Missing stream for Object column structure during serialization of binary bulk state suffix");

    /// Write statistics in suffix if needed.
    if (settings.object_and_dynamic_write_statistics == SerializeBinaryBulkSettings::ObjectAndDynamicStatisticsMode::SUFFIX)
    {
        for (const auto & path : object_state->sorted_dynamic_paths)
            writeVarUInt(object_state->statistics.data[path], *stream);
    }

    settings.path.push_back(Substream::ObjectData);

    for (const auto & path : sorted_typed_paths)
    {
        settings.path.push_back(Substream::ObjectTypedPath);
        settings.path.back().object_path_name = path;
        typed_path_serializations.at(path)->serializeBinaryBulkStateSuffix(settings, object_state->typed_path_states[path]);
        settings.path.pop_back();
    }

    for (const auto & path : object_state->sorted_dynamic_paths)
    {
        settings.path.push_back(Substream::ObjectDynamicPath);
        settings.path.back().object_path_name = path;
        dynamic_serialization->serializeBinaryBulkStateSuffix(settings, object_state->dynamic_path_states[path]);
        settings.path.pop_back();
    }

    settings.path.push_back(Substream::ObjectSharedData);
    shared_data_serialization->serializeBinaryBulkStateSuffix(settings, object_state->shared_data_state);
    settings.path.pop_back();
    settings.path.pop_back();
}

void SerializationObject::deserializeBinaryBulkWithMultipleStreams(
    ColumnPtr & column,
    size_t limit,
    DeserializeBinaryBulkSettings & settings,
    DeserializeBinaryBulkStatePtr & state,
    SubstreamsCache * cache) const
{
    auto * object_state = checkAndGetState<DeserializeBinaryBulkStateObject>(state);
    auto * structure_state = checkAndGetState<DeserializeBinaryBulkStateObjectStructure>(object_state->structure_state);
    auto mutable_column = column->assumeMutable();
    auto & column_object = assert_cast<ColumnObject &>(*mutable_column);
    /// If it's a new object column, set dynamic paths and statistics.
    if (column_object.empty())
    {
        column_object.setDynamicPaths(structure_state->sorted_dynamic_paths);
        column_object.setStatistics(structure_state->statistics);
    }

    auto & typed_paths = column_object.getTypedPaths();
    auto & dynamic_paths = column_object.getDynamicPaths();
    auto & shared_data = column_object.getSharedDataPtr();

    settings.path.push_back(Substream::ObjectData);
    for (const auto & path : sorted_typed_paths)
    {
        settings.path.push_back(Substream::ObjectTypedPath);
        settings.path.back().object_path_name = path;
        typed_path_serializations.at(path)->deserializeBinaryBulkWithMultipleStreams(typed_paths[path], limit, settings, object_state->typed_path_states[path], cache);
        settings.path.pop_back();
    }

    for (const auto & path : structure_state->sorted_dynamic_paths)
    {
        settings.path.push_back(Substream::ObjectDynamicPath);
        settings.path.back().object_path_name = path;
        dynamic_serialization->deserializeBinaryBulkWithMultipleStreams(dynamic_paths[path], limit, settings, object_state->dynamic_path_states[path], cache);
        settings.path.pop_back();
    }

    settings.path.push_back(Substream::ObjectSharedData);
    shared_data_serialization->deserializeBinaryBulkWithMultipleStreams(shared_data, limit, settings, object_state->shared_data_state, cache);
    settings.path.pop_back();
    settings.path.pop_back();
}

void SerializationObject::serializeBinary(const Field & field, WriteBuffer & ostr, const DB::FormatSettings & settings) const
{
    auto & object = field.get<Object>();
    /// Serialize number of paths and then pairs (path, value).
    writeVarUInt(object.size(), ostr);
    for (const auto & [path, value] : object)
    {
        writeStringBinary(path, ostr);
        if (auto it = typed_path_serializations.find(path); it != typed_path_serializations.end())
            it->second->serializeBinary(value, ostr, settings);
        else
            dynamic_serialization->serializeBinary(value, ostr, settings);
    }
}

void SerializationObject::serializeBinary(const IColumn & col, size_t row_num, WriteBuffer & ostr, const FormatSettings & settings) const
{
    const auto & column_object = assert_cast<const ColumnObject &>(col);
    const auto & typed_paths = column_object.getTypedPaths();
    const auto & dynamic_paths = column_object.getDynamicPaths();
    const auto & shared_data_offsets = column_object.getSharedDataOffsets();
    size_t offset = shared_data_offsets[ssize_t(row_num) - 1];
    size_t end = shared_data_offsets[ssize_t(row_num)];

    /// Serialize number of paths and then pairs (path, value).
    writeVarUInt(typed_paths.size() + dynamic_paths.size() + (end - offset), ostr);

    for (const auto & [path, column] : typed_paths)
    {
        writeStringBinary(path, ostr);
        typed_path_serializations.at(path)->serializeBinary(*column, row_num, ostr, settings);
    }

    for (const auto & [path, column] : dynamic_paths)
    {
        writeStringBinary(path, ostr);
        dynamic_serialization->serializeBinary(*column, row_num, ostr, settings);
    }

    const auto [shared_data_paths, shared_data_values] = column_object.getSharedDataPathsAndValues();
    for (size_t i = offset; i != end; ++i)
    {
        writeStringBinary(shared_data_paths->getDataAt(i), ostr);
        auto value = shared_data_values->getDataAt(i);
        ostr.write(value.data, value.size);
    }
}

void SerializationObject::deserializeBinary(Field & field, ReadBuffer & istr, const FormatSettings & settings) const
{
    Object object;
    size_t number_of_paths;
    readVarUInt(number_of_paths, istr);
    /// Read pairs (path, value).
    for (size_t i = 0; i != number_of_paths; ++i)
    {
        String path;
        readStringBinary(path, istr);
        if (!shouldSkipPath(path))
        {
            if (auto it = typed_path_serializations.find(path); it != typed_path_serializations.end())
                it->second->deserializeBinary(object[path], istr, settings);
            else
                dynamic_serialization->deserializeBinary(object[path], istr, settings);
        }
        else
        {
            /// Skip value of this path.
            Field tmp;
            dynamic_serialization->deserializeBinary(tmp, istr, settings);
        }
    }

    field = std::move(object);
}

/// Restore column object to the state with previous size.
/// We can use it in case of an exception during deserialization.
void SerializationObject::restoreColumnObject(ColumnObject & column_object, size_t prev_size)
{
    auto & typed_paths = column_object.getTypedPaths();
    auto & dynamic_paths = column_object.getDynamicPaths();
    auto [shared_data_paths, shared_data_values] = column_object.getSharedDataPathsAndValues();
    auto & shared_data_offsets = column_object.getSharedDataOffsets();

    for (auto & [_, column] : typed_paths)
    {
        if (column->size() > prev_size)
            column->popBack(column->size() - prev_size);
    }

    for (auto & [_, column] : dynamic_paths)
    {
        if (column->size() > prev_size)
            column->popBack(column->size() - prev_size);
    }

    if (shared_data_offsets.size() > prev_size)
        shared_data_offsets.resize(prev_size);
    size_t prev_shared_data_offset = shared_data_offsets.back();
    if (shared_data_paths->size() > prev_shared_data_offset)
        shared_data_paths->popBack(shared_data_paths->size() - prev_shared_data_offset);
    if (shared_data_values->size() > prev_shared_data_offset)
        shared_data_values->popBack(shared_data_values->size() - prev_shared_data_offset);
}

void SerializationObject::deserializeBinary(IColumn & col, ReadBuffer & istr, const FormatSettings & settings) const
{
    auto & column_object = assert_cast<ColumnObject &>(col);
    auto & typed_paths = column_object.getTypedPaths();
    auto & dynamic_paths = column_object.getDynamicPaths();
    auto [shared_data_paths, shared_data_values] = column_object.getSharedDataPathsAndValues();
    auto & shared_data_offsets = column_object.getSharedDataOffsets();

    size_t number_of_paths;
    readVarUInt(number_of_paths, istr);
    std::vector<std::pair<String, String>> paths_and_values_for_shared_data;
    size_t prev_size = column_object.size();
    try
    {
        /// Read pairs (path, value).
        for (size_t i = 0; i != number_of_paths; ++i)
        {
            String path;
            readStringBinary(path, istr);
            if (!shouldSkipPath(path))
            {
                /// Check if we have this path in typed paths.
                if (auto typed_it = typed_path_serializations.find(path); typed_it != typed_path_serializations.end())
                {
                    auto & typed_column = typed_paths[path];
                    /// Check if we already had this path.
                    if (typed_column->size() > prev_size)
                    {
                        if (!settings.json.type_json_skip_duplicated_paths)
                            throw Exception(ErrorCodes::INCORRECT_DATA, "Found duplicated path during binary deserialization of Object type: {}", path);
                    }
                    else
                    {
                        typed_it->second->deserializeBinary(*typed_column, istr, settings);
                    }
                }
                /// Check if we have this path in dynamic paths.
                else if (auto dynamic_it = dynamic_paths.find(path); dynamic_it != dynamic_paths.end())
                {
                    /// Check if we already had this path.
                    if (dynamic_it->second->size() > prev_size)
                    {
                        if (!settings.json.type_json_skip_duplicated_paths)
                            throw Exception(ErrorCodes::INCORRECT_DATA, "Found duplicated path during binary deserialization of Object type: {}", path);
                    }

                    dynamic_serialization->deserializeBinary(*dynamic_it->second, istr, settings);
                }
                /// Try to add a new dynamic paths.
                else if (auto * dynamic_column = column_object.tryToAddNewDynamicPath(path))
                {
                    dynamic_serialization->deserializeBinary(*dynamic_column, istr, settings);
                }
                /// Otherwise this path should go to shared data.
                else
                {
                    auto tmp_dynamic_column = ColumnDynamic::create();
                    tmp_dynamic_column->reserve(1);
                    String value;
                    readParsedValueIntoString(value, istr, [&](ReadBuffer & buf){ dynamic_serialization->deserializeBinary(*tmp_dynamic_column, buf, settings); });
                    paths_and_values_for_shared_data.emplace_back(std::move(path), std::move(value));
                }
            }
            else
            {
                /// Skip value of this path.
                Field tmp;
                dynamic_serialization->deserializeBinary(tmp, istr, settings);
            }
        }

        std::sort(paths_and_values_for_shared_data.begin(), paths_and_values_for_shared_data.end());
        for (size_t i = 0; i != paths_and_values_for_shared_data.size(); ++i)
        {
            const auto & [path, value] = paths_and_values_for_shared_data[i];
            if (i != 0 && path == paths_and_values_for_shared_data[i - 1].first)
            {
                if (!settings.json.type_json_skip_duplicated_paths)
                    throw Exception(ErrorCodes::INCORRECT_DATA, "Found duplicated path during binary deserialization of Object type: {}", path);
            }
            else
            {
                shared_data_paths->insertData(path.data(), path.size());
                shared_data_values->insertData(value.data(), value.size());
            }
        }
        shared_data_offsets.push_back(shared_data_paths->size());
    }
    catch (...)
    {
        restoreColumnObject(column_object, prev_size);
        throw;
    }
}

SerializationPtr SerializationObject::TypedPathSubcolumnCreator::create(const DB::SerializationPtr & prev) const
{
    return std::make_shared<SerializationObjectTypedPath>(prev, path);
}

}

