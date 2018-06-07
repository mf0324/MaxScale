/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2020-01-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#include "avrorouter.hh"

#include <maxscale/mysql_utils.h>
#include <jansson.h>
#include <maxscale/alloc.h>
#include <strings.h>
#include <signal.h>
#include <maxscale/utils.h>

#define WRITE_EVENT         0
#define UPDATE_EVENT        1
#define UPDATE_EVENT_AFTER  2
#define DELETE_EVENT        3

static bool warn_decimal = false; /**< Remove when support for DECIMAL is added */
static bool warn_bit = false; /**< Remove when support for BIT is added */
static bool warn_large_enumset = false; /**< Remove when support for ENUM/SET values
                                         * larger than 255 is added */

uint8_t* process_row_event_data(STableMapEvent map, STableCreateEvent create,
                                RowEventHandler* conv, uint8_t *ptr,
                                uint8_t *columns_present, uint8_t *end);

/**
 * @brief Get row event name
 * @param event Event type
 * @return String representation of the event
 */
static int get_event_type(uint8_t event)
{
    switch (event)
    {

    case WRITE_ROWS_EVENTv0:
    case WRITE_ROWS_EVENTv1:
    case WRITE_ROWS_EVENTv2:
        return WRITE_EVENT;

    case UPDATE_ROWS_EVENTv0:
    case UPDATE_ROWS_EVENTv1:
    case UPDATE_ROWS_EVENTv2:
        return UPDATE_EVENT;

    case DELETE_ROWS_EVENTv0:
    case DELETE_ROWS_EVENTv1:
    case DELETE_ROWS_EVENTv2:
        return DELETE_EVENT;

    default:
        MXS_ERROR("Unexpected event type: %d (%0x)", event, event);
        return -1;
    }
}

/**
 * @brief Handle a table map event
 *
 * This converts a table map events into table meta data that will be used when
 * converting binlogs to Avro format.
 * @param router Avro router instance
 * @param hdr Replication header
 * @param ptr Pointer to event payload
 */
bool handle_table_map_event(Avro *router, REP_HEADER *hdr, uint8_t *ptr)
{
    bool rval = false;
    uint64_t id;
    char table_ident[MYSQL_TABLE_MAXLEN + MYSQL_DATABASE_MAXLEN + 2];
    int ev_len = router->event_type_hdr_lens[hdr->event_type];

    read_table_info(ptr, ev_len, &id, table_ident, sizeof(table_ident));
    auto create = router->created_tables.find(table_ident);

    if (create != router->created_tables.end())
    {
        ss_dassert(create->second->columns.size() > 0);
        auto it = router->table_maps.find(table_ident);
        STableMapEvent map(table_map_alloc(ptr, ev_len, create->second.get()));

        if (it != router->table_maps.end())
        {
            auto old = it->second;

            if (old->id == map->id && old->version == map->version &&
                old->table == map->table && old->database == map->database)
            {
                // We can reuse the table map object
                return true;
            }
        }

        if (router->event_hander->open_table(map, create->second))
        {
            create->second->was_used = true;

            auto old = router->table_maps.find(table_ident);
            bool notify = old != router->table_maps.end();

            if (notify)
            {
                router->active_maps.erase(old->second->id);
            }

            router->table_maps[table_ident] = map;
            router->active_maps[map->id] = map;
            ss_dassert(router->active_maps[id] == map);
            MXS_DEBUG("Table %s mapped to %lu", table_ident, map->id);
            rval = true;

            if (notify)
            {
                notify_all_clients(router);
            }
        }
    }
    else
    {
        MXS_WARNING("Table map event for table '%s' read before the DDL statement "
                    "for that table  was read. Data will not be processed for this "
                    "table until a DDL statement for it is read.", table_ident);
    }

    if (rval)
    {
        MXS_INFO("Table Map for '%s' at %lu", table_ident, router->current_pos);
    }

    return rval;
}

/**
 * @brief Handle a single RBR row event
 *
 * These events contain the changes in the data. This function assumes that full
 * row image is sent in every row event.
 *
 * @param router Avro router instance
 * @param hdr Replication header
 * @param ptr Pointer to the start of the event
 * @return True on succcess, false on error
 */
bool handle_row_event(Avro *router, REP_HEADER *hdr, uint8_t *ptr)
{
    bool rval = false;
    uint8_t *start = ptr;
    uint8_t *end = ptr + hdr->event_size - BINLOG_EVENT_HDR_LEN;
    uint8_t table_id_size = router->event_type_hdr_lens[hdr->event_type] == 6 ? 4 : 6;
    uint64_t table_id = 0;

    /** The first value is the ID where the table was mapped. This should be
     * the same as the ID in the table map even which was processed before this
     * row event. */
    memcpy(&table_id, ptr, table_id_size);
    ptr += table_id_size;

    /** Replication flags, currently ignored for the most part. */
    uint16_t flags = 0;
    memcpy(&flags, ptr, 2);
    ptr += 2;

    if (table_id == TABLE_DUMMY_ID && flags & ROW_EVENT_END_STATEMENT)
    {
        /** This is an dummy event which should release all table maps. Right
         * now we just return without processing the rows. */
        return true;
    }

    /** Newer replication events have extra data stored in the header. MariaDB
     * 10.1 does not use these and instead use the v1 events */
    if (hdr->event_type > DELETE_ROWS_EVENTv1)
    {
        /** Version 2 row event, skip extra data */
        uint16_t extra_len = 0;
        memcpy(&extra_len, ptr, 2);
        ptr += 2 + extra_len;
    }

    /** Number of columns in the table */
    uint64_t ncolumns = mxs_leint_consume(&ptr);

    /** If full row image is used, all columns are present. Currently only full
     * row image is supported and thus the bitfield should be all ones. In
     * the future partial row images could be used if the bitfield containing
     * the columns that are present in this event is used. */
    const int coldata_size = (ncolumns + 7) / 8;
    uint8_t col_present[coldata_size];
    memcpy(&col_present, ptr, coldata_size);
    ptr += coldata_size;

    /** Update events have the before and after images of the row. This can be
     * used to calculate a "delta" of sorts if necessary. Currently we store
     * both the before and the after images. */
    uint8_t col_update[coldata_size];
    if (hdr->event_type == UPDATE_ROWS_EVENTv1 ||
        hdr->event_type == UPDATE_ROWS_EVENTv2)
    {
        memcpy(&col_update, ptr, coldata_size);
        ptr += coldata_size;
    }

    // There should always be a table map event prior to a row event.

    auto it = router->active_maps.find(table_id);

    if (it != router->active_maps.end())
    {
        STableMapEvent map = it->second;
        char table_ident[MYSQL_TABLE_MAXLEN + MYSQL_DATABASE_MAXLEN + 2];
        snprintf(table_ident, sizeof(table_ident), "%s.%s", map->database.c_str(), map->table.c_str());

        bool ok = router->event_hander->prepare_table(map->database, map->table);
        auto create = router->created_tables.find(table_ident);

        if (ok && create != router->created_tables.end() &&
            ncolumns == map->columns() && create->second->columns.size() == map->columns())
        {
            /** Each event has one or more rows in it. The number of rows is not known
             * beforehand so we must continue processing them until we reach the end
             * of the event. */
            int rows = 0;
            MXS_INFO("Row Event for '%s' at %lu", table_ident, router->current_pos);

            while (ptr < end)
            {
                static uint64_t total_row_count = 1;
                int event_type = get_event_type(hdr->event_type);

                // Increment the event count for this transaction
                router->gtid.event_num++;

                router->event_hander->prepare_row(router->gtid, *hdr, event_type);
                ptr = process_row_event_data(map, create->second, router->event_hander, ptr, col_present, end);
                router->event_hander->commit(router->gtid);

                /** Update rows events have the before and after images of the
                 * affected rows so we'll process them as another record with
                 * a different type */
                if (event_type == UPDATE_EVENT)
                {
                    router->event_hander->prepare_row(router->gtid, *hdr, UPDATE_EVENT_AFTER);
                    ptr = process_row_event_data(map, create->second, router->event_hander, ptr, col_present, end);
                    router->event_hander->commit(router->gtid);
                }

                rows++;
            }

            rval = true;
        }
        else if (!ok)
        {
            MXS_ERROR("Avro file handle was not found for table %s.%s. See earlier"
                      " errors for more details.", map->database.c_str(), map->table.c_str());
        }
        else if (create == router->created_tables.end())
        {
            MXS_ERROR("Create table statement for %s.%s was not found from the "
                      "binary logs or the stored schema was not correct.",
                      map->database.c_str(), map->table.c_str());
        }
        else if (ncolumns == map->columns() && create->second->columns.size() != map->columns())
        {
            MXS_ERROR("Table map event has a different column count for table "
                      "%s.%s than the CREATE TABLE statement. Possible "
                      "unsupported DDL detected.", map->database.c_str(), map->table.c_str());
        }
        else
        {
            MXS_ERROR("Row event and table map event have different column "
                      "counts for table %s.%s, only full row image is currently "
                      "supported.", map->database.c_str(), map->table.c_str());
        }
    }
    else
    {
        MXS_ERROR("Row event for unknown table mapped to ID %lu. Data will not "
                  "be processed.", table_id);
    }

    return rval;
}

/**
 * @brief Unpack numeric types
 *
 * Convert the raw binary data into actual numeric types.
 *
 * @param conv     Event converter to use
 * @param idx      Position of this column in the row
 * @param type     Event type
 * @param metadata Field metadata
 * @param value    Pointer to the start of the in-memory representation of the data
 */
void set_numeric_field_value(RowEventHandler* conv, int idx, uint8_t type,
                             uint8_t *metadata, uint8_t *value)
{
    switch (type)
    {
    case TABLE_COL_TYPE_TINY:
        {
            char c = *value;
            conv->column(idx, c);
            break;
        }

    case TABLE_COL_TYPE_SHORT:
        {
            short s =  gw_mysql_get_byte2(value);
            conv->column(idx, s);
            break;
        }

    case TABLE_COL_TYPE_INT24:
        {
            int x = gw_mysql_get_byte3(value);

            if (x & 0x800000)
            {
                x = -((0xffffff & (~x)) + 1);
            }

            conv->column(idx, x);
            break;
        }

    case TABLE_COL_TYPE_LONG:
        {
            int x = gw_mysql_get_byte4(value);
            conv->column(idx, x);
            break;
        }

    case TABLE_COL_TYPE_LONGLONG:
        {
            long l = gw_mysql_get_byte8(value);
            conv->column(idx, l);
            break;
        }

    case TABLE_COL_TYPE_FLOAT:
        {
            float f = 0;
            memcpy(&f, value, 4);
            conv->column(idx, f);
            break;
        }

    case TABLE_COL_TYPE_DOUBLE:
        {
            double d = 0;
            memcpy(&d, value, 8);
            conv->column(idx, d);
            break;
        }

    default:
        break;
    }
}

/**
 * @brief Check if a bit is set
 *
 * @param ptr Pointer to start of bitfield
 * @param columns Number of columns (bits)
 * @param current_column Zero indexed column number
 * @return True if the bit is set
 */
static bool bit_is_set(uint8_t *ptr, int columns, int current_column)
{
    if (current_column >= 8)
    {
        ptr += current_column / 8;
        current_column = current_column % 8;
    }

    return ((*ptr) & (1 << current_column));
}

/**
 * @brief Get the length of the metadata for a particular field
 *
 * @param type Type of the field
 * @return Length of the metadata for this field
 */
int get_metadata_len(uint8_t type)
{
    switch (type)
    {
    case TABLE_COL_TYPE_STRING:
    case TABLE_COL_TYPE_VAR_STRING:
    case TABLE_COL_TYPE_VARCHAR:
    case TABLE_COL_TYPE_DECIMAL:
    case TABLE_COL_TYPE_NEWDECIMAL:
    case TABLE_COL_TYPE_ENUM:
    case TABLE_COL_TYPE_SET:
    case TABLE_COL_TYPE_BIT:
        return 2;

    case TABLE_COL_TYPE_BLOB:
    case TABLE_COL_TYPE_FLOAT:
    case TABLE_COL_TYPE_DOUBLE:
    case TABLE_COL_TYPE_DATETIME2:
    case TABLE_COL_TYPE_TIMESTAMP2:
    case TABLE_COL_TYPE_TIME2:
        return 1;

    default:
        return 0;
    }
}

// Make sure that both `i` and `trace` are defined before using this macro
#define check_overflow(t) do \
    { \
        if (!(t)) \
        { \
            for (long x = 0; x < i;x++) \
            { \
                MXS_ALERT("%s", trace[x]); \
            } \
            raise(SIGABRT); \
        } \
    }while(false)

// Debug function for checking whether a row event consists of only NULL values
static bool all_fields_null(uint8_t* null_bitmap, int ncolumns)
{
    bool rval = true;

    for (long i = 0; i < ncolumns; i++)
    {
        if (!bit_is_set(null_bitmap, ncolumns, i))
        {
            rval = false;
            break;
        }
    }

    return rval;
}

/**
 * @brief Extract the values from a single row  in a row event
 *
 * @param map Table map event associated with this row
 * @param create Table creation associated with this row
 * @param record Avro record used for storing this row
 * @param ptr Pointer to the start of the row data, should be after the row event header
 * @param columns_present The bitfield holding the columns that are present for
 * this row event. Currently this should be a bitfield which has all bits set.
 * @return Pointer to the first byte after the current row event
 */
uint8_t* process_row_event_data(STableMapEvent map, STableCreateEvent create, RowEventHandler* conv,
                                uint8_t *ptr, uint8_t *columns_present, uint8_t *end)
{
    int npresent = 0;
    long ncolumns = map->columns();
    uint8_t *metadata = &map->column_metadata[0];
    size_t metadata_offset = 0;

    /** BIT type values use the extra bits in the row event header */
    int extra_bits = (((ncolumns + 7) / 8) * 8) - ncolumns;
    ss_dassert(ptr < end);

    /** Store the null value bitmap */
    uint8_t *null_bitmap = ptr;
    ptr += (ncolumns + 7) / 8;
    ss_dassert(ptr < end || (bit_is_set(null_bitmap, ncolumns, 0)));

    char trace[ncolumns][768];
    memset(trace, 0, sizeof(trace));

    for (long i = 0; i < ncolumns && npresent < ncolumns; i++)
    {
        if (bit_is_set(columns_present, ncolumns, i))
        {
            npresent++;
            if (bit_is_set(null_bitmap, ncolumns, i))
            {
                sprintf(trace[i], "[%ld] NULL", i);
                conv->column(i);
            }
            else if (column_is_fixed_string(map->column_types[i]))
            {
                /** ENUM and SET are stored as STRING types with the type stored
                 * in the metadata. */
                if (fixed_string_is_enum(metadata[metadata_offset]))
                {
                    uint8_t val[metadata[metadata_offset + 1]];
                    uint64_t bytes = unpack_enum(ptr, &metadata[metadata_offset], val);
                    char strval[bytes * 2 + 1];
                    gw_bin2hex(strval, val, bytes);
                    conv->column(i, strval);
                    sprintf(trace[i], "[%ld] ENUM: %lu bytes", i, bytes);
                    ptr += bytes;
                    check_overflow(ptr <= end);
                }
                else
                {
                    /**
                     * The first byte in the metadata stores the real type of
                     * the string (ENUM and SET types are also stored as fixed
                     * length strings).
                     *
                     * The first two bits of the second byte contain the XOR'ed
                     * field length but as that information is not relevant for
                     * us, we just use this information to know whether to read
                     * one or two bytes for string length.
                     */

                    uint16_t meta = metadata[metadata_offset + 1] + (metadata[metadata_offset] << 8);
                    int bytes = 0;
                    uint16_t extra_length = (((meta >> 4) & 0x300) ^ 0x300);
                    uint16_t field_length = (meta & 0xff) + extra_length;

                    if (field_length > 255)
                    {
                        bytes = ptr[0] + (ptr[1] << 8);
                        ptr += 2;
                    }
                    else
                    {
                        bytes = *ptr++;
                    }

                    sprintf(trace[i], "[%ld] CHAR: field: %d bytes, data: %d bytes", i, field_length, bytes);
                    char str[bytes + 1];
                    memcpy(str, ptr, bytes);
                    str[bytes] = '\0';
                    conv->column(i, str);
                    ptr += bytes;
                    check_overflow(ptr <= end);
                }
            }
            else if (column_is_bit(map->column_types[i]))
            {
                uint8_t len = metadata[metadata_offset + 1];
                uint8_t bit_len = metadata[metadata_offset] > 0 ? 1 : 0;
                size_t bytes = len + bit_len;

                // TODO: extract the bytes
                if (!warn_bit)
                {
                    warn_bit = true;
                    MXS_WARNING("BIT is not currently supported, values are stored as 0.");
                }
                conv->column(i, 0);
                sprintf(trace[i], "[%ld] BIT", i);
                ptr += bytes;
                check_overflow(ptr <= end);
            }
            else if (column_is_decimal(map->column_types[i]))
            {
                double f_value = 0.0;
                ptr += unpack_decimal_field(ptr, metadata + metadata_offset, &f_value);
                conv->column(i, f_value);
                sprintf(trace[i], "[%ld] DECIMAL", i);
                check_overflow(ptr <= end);
            }
            else if (column_is_variable_string(map->column_types[i]))
            {
                size_t sz;
                int bytes = metadata[metadata_offset] | metadata[metadata_offset + 1] << 8;
                if (bytes > 255)
                {
                    sz = gw_mysql_get_byte2(ptr);
                    ptr += 2;
                }
                else
                {
                    sz = *ptr;
                    ptr++;
                }

                sprintf(trace[i], "[%ld] VARCHAR: field: %d bytes, data: %lu bytes", i, bytes, sz);
                char buf[sz + 1];
                memcpy(buf, ptr, sz);
                buf[sz] = '\0';
                ptr += sz;
                conv->column(i, buf);
                check_overflow(ptr <= end);
            }
            else if (column_is_blob(map->column_types[i]))
            {
                uint8_t bytes = metadata[metadata_offset];
                uint64_t len = 0;
                memcpy(&len, ptr, bytes);
                ptr += bytes;
                sprintf(trace[i], "[%ld] BLOB: field: %d bytes, data: %lu bytes", i, bytes, len);
                if (len)
                {
                    conv->column(i, ptr, len);
                    ptr += len;
                }
                else
                {
                    uint8_t nullvalue = 0;
                    conv->column(i, &nullvalue, 1);
                }
                check_overflow(ptr <= end);
            }
            else if (column_is_temporal(map->column_types[i]))
            {
                char buf[80];
                struct tm tm;
                ptr += unpack_temporal_value(map->column_types[i], ptr,
                                             &metadata[metadata_offset],
                                             create->columns[i].length, &tm);
                format_temporal_value(buf, sizeof(buf), map->column_types[i], &tm);
                conv->column(i, buf);
                sprintf(trace[i], "[%ld] %s: %s", i, column_type_to_string(map->column_types[i]), buf);
                check_overflow(ptr <= end);
            }
            /** All numeric types (INT, LONG, FLOAT etc.) */
            else
            {
                uint8_t lval[16];
                memset(lval, 0, sizeof(lval));
                ptr += unpack_numeric_field(ptr, map->column_types[i],
                                            &metadata[metadata_offset], lval);
                set_numeric_field_value(conv, i, map->column_types[i], &metadata[metadata_offset], lval);
                sprintf(trace[i], "[%ld] %s", i, column_type_to_string(map->column_types[i]));
                check_overflow(ptr <= end);
            }
            ss_dassert(metadata_offset <= map->column_metadata.size());
            metadata_offset += get_metadata_len(map->column_types[i]);
        }
        else
        {
            sprintf(trace[i], "[%ld] %s: Not present", i, column_type_to_string(map->column_types[i]));
        }

        MXS_INFO("%s", trace[i]);
    }

    return ptr;
}