// Thin RAII wrapper around SQLite plus the project schema. Tables are laid
// out as time series (Unix-second timestamps) so a plotting front end can
// query ranges directly.
#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

struct sqlite3;
struct sqlite3_stmt;

namespace store {

class Db {
 public:
  Db() = default;
  ~Db();
  Db(const Db&) = delete;
  Db& operator=(const Db&) = delete;

  // Opens (creating if needed) and applies the schema.
  bool open(const std::filesystem::path& path, std::string& err);
  void close();
  bool is_open() const { return db_ != nullptr; }

  bool exec(const char* sql, std::string& err);
  bool begin(std::string& err) { return exec("BEGIN", err); }
  bool commit(std::string& err) { return exec("COMMIT", err); }
  void rollback();

  int64_t last_insert_rowid() const;
  int changes() const;
  std::string last_error() const;
  sqlite3* raw() const { return db_; }

 private:
  sqlite3* db_ = nullptr;
};

// Prepared statement. Bind indices are 1-based (SQLite convention).
class Stmt {
 public:
  Stmt(Db& db, const char* sql);
  ~Stmt();
  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;

  bool ok() const { return stmt_ != nullptr; }

  Stmt& bind(int idx, int64_t v);
  Stmt& bind(int idx, int v) { return bind(idx, static_cast<int64_t>(v)); }
  Stmt& bind(int idx, double v);
  Stmt& bind(int idx, const std::string& v);
  Stmt& bind_null(int idx);
  Stmt& bind(int idx, const std::optional<int64_t>& v);
  Stmt& bind(int idx, const std::optional<double>& v);
  Stmt& bind(int idx, const std::optional<std::string>& v);

  // Runs a statement that returns no rows, then resets it for reuse.
  bool run(std::string& err);
  // Steps a query; true while a row is available.
  bool row();
  void reset();

  int64_t col_int(int i) const;
  double col_double(int i) const;
  std::string col_text(int i) const;
  bool col_null(int i) const;

 private:
  Db& db_;
  sqlite3_stmt* stmt_ = nullptr;
};

}  // namespace store
