#pragma once
//
// sqlite_master parsing. Page 1 roots the schema table, whose rows describe
// every table/index: type, name, root page, and the CREATE statement. We use it
// to (a) find table root pages to walk and (b) label recovered columns by name
// when a residual record's column count matches a known table.
//
#include <vector>
#include <string>
#include <cstdint>
#include "sqlrecover/types.hpp"

namespace sqlrecover {

class Database;

struct TableDef {
    std::string              name;
    uint32_t                 root_page = 0;
    std::vector<std::string> columns;     // parsed from CREATE TABLE
};

// Read sqlite_master from page 1 and return ordinary table definitions.
std::vector<TableDef> read_schema(const Database& db);

// Very small CREATE TABLE column-name extractor. Not a full SQL parser — it
// pulls the leading identifier of each top-level comma-separated column def,
// which is enough to label fields. Returns empty on anything it can't handle.
std::vector<std::string> parse_create_columns(const std::string& create_sql);

} // namespace sqlrecover
