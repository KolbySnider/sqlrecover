#pragma once
/// @file
/// @brief sqlite_master parsing. Page 1 is the schema table root; each
/// row is (type, name, tbl_name, rootpage, sql). We use it for two
/// things: find root pages to walk, and name columns on recovered rows
/// whose arity matches a known table.

#include <vector>
#include <string>
#include <cstdint>
#include "core/types.hpp"

namespace sqlrecover {

class Database;

/// @brief One user table from sqlite_master.
struct TableDef {
    std::string              name;
    uint32_t                 root_page = 0;
    std::vector<std::string> columns;
};

/// @brief Read sqlite_master and return user tables (anything not
/// prefixed sqlite_).
std::vector<TableDef> read_schema(const Database& db);

/// @brief Pull column names out of a CREATE TABLE statement. Not a real
/// SQL parser -- just grabs the leading identifier of each top-level
/// comma-separated piece.
/// @param create_sql The full "CREATE TABLE ..." text from sqlite_master.
/// @return Column names in order; empty if anything looks weird rather
/// than guessing.
std::vector<std::string> parse_create_columns(const std::string& create_sql);

} // namespace sqlrecover
