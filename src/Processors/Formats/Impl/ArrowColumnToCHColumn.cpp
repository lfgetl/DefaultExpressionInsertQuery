#include "config_formats.h"
#include "ArrowColumnToCHColumn.h"

#if USE_ARROW || USE_ORC || USE_PARQUET
#include <DataTypes/DataTypeFactory.h>
#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/DataTypesDecimal.h>
#include <DataTypes/DataTypesNumber.h>
#include <DataTypes/DataTypeArray.h>
#include <common/DateLUTImpl.h>
#include <common/types.h>
#include <Core/Block.h>
#include <Columns/ColumnString.h>
#include <Columns/ColumnNullable.h>
#include <Columns/ColumnArray.h>
#include <Interpreters/castColumn.h>
#include <algorithm>
#include <DataTypes/DataTypeLowCardinality.h>


namespace DB
{
    namespace ErrorCodes
    {
        extern const int UNKNOWN_TYPE;
        extern const int VALUE_IS_OUT_OF_RANGE_OF_DATA_TYPE;
        extern const int CANNOT_CONVERT_TYPE;
        extern const int CANNOT_INSERT_NULL_IN_ORDINARY_COLUMN;
        extern const int THERE_IS_NO_COLUMN;
    }

    static const std::initializer_list<std::pair<arrow::Type::type, const char *>> arrow_type_to_internal_type =
    {
            {arrow::Type::UINT8, "UInt8"},
            {arrow::Type::INT8, "Int8"},
            {arrow::Type::UINT16, "UInt16"},
            {arrow::Type::INT16, "Int16"},
            {arrow::Type::UINT32, "UInt32"},
            {arrow::Type::INT32, "Int32"},
            {arrow::Type::UINT64, "UInt64"},
            {arrow::Type::INT64, "Int64"},
            {arrow::Type::HALF_FLOAT, "Float32"},
            {arrow::Type::FLOAT, "Float32"},
            {arrow::Type::DOUBLE, "Float64"},

            {arrow::Type::BOOL, "UInt8"},
            {arrow::Type::DATE32, "Date"},
            {arrow::Type::DATE64, "DateTime"},
            {arrow::Type::TIMESTAMP, "DateTime"},

            {arrow::Type::STRING, "String"},
            {arrow::Type::BINARY, "String"},

            // TODO: add other types that are convertible to internal ones:
            // 0. ENUM?
            // 1. UUID -> String
            // 2. JSON -> String
            // Full list of types: contrib/arrow/cpp/src/arrow/type.h
    };

/// Inserts numeric data right into internal column data to reduce an overhead
    template <typename NumericType, typename VectorType = ColumnVector<NumericType>>
    static void fillColumnWithNumericData(std::shared_ptr<arrow::ChunkedArray> & arrow_column, IColumn & internal_column)
    {
        auto & column_data = static_cast<VectorType &>(internal_column).getData();
        column_data.reserve(arrow_column->length());

        for (size_t chunk_i = 0, num_chunks = static_cast<size_t>(arrow_column->num_chunks()); chunk_i < num_chunks; ++chunk_i)
        {
            std::shared_ptr<arrow::Array> chunk = arrow_column->chunk(chunk_i);
            /// buffers[0] is a null bitmap and buffers[1] are actual values
            std::shared_ptr<arrow::Buffer> buffer = chunk->data()->buffers[1];

            const auto * raw_data = reinterpret_cast<const NumericType *>(buffer->data());
            column_data.insert_assume_reserved(raw_data, raw_data + chunk->length());
        }
    }

/// Inserts chars and offsets right into internal column data to reduce an overhead.
/// Internal offsets are shifted by one to the right in comparison with Arrow ones. So the last offset should map to the end of all chars.
/// Also internal strings are null terminated.
    static void fillColumnWithStringData(std::shared_ptr<arrow::ChunkedArray> & arrow_column, IColumn & internal_column)
    {
        PaddedPODArray<UInt8> & column_chars_t = assert_cast<ColumnString &>(internal_column).getChars();
        PaddedPODArray<UInt64> & column_offsets = assert_cast<ColumnString &>(internal_column).getOffsets();

        size_t chars_t_size = 0;
        for (size_t chunk_i = 0, num_chunks = static_cast<size_t>(arrow_column->num_chunks()); chunk_i < num_chunks; ++chunk_i)
        {
            arrow::BinaryArray & chunk = static_cast<arrow::BinaryArray &>(*(arrow_column->chunk(chunk_i)));
            const size_t chunk_length = chunk.length();

            if (chunk_length > 0)
            {
                chars_t_size += chunk.value_offset(chunk_length - 1) + chunk.value_length(chunk_length - 1);
                chars_t_size += chunk_length; /// additional space for null bytes
            }
        }

        column_chars_t.reserve(chars_t_size);
        column_offsets.reserve(arrow_column->length());

        for (size_t chunk_i = 0, num_chunks = static_cast<size_t>(arrow_column->num_chunks()); chunk_i < num_chunks; ++chunk_i)
        {
            arrow::BinaryArray & chunk = static_cast<arrow::BinaryArray &>(*(arrow_column->chunk(chunk_i)));
            std::shared_ptr<arrow::Buffer> buffer = chunk.value_data();
            const size_t chunk_length = chunk.length();

            for (size_t offset_i = 0; offset_i != chunk_length; ++offset_i)
            {
                if (!chunk.IsNull(offset_i) && buffer)
                {
                    const auto * raw_data = buffer->data() + chunk.value_offset(offset_i);
                    column_chars_t.insert_assume_reserved(raw_data, raw_data + chunk.value_length(offset_i));
                }
                column_chars_t.emplace_back('\0');

                column_offsets.emplace_back(column_chars_t.size());
            }
        }
    }

    static void fillColumnWithBooleanData(std::shared_ptr<arrow::ChunkedArray> & arrow_column, IColumn & internal_column)
    {
        auto & column_data = assert_cast<ColumnVector<UInt8> &>(internal_column).getData();
        column_data.reserve(arrow_column->length());

        for (size_t chunk_i = 0, num_chunks = static_cast<size_t>(arrow_column->num_chunks()); chunk_i < num_chunks; ++chunk_i)
        {
            arrow::BooleanArray & chunk = static_cast<arrow::BooleanArray &>(*(arrow_column->chunk(chunk_i)));
            /// buffers[0] is a null bitmap and buffers[1] are actual values
            std::shared_ptr<arrow::Buffer> buffer = chunk.data()->buffers[1];

            for (size_t bool_i = 0; bool_i != static_cast<size_t>(chunk.length()); ++bool_i)
                column_data.emplace_back(chunk.Value(bool_i));
        }
    }

/// Arrow stores Parquet::DATE in Int32, while ClickHouse stores Date in UInt16. Therefore, it should be checked before saving
    static void fillColumnWithDate32Data(std::shared_ptr<arrow::ChunkedArray> & arrow_column, IColumn & internal_column)
    {
        PaddedPODArray<UInt16> & column_data = assert_cast<ColumnVector<UInt16> &>(internal_column).getData();
        column_data.reserve(arrow_column->length());

        for (size_t chunk_i = 0, num_chunks = static_cast<size_t>(arrow_column->num_chunks()); chunk_i < num_chunks; ++chunk_i)
        {
            arrow::Date32Array & chunk = static_cast<arrow::Date32Array &>(*(arrow_column->chunk(chunk_i)));

            for (size_t value_i = 0, length = static_cast<size_t>(chunk.length()); value_i < length; ++value_i)
            {
                UInt32 days_num = static_cast<UInt32>(chunk.Value(value_i));
                if (days_num > DATE_LUT_MAX_DAY_NUM)
                {
                    // TODO: will it rollback correctly?
                    throw Exception{"Input value " + std::to_string(days_num) + " of a column \"" + internal_column.getName()
                                    + "\" is greater than "
                                      "max allowed Date value, which is "
                                    + std::to_string(DATE_LUT_MAX_DAY_NUM),
                                    ErrorCodes::VALUE_IS_OUT_OF_RANGE_OF_DATA_TYPE};
                }

                column_data.emplace_back(days_num);
            }
        }
    }

/// Arrow stores Parquet::DATETIME in Int64, while ClickHouse stores DateTime in UInt32. Therefore, it should be checked before saving
    static void fillColumnWithDate64Data(std::shared_ptr<arrow::ChunkedArray> & arrow_column, IColumn & internal_column)
    {
        auto & column_data = assert_cast<ColumnVector<UInt32> &>(internal_column).getData();
        column_data.reserve(arrow_column->length());

        for (size_t chunk_i = 0, num_chunks = static_cast<size_t>(arrow_column->num_chunks()); chunk_i < num_chunks; ++chunk_i)
        {
            auto & chunk = static_cast<arrow::Date64Array &>(*(arrow_column->chunk(chunk_i)));
            for (size_t value_i = 0, length = static_cast<size_t>(chunk.length()); value_i < length; ++value_i)
            {
                auto timestamp = static_cast<UInt32>(chunk.Value(value_i) / 1000); // Always? in ms
                column_data.emplace_back(timestamp);
            }
        }
    }

    static void fillColumnWithTimestampData(std::shared_ptr<arrow::ChunkedArray> & arrow_column, IColumn & internal_column)
    {
        auto & column_data = assert_cast<ColumnVector<UInt32> &>(internal_column).getData();
        column_data.reserve(arrow_column->length());

        for (size_t chunk_i = 0, num_chunks = static_cast<size_t>(arrow_column->num_chunks()); chunk_i < num_chunks; ++chunk_i)
        {
            auto & chunk = static_cast<arrow::TimestampArray &>(*(arrow_column->chunk(chunk_i)));
            const auto & type = static_cast<const ::arrow::TimestampType &>(*chunk.type());

            UInt32 divide = 1;
            const auto unit = type.unit();
            switch (unit)
            {
                case arrow::TimeUnit::SECOND:
                    divide = 1;
                    break;
                case arrow::TimeUnit::MILLI:
                    divide = 1000;
                    break;
                case arrow::TimeUnit::MICRO:
                    divide = 1000000;
                    break;
                case arrow::TimeUnit::NANO:
                    divide = 1000000000;
                    break;
            }

            for (size_t value_i = 0, length = static_cast<size_t>(chunk.length()); value_i < length; ++value_i)
            {
                auto timestamp = static_cast<UInt32>(chunk.Value(value_i) / divide); // ms! TODO: check other 's' 'ns' ...
                column_data.emplace_back(timestamp);
            }
        }
    }

    static void fillColumnWithDecimalData(std::shared_ptr<arrow::ChunkedArray> & arrow_column, IColumn & internal_column)
    {
        auto & column = assert_cast<ColumnDecimal<Decimal128> &>(internal_column);
        auto & column_data = column.getData();
        column_data.reserve(arrow_column->length());

        for (size_t chunk_i = 0, num_chunks = static_cast<size_t>(arrow_column->num_chunks()); chunk_i < num_chunks; ++chunk_i)
        {
            auto & chunk = static_cast<arrow::DecimalArray &>(*(arrow_column->chunk(chunk_i)));
            for (size_t value_i = 0, length = static_cast<size_t>(chunk.length()); value_i < length; ++value_i)
            {
                column_data.emplace_back(chunk.IsNull(value_i) ? Decimal128(0) : *reinterpret_cast<const Decimal128 *>(chunk.Value(value_i))); // TODO: copy column
            }
        }
    }

/// Creates a null bytemap from arrow's null bitmap
    static void fillByteMapFromArrowColumn(std::shared_ptr<arrow::ChunkedArray> & arrow_column, IColumn & bytemap)
    {
        PaddedPODArray<UInt8> & bytemap_data = assert_cast<ColumnVector<UInt8> &>(bytemap).getData();
        bytemap_data.reserve(arrow_column->length());

        for (size_t chunk_i = 0; chunk_i != static_cast<size_t>(arrow_column->num_chunks()); ++chunk_i)
        {
            std::shared_ptr<arrow::Array> chunk = arrow_column->chunk(chunk_i);

            for (size_t value_i = 0; value_i != static_cast<size_t>(chunk->length()); ++value_i)
                bytemap_data.emplace_back(chunk->IsNull(value_i));
        }
    }

    static void fillOffsetsFromArrowListColumn(std::shared_ptr<arrow::ChunkedArray> & arrow_column, IColumn & offsets)
    {
        ColumnArray::Offsets & offsets_data = assert_cast<ColumnVector<UInt64> &>(offsets).getData();
        offsets_data.reserve(arrow_column->length());

        for (size_t chunk_i = 0, num_chunks = static_cast<size_t>(arrow_column->num_chunks()); chunk_i < num_chunks; ++chunk_i)
        {
            arrow::ListArray & list_chunk = static_cast<arrow::ListArray &>(*(arrow_column->chunk(chunk_i)));
            auto arrow_offsets_array = list_chunk.offsets();
            auto & arrow_offsets = static_cast<arrow::Int32Array &>(*arrow_offsets_array);
            auto start = offsets_data.back();
            for (int64_t i = 1; i < arrow_offsets.length(); ++i)
                offsets_data.emplace_back(start + arrow_offsets.Value(i));
        }
    }

    static void readColumnFromArrowColumn(std::shared_ptr<arrow::ChunkedArray> & arrow_column, IColumn & internal_column, const std::string & column_name, const std::string format_name, bool is_nullable)
    {
        if (internal_column.isNullable())
        {
            ColumnNullable & column_nullable = typeid_cast<ColumnNullable &>(internal_column);
            readColumnFromArrowColumn(arrow_column, column_nullable.getNestedColumn(), column_name, format_name, true);
            fillByteMapFromArrowColumn(arrow_column, column_nullable.getNullMapColumn());
            return;
        }

        // TODO: check if a column is const?
        if (!is_nullable && !checkColumn<ColumnArray>(internal_column) && arrow_column->null_count())
        {
            throw Exception
                {
                    "Can not insert NULL data into non-nullable column \"" + column_name + "\"",
                    ErrorCodes::CANNOT_INSERT_NULL_IN_ORDINARY_COLUMN
                };
        }

        switch (arrow_column->type()->id())
        {
            case arrow::Type::STRING:
            case arrow::Type::BINARY:
                //case arrow::Type::FIXED_SIZE_BINARY:
                fillColumnWithStringData(arrow_column, internal_column);
                break;
            case arrow::Type::BOOL:
                fillColumnWithBooleanData(arrow_column, internal_column);
                break;
            case arrow::Type::DATE32:
                fillColumnWithDate32Data(arrow_column, internal_column);
                break;
            case arrow::Type::DATE64:
                fillColumnWithDate64Data(arrow_column, internal_column);
                break;
            case arrow::Type::TIMESTAMP:
                fillColumnWithTimestampData(arrow_column, internal_column);
                break;
            case arrow::Type::DECIMAL:
                //fillColumnWithNumericData<Decimal128, ColumnDecimal<Decimal128>>(arrow_column, read_column); // Have problems with trash values under NULL, but faster
                fillColumnWithDecimalData(arrow_column, internal_column /*, internal_nested_type*/);
                break;
            case arrow::Type::LIST:
            {
                const auto * list_type = static_cast<arrow::ListType *>(arrow_column->type().get());
                auto list_nested_type = list_type->value_type();
                arrow::ArrayVector array_vector;
                array_vector.reserve(arrow_column->num_chunks());
                for (size_t chunk_i = 0, num_chunks = static_cast<size_t>(arrow_column->num_chunks()); chunk_i < num_chunks; ++chunk_i)
                {
                    arrow::ListArray & list_chunk = static_cast<arrow::ListArray &>(*(arrow_column->chunk(chunk_i)));
                    std::shared_ptr<arrow::Array> chunk = list_chunk.values();
                    array_vector.emplace_back(std::move(chunk));
                }
                auto arrow_nested_column = std::make_shared<arrow::ChunkedArray>(array_vector);

                ColumnArray & column_array = typeid_cast<ColumnArray &>(internal_column);
                readColumnFromArrowColumn(arrow_nested_column, column_array.getData(), column_name, format_name, false);
                fillOffsetsFromArrowListColumn(arrow_column, column_array.getOffsetsColumn());
                break;
            }
#    define DISPATCH(ARROW_NUMERIC_TYPE, CPP_NUMERIC_TYPE) \
        case ARROW_NUMERIC_TYPE: \
            fillColumnWithNumericData<CPP_NUMERIC_TYPE>(arrow_column, internal_column); \
            break;

            FOR_ARROW_NUMERIC_TYPES(DISPATCH)
#    undef DISPATCH
                // TODO: support TIMESTAMP_MICROS and TIMESTAMP_MILLIS with truncated micro- and milliseconds?
                // TODO: read JSON as a string?
                // TODO: read UUID as a string?
            default:
                throw Exception
                    {
                        "Unsupported " + format_name + " type \"" + arrow_column->type()->name() + "\" of an input column \""
                        + column_name + "\"",
                        ErrorCodes::UNKNOWN_TYPE
                    };
        }
    }

    static DataTypePtr getInternalType(std::shared_ptr<arrow::DataType> arrow_type, const DataTypePtr & column_type, const std::string & column_name, const std::string & format_name)
    {
        if (column_type->isNullable())
        {
            DataTypePtr nested_type = typeid_cast<const DataTypeNullable *>(column_type.get())->getNestedType();
            return makeNullable(getInternalType(arrow_type, nested_type, column_name, format_name));
        }

        if (arrow_type->id() == arrow::Type::DECIMAL)
        {
            const auto * decimal_type = static_cast<arrow::DecimalType *>(arrow_type.get());
            return std::make_shared<DataTypeDecimal<Decimal128>>(decimal_type->precision(), decimal_type->scale());
        }

        if (arrow_type->id() == arrow::Type::LIST)
        {
            const auto * list_type = static_cast<arrow::ListType *>(arrow_type.get());
            auto list_nested_type = list_type->value_type();

            const DataTypeArray * array_type = typeid_cast<const DataTypeArray *>(column_type.get());
            if (!array_type)
                throw Exception{"Cannot convert arrow LIST type to a not Array ClickHouse type " + column_type->getName(), ErrorCodes::CANNOT_CONVERT_TYPE};

            return std::make_shared<DataTypeArray>(getInternalType(list_nested_type, array_type->getNestedType(), column_name, format_name));
        }

        if (const auto * internal_type_it = std::find_if(arrow_type_to_internal_type.begin(), arrow_type_to_internal_type.end(),
                                                              [=](auto && elem) { return elem.first == arrow_type->id(); });
            internal_type_it != arrow_type_to_internal_type.end())
        {
            return DataTypeFactory::instance().get(internal_type_it->second);
        }
        throw Exception
            {
                "The type \"" + arrow_type->name() + "\" of an input column \"" + column_name + "\" is not supported for conversion from a " + format_name + " data format",
                ErrorCodes::CANNOT_CONVERT_TYPE
            };
    }

    void ArrowColumnToCHColumn::arrowTableToCHChunk(Chunk & res, std::shared_ptr<arrow::Table> & table,
                                                    const Block & header, std::string format_name)
    {
        Columns columns_list;
        UInt64 num_rows = 0;

        columns_list.reserve(header.rows());

        using NameToColumnPtr = std::unordered_map<std::string, std::shared_ptr<arrow::ChunkedArray>>;

        NameToColumnPtr name_to_column_ptr;
        for (const auto& column_name : table->ColumnNames())
        {
            std::shared_ptr<arrow::ChunkedArray> arrow_column = table->GetColumnByName(column_name);
            name_to_column_ptr[column_name] = arrow_column;
        }

        for (size_t column_i = 0, columns = header.columns(); column_i < columns; ++column_i)
        {
            ColumnWithTypeAndName header_column = header.getByPosition(column_i);
            const auto column_type = recursiveRemoveLowCardinality(header_column.type);

            if (name_to_column_ptr.find(header_column.name) == name_to_column_ptr.end())
                // TODO: What if some columns were not presented? Insert NULLs? What if a column is not nullable?
                throw Exception{"Column \"" + header_column.name + "\" is not presented in input data",
                                ErrorCodes::THERE_IS_NO_COLUMN};

            std::shared_ptr<arrow::ChunkedArray> arrow_column = name_to_column_ptr[header_column.name];

            DataTypePtr internal_type = getInternalType(arrow_column->type(), column_type, header_column.name, format_name);

            MutableColumnPtr read_column = internal_type->createColumn();
            readColumnFromArrowColumn(arrow_column, *read_column, header_column.name, format_name, false);

            ColumnWithTypeAndName column;
            column.name = header_column.name;
            column.type = internal_type;
            column.column = std::move(read_column);

            column.column = castColumn(column, header_column.type);
            column.type = header_column.type;
            num_rows = column.column->size();
            columns_list.push_back(std::move(column.column));
        }

        res.setColumns(columns_list, num_rows);
    }
}
#endif
