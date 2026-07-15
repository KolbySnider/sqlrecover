#include "format/schema.hpp"
#include "format/database.hpp"
#include "format/btree.hpp"
#include <cctype>

namespace sqlrecover {

std::vector<std::string> parse_create_columns(const std::string& sql) {
    // Find the top-level "(...)" and split on commas at depth 1, taking the
    // first identifier of each piece. Anything weird falls through to empty
    // and we use positional labels.
    std::vector<std::string> cols;
    size_t open = sql.find('(');
    if (open == std::string::npos) return cols;

    int depth = 0;
    std::string cur;
    auto flush = [&]() {
        size_t i = 0;
        while (i < cur.size() && std::isspace((unsigned char)cur[i])) ++i;
        std::string name;
        if (i < cur.size() && (cur[i]=='"'||cur[i]=='`'||cur[i]=='[')) {
            char close = (cur[i]=='[') ? ']' : cur[i];
            ++i;
            while (i < cur.size() && cur[i] != close) name += cur[i++];
        } else {
            while (i < cur.size() && (std::isalnum((unsigned char)cur[i]) || cur[i]=='_'))
                name += cur[i++];
        }
        // Skip table-level constraints that look like columns
        std::string upper;
        for (char ch : name) upper += std::toupper((unsigned char)ch);
        if (!name.empty() && upper!="PRIMARY" && upper!="UNIQUE" &&
            upper!="CHECK" && upper!="FOREIGN" && upper!="CONSTRAINT")
            cols.push_back(name);
        cur.clear();
    };

    for (size_t i = open; i < sql.size(); ++i) {
        char ch = sql[i];
        if (ch == '(') { if (++depth == 1) continue; }
        if (ch == ')') { if (--depth == 0) { flush(); break; } }
        if (depth == 1 && ch == ',') { flush(); continue; }
        if (depth >= 1) cur += ch;
    }
    return cols;
}

std::vector<TableDef> read_schema(const Database& db) {
    // sqlite_master is a normal table rooted at page 1.
    // Columns: type, name, tbl_name, rootpage, sql
    std::vector<TableDef> defs;
    std::vector<bool> visited(db.page_count() + 2, false);

    walk_table_btree(db, 1, "sqlite_master", visited, [&](Record&& r) {
        if (r.values.size() < 5) return;
        const Value& type = r.values[0];
        const Value& name = r.values[1];
        const Value& root = r.values[3];
        const Value& sql  = r.values[4];
        if (type.type != Value::Type::Text || type.text != "table") return;
        if (root.type != Value::Type::Int) return;

        TableDef d;
        d.name = (name.type == Value::Type::Text) ? name.text : "";
        d.root_page = static_cast<uint32_t>(root.i);
        if (sql.type == Value::Type::Text)
            d.columns = parse_create_columns(sql.text);
        if (d.root_page != 0 && d.name.rfind("sqlite_", 0) != 0)
            defs.push_back(std::move(d));
    });
    return defs;
}

} // namespace sqlrecover
