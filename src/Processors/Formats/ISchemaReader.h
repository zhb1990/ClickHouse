#pragma once

#include <Core/NamesAndTypes.h>
#include <DataTypes/IDataType.h>
#include <Formats/FormatSettings.h>
#include <IO/ReadBuffer.h>
#include <Interpreters/Context.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int TYPE_MISMATCH;
}

/// Base class for schema inference for the data in some specific format.
/// It reads some data from read buffer and try to determine the schema
/// from read data.
class ISchemaReader
{
public:
    explicit ISchemaReader(ReadBuffer & in_) : in(in_) {}

    virtual NamesAndTypesList readSchema() = 0;

    /// True if order of columns is important in format.
    /// Exceptions: JSON, TSKV.
    virtual bool hasStrictOrderOfColumns() const { return true; }

    virtual bool needContext() const { return false; }
    virtual void setContext(ContextPtr &) {}

    virtual void setMaxRowsToRead(size_t) {}
    virtual size_t getNumRowsRead() const { return 0; }

    virtual ~ISchemaReader() = default;

protected:
    ReadBuffer & in;
};

using CommonDataTypeChecker = std::function<DataTypePtr(const DataTypePtr &, const DataTypePtr &)>;

class IIRowSchemaReader : public ISchemaReader
{
public:
    IIRowSchemaReader(ReadBuffer & in_, const FormatSettings & format_settings_, DataTypePtr default_type_ = nullptr);

    bool needContext() const override { return !hints_str.empty(); }
    void setContext(ContextPtr & context) override;

    virtual void transformTypesIfNeeded(DataTypePtr & type, DataTypePtr & new_type);

protected:
    void setMaxRowsToRead(size_t max_rows) override { max_rows_to_read = max_rows; }
    size_t getNumRowsRead() const override { return rows_read; }

    virtual void transformFinalTypeIfNeeded(DataTypePtr &) {}

    size_t max_rows_to_read;
    size_t rows_read = 0;
    DataTypePtr default_type;
    String hints_str;
    FormatSettings format_settings;
    std::unordered_map<String, DataTypePtr> hints;
};

/// Base class for schema inference for formats that read data row by row.
/// It reads data row by row (up to max_rows_to_read), determines types of columns
/// for each row and compare them with types from the previous rows. If some column
/// contains values with different types in different rows, the default type
/// (from argument default_type_) will be used for this column or the exception
/// will be thrown (if default type is not set). If different columns have different
/// default types, you can provide them by default_types_ argument.
class IRowSchemaReader : public IIRowSchemaReader
{
public:
    IRowSchemaReader(ReadBuffer & in_, const FormatSettings & format_settings_);
    IRowSchemaReader(ReadBuffer & in_, const FormatSettings & format_settings_, DataTypePtr default_type_);
    IRowSchemaReader(ReadBuffer & in_, const FormatSettings & format_settings_, const DataTypes & default_types_);

    NamesAndTypesList readSchema() override;

protected:
    /// Read one row and determine types of columns in it.
    /// Return types in the same order in which the values were in the row.
    /// If it's impossible to determine the type for some column, return nullptr for it.
    /// Return empty list if can't read more data.
    virtual DataTypes readRowAndGetDataTypes() = 0;

    void setColumnNames(const std::vector<String> & names) { column_names = names; }

    size_t field_index;

private:
    DataTypePtr getDefaultType(size_t column) const;
    void initColumnNames(const String & column_names_str);

    DataTypes default_types;
    std::vector<String> column_names;
};

/// Base class for schema inference for formats that read data row by row and each
/// row contains column names and values (ex: JSONEachRow, TSKV).
/// Differ from IRowSchemaReader in that after reading a row we get
/// a map {column_name : type} and some columns may be missed in a single row
/// (in this case we will use types from the previous rows for missed columns).
class IRowWithNamesSchemaReader : public IIRowSchemaReader
{
public:
    IRowWithNamesSchemaReader(ReadBuffer & in_, const FormatSettings & format_settings_, DataTypePtr default_type_ = nullptr);
    NamesAndTypesList readSchema() override;
    bool hasStrictOrderOfColumns() const override { return false; }

protected:
    /// Read one row and determine types of columns in it.
    /// Return list with names and types.
    /// If it's impossible to determine the type for some column, return nullptr for it.
    /// Set eof = true if can't read more data.
    virtual NamesAndTypesList readRowAndGetNamesAndDataTypes(bool & eof) = 0;
};

/// Base class for schema inference for formats that don't need any data to
/// determine the schema: formats with constant schema (ex: JSONAsString, LineAsString)
/// and formats that use external format schema (ex: Protobuf, CapnProto).
class IExternalSchemaReader
{
public:
    virtual NamesAndTypesList readSchema() = 0;

    virtual ~IExternalSchemaReader() = default;
};

template <class SchemaReader>
void chooseResultColumnType(
    SchemaReader & schema_reader,
    DataTypePtr & type,
    DataTypePtr & new_type,
    const DataTypePtr & default_type,
    const String & column_name,
    size_t row)
{
    if (!type)
    {
        type = new_type;
        return;
    }

    if (!new_type || type->equals(*new_type))
        return;

    schema_reader.transformTypesIfNeeded(type, new_type);
    if (type->equals(*new_type))
        return;

    /// If the new type and the previous type for this column are different,
    /// we will use default type if we have it or throw an exception.
    if (default_type)
        type = default_type;
    else
    {
        throw Exception(
            ErrorCodes::TYPE_MISMATCH,
            "Automatically defined type {} for column '{}' in row {} differs from type defined by previous rows: {}. "
            "You can specify the type for this column using setting schema_inference_hints",
            type->getName(),
            column_name,
            row,
            new_type->getName());
    }
}

void checkResultColumnTypeAndAppend(
    NamesAndTypesList & result, DataTypePtr & type, const String & name, const FormatSettings & settings, const DataTypePtr & default_type, size_t rows_read);

Strings splitColumnNames(const String & column_names_str);

}
