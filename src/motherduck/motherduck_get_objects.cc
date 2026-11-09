// Copyright (c) 2026 ADBC Drivers Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// For #warning Please include winsock2.h before windows.h on RTools/msys2
#ifdef _WIN32
#include <winsock2.h>
#endif

#include "motherduck_get_objects.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "result_helper.h"

namespace adbcpq {

namespace {

using adbc::driver::GetObjectsDepth;
using adbc::driver::GetObjectsHelper;
using adbc::driver::Result;
using adbc::driver::Status;

// Rows from user databases only: not system, temp or other internal ones, nor
// MotherDuck's md_information_schema, which isn't marked internal.
#define MOTHERDUCK_USER_DATABASE "NOT d.internal AND d.type <> 'motherduck_info'"

constexpr char kCatalogsQuery[] =
    "SELECT d.database_name FROM duckdb_databases() AS d "
    "WHERE " MOTHERDUCK_USER_DATABASE;

constexpr char kSchemasQuery[] =
    "SELECT s.database_name, s.schema_name FROM duckdb_schemas() AS s "
    "JOIN duckdb_databases() AS d ON d.database_name = s.database_name "
    "WHERE " MOTHERDUCK_USER_DATABASE;

constexpr char kTablesQuery[] =
    "SELECT t.database_name, t.schema_name, t.table_name, t.table_type FROM ("
    "SELECT database_name, schema_name, table_name, 'table' AS table_type "
    "FROM duckdb_tables() WHERE NOT internal "
    "UNION ALL SELECT database_name, schema_name, view_name, 'view' "
    "FROM duckdb_views() WHERE NOT internal) AS t "
    "JOIN duckdb_databases() AS d ON d.database_name = t.database_name "
    "WHERE " MOTHERDUCK_USER_DATABASE;

constexpr char kColumnsQuery[] =
    "SELECT c.database_name, c.schema_name, c.table_name, c.column_name, "
    "c.column_index, c.comment, c.data_type FROM duckdb_columns() AS c "
    "JOIN duckdb_databases() AS d ON d.database_name = c.database_name "
    "WHERE " MOTHERDUCK_USER_DATABASE " AND NOT c.internal";

// One row per constraint column, in order, so no array text needs parsing. A
// constraint on no column, such as CHECK (random() > 0), gets one row with a
// NULL column. referenced_column_names is empty except for foreign keys, so
// ref is NULL.
constexpr char kConstraintsQuery[] =
    "SELECT k.database_name, k.schema_name, k.table_name, k.constraint_index, "
    "k.constraint_name, k.constraint_type, k.constraint_column_names[k.i], "
    "k.referenced_table, k.referenced_column_names[k.i] FROM ("
    "SELECT *, unnest(range(1, greatest(len(constraint_column_names), 1) + 1)) AS i "
    "FROM duckdb_constraints() "
    "WHERE constraint_type IN ('PRIMARY KEY', 'FOREIGN KEY', 'UNIQUE', 'CHECK')) AS k "
    "JOIN duckdb_databases() AS d ON d.database_name = k.database_name "
    "WHERE " MOTHERDUCK_USER_DATABASE;

#undef MOTHERDUCK_USER_DATABASE

// A query plus the filters added to it as parameters.
class Query {
 public:
  explicit Query(const char* sql) : sql_(sql) {}

  // Adds `column ILIKE pattern`, if there is a pattern.
  Query& Like(const char* column, std::optional<std::string_view> pattern) {
    if (pattern) {
      sql_ += std::string(" AND ") + column + " ILIKE " + Param(*pattern);
    }
    return *this;
  }

  // Adds `column IN (values)`, if there are values.
  Query& In(const char* column, const std::vector<std::string_view>& values) {
    if (values.empty()) return *this;
    sql_ += std::string(" AND ") + column + " IN (";
    for (size_t i = 0; i < values.size(); i++) {
      if (i > 0) sql_ += ", ";
      sql_ += Param(values[i]);
    }
    sql_ += ")";
    return *this;
  }

  // Runs the query, ordered by the given columns, and calls on_row for each row.
  template <typename OnRow>
  Status Run(PGconn* conn, const char* order_by, OnRow&& on_row) {
    PqResultHelper helper(conn, sql_ + " ORDER BY " + order_by);
    UNWRAP_STATUS(helper.Execute(params_));
    for (PqResultRow row : helper) {
      UNWRAP_STATUS(on_row(row));
    }
    return Status::Ok();
  }

 private:
  // The endpoint can't infer parameter types, so each is cast.
  std::string Param(std::string_view value) {
    params_.emplace_back(value);
    return "$" + std::to_string(params_.size()) + "::VARCHAR";
  }

  std::string sql_;
  std::vector<std::string> params_;
};

std::string Text(const PqRecord& record) { return std::string(record.data, record.len); }

std::optional<std::string> NullableText(const PqRecord& record) {
  if (record.is_null) return std::nullopt;
  return Text(record);
}

using SchemaKey = std::pair<std::string, std::string>;
using TableKey = std::tuple<std::string, std::string, std::string>;

struct TableRow {
  std::string name;
  std::string type;
};

struct ColumnRow {
  std::string name;
  int32_t ordinal_position;
  std::optional<std::string> remarks;
  std::string type_name;
};

struct ConstraintRow {
  int64_t index;
  std::string name;
  std::string type;
  std::vector<std::string> column_names;
  std::string referenced_table;
  std::vector<std::string> referenced_column_names;
};

// Steps through the rows for one key of a map, or none if the key is absent.
template <typename Row>
class Cursor {
 public:
  template <typename Key>
  void Reset(const std::map<Key, std::vector<Row>>& rows, const Key& key) {
    auto it = rows.find(key);
    rows_ = it == rows.end() ? nullptr : &it->second;
    next_ = 0;
  }

  const Row* Next() {
    if (rows_ == nullptr || next_ >= rows_->size()) return nullptr;
    return &(*rows_)[next_++];
  }

 private:
  const std::vector<Row>* rows_ = nullptr;
  size_t next_ = 0;
};

class MotherduckGetObjectsHelper : public GetObjectsHelper {
 public:
  explicit MotherduckGetObjectsHelper(PGconn* conn) : conn_(conn) {}

  Status Load(GetObjectsDepth depth, std::optional<std::string_view> catalog_filter,
              std::optional<std::string_view> schema_filter,
              std::optional<std::string_view> table_filter,
              std::optional<std::string_view> column_filter,
              const std::vector<std::string_view>& table_types) override {
    UNWRAP_STATUS(Query(kCatalogsQuery)
                      .Like("d.database_name", catalog_filter)
                      .Run(conn_, "1", [&](const PqResultRow& row) {
                        catalogs_.push_back(Text(row[0]));
                        return Status::Ok();
                      }));
    if (depth == GetObjectsDepth::kCatalogs) return Status::Ok();

    UNWRAP_STATUS(Query(kSchemasQuery)
                      .Like("s.database_name", catalog_filter)
                      .Like("s.schema_name", schema_filter)
                      .Run(conn_, "1, 2", [&](const PqResultRow& row) {
                        schemas_[Text(row[0])].push_back(Text(row[1]));
                        return Status::Ok();
                      }));
    if (depth == GetObjectsDepth::kSchemas) return Status::Ok();

    UNWRAP_STATUS(Query(kTablesQuery)
                      .Like("t.database_name", catalog_filter)
                      .Like("t.schema_name", schema_filter)
                      .Like("t.table_name", table_filter)
                      .In("t.table_type", table_types)
                      .Run(conn_, "1, 2, 3", [&](const PqResultRow& row) {
                        tables_[{Text(row[0]), Text(row[1])}].push_back(
                            {Text(row[2]), Text(row[3])});
                        return Status::Ok();
                      }));
    if (depth == GetObjectsDepth::kTables) return Status::Ok();

    UNWRAP_STATUS(Query(kColumnsQuery)
                      .Like("c.database_name", catalog_filter)
                      .Like("c.schema_name", schema_filter)
                      .Like("c.table_name", table_filter)
                      .Like("c.column_name", column_filter)
                      .Run(conn_, "1, 2, 3, 5", [&](const PqResultRow& row) -> Status {
                        int64_t ordinal_position;
                        UNWRAP_RESULT(ordinal_position, row[4].ParseInteger());
                        columns_[{Text(row[0]), Text(row[1]), Text(row[2])}].push_back(
                            {Text(row[3]), static_cast<int32_t>(ordinal_position),
                             NullableText(row[5]), Text(row[6])});
                        return Status::Ok();
                      }));

    UNWRAP_STATUS(
        Query(kConstraintsQuery)
            .Like("k.database_name", catalog_filter)
            .Like("k.schema_name", schema_filter)
            .Like("k.table_name", table_filter)
            .Run(conn_, "1, 2, 3, 4, k.i", [&](const PqResultRow& row) -> Status {
              int64_t index;
              UNWRAP_RESULT(index, row[3].ParseInteger());
              std::vector<ConstraintRow>& constraints =
                  constraints_[{Text(row[0]), Text(row[1]), Text(row[2])}];
              // Rows for a constraint's columns are consecutive.
              if (constraints.empty() || constraints.back().index != index) {
                constraints.push_back({index,
                                       Text(row[4]),
                                       Text(row[5]),
                                       {},
                                       NullableText(row[7]).value_or(""),
                                       {}});
              }
              if (!row[6].is_null) {
                constraints.back().column_names.push_back(Text(row[6]));
              }
              if (!row[8].is_null) {
                constraints.back().referenced_column_names.push_back(Text(row[8]));
              }
              return Status::Ok();
            }));

    if (column_filter) KeepConstraintsOnListedColumns();
    return Status::Ok();
  }

  // Load() applied the filters, so these only select the rows to step through.

  Status LoadCatalogs(std::optional<std::string_view>) override {
    next_catalog_ = 0;
    return Status::Ok();
  }

  Result<std::optional<std::string_view>> NextCatalog() override {
    if (next_catalog_ >= catalogs_.size()) return std::nullopt;
    return std::string_view(catalogs_[next_catalog_++]);
  }

  Status LoadSchemas(std::string_view catalog, std::optional<std::string_view>) override {
    schemas_cursor_.Reset(schemas_, std::string(catalog));
    return Status::Ok();
  }

  Result<std::optional<std::string_view>> NextSchema() override {
    const std::string* schema = schemas_cursor_.Next();
    if (schema == nullptr) return std::nullopt;
    return std::string_view(*schema);
  }

  Status LoadTables(std::string_view catalog, std::string_view schema,
                    std::optional<std::string_view>,
                    const std::vector<std::string_view>&) override {
    tables_cursor_.Reset(tables_, SchemaKey(catalog, schema));
    return Status::Ok();
  }

  Result<std::optional<Table>> NextTable() override {
    const TableRow* table = tables_cursor_.Next();
    if (table == nullptr) return std::nullopt;
    return Table{table->name, table->type};
  }

  Status LoadColumns(std::string_view catalog, std::string_view schema,
                     std::string_view table, std::optional<std::string_view>) override {
    current_table_ = TableKey(catalog, schema, table);
    columns_cursor_.Reset(columns_, current_table_);
    constraints_cursor_.Reset(constraints_, current_table_);
    return Status::Ok();
  }

  Result<std::optional<Column>> NextColumn() override {
    const ColumnRow* row = columns_cursor_.Next();
    if (row == nullptr) return std::nullopt;
    Column column;
    column.column_name = row->name;
    column.ordinal_position = row->ordinal_position;
    if (row->remarks) column.remarks = *row->remarks;
    column.xdbc = ColumnXdbc();
    column.xdbc->xdbc_type_name = row->type_name;
    return column;
  }

  Result<std::optional<Constraint>> NextConstraint() override {
    const ConstraintRow* row = constraints_cursor_.Next();
    if (row == nullptr) return std::nullopt;
    Constraint constraint;
    constraint.name = row->name;
    constraint.type = row->type;
    constraint.column_names.assign(row->column_names.begin(), row->column_names.end());
    if (row->type == "FOREIGN KEY") {
      // DuckDB foreign keys reference a table in the same schema.
      constraint.usage.emplace();
      for (const std::string& column : row->referenced_column_names) {
        constraint.usage->push_back({std::get<0>(current_table_),
                                     std::get<1>(current_table_), row->referenced_table,
                                     column});
      }
    }
    return constraint;
  }

 private:
  // With a column filter, keeps only the constraints on a column it matched.
  void KeepConstraintsOnListedColumns() {
    for (auto& [table, constraints] : constraints_) {
      std::set<std::string_view> listed;
      auto columns = columns_.find(table);
      if (columns != columns_.end()) {
        for (const ColumnRow& column : columns->second) listed.insert(column.name);
      }
      std::vector<ConstraintRow> kept;
      for (ConstraintRow& constraint : constraints) {
        for (const std::string& column : constraint.column_names) {
          if (listed.count(column) > 0) {
            kept.push_back(std::move(constraint));
            break;
          }
        }
      }
      constraints = std::move(kept);
    }
  }

  PGconn* conn_;

  std::vector<std::string> catalogs_;
  std::map<std::string, std::vector<std::string>> schemas_;
  std::map<SchemaKey, std::vector<TableRow>> tables_;
  std::map<TableKey, std::vector<ColumnRow>> columns_;
  std::map<TableKey, std::vector<ConstraintRow>> constraints_;

  size_t next_catalog_ = 0;
  Cursor<std::string> schemas_cursor_;
  Cursor<TableRow> tables_cursor_;
  TableKey current_table_;
  Cursor<ColumnRow> columns_cursor_;
  Cursor<ConstraintRow> constraints_cursor_;
};

}  // namespace

std::unique_ptr<GetObjectsHelper> MakeMotherduckGetObjectsHelper(PGconn* conn) {
  return std::make_unique<MotherduckGetObjectsHelper>(conn);
}

}  // namespace adbcpq
