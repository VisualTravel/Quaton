#include "quaton/database/transaction.h"

#include "quaton/logger.h"

namespace Quaton {
namespace Database {

bool BeginTransaction(sqlite3* db) {
  if (!db) {
    LOG_ERROR("Database handle is null");
    return false;
  }

  char* err_msg = nullptr;
  int rc = sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, &err_msg);

  if (rc != SQLITE_OK) {
    LOG_ERROR("Failed to begin transaction: %s",
              err_msg ? err_msg : "Unknown error");
    if (err_msg) {
      sqlite3_free(err_msg);
    }
    return false;
  }

  return true;
}

bool CommitTransaction(sqlite3* db) {
  if (!db) {
    LOG_ERROR("Database handle is null");
    return false;
  }

  char* err_msg = nullptr;
  int rc = sqlite3_exec(db, "COMMIT;", nullptr, nullptr, &err_msg);

  if (rc != SQLITE_OK) {
    LOG_ERROR("Failed to commit transaction: %s",
              err_msg ? err_msg : "Unknown error");
    if (err_msg) {
      sqlite3_free(err_msg);
    }
    return false;
  }

  return true;
}

bool RollbackTransaction(sqlite3* db) {
  if (!db) {
    LOG_ERROR("Database handle is null");
    return false;
  }

  char* err_msg = nullptr;
  int rc = sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, &err_msg);

  if (rc != SQLITE_OK) {
    LOG_ERROR("Failed to rollback transaction: %s",
              err_msg ? err_msg : "Unknown error");
    if (err_msg) {
      sqlite3_free(err_msg);
    }
    return false;
  }

  return true;
}

Transaction::Transaction(sqlite3* db)
    : db_(db), active_(false), committed_(false) {
  if (db_) {
    active_ = BeginTransaction(db_);
  }
}

Transaction::~Transaction() {
  if (active_ && !committed_) {
    RollbackTransaction(db_);
  }
}

bool Transaction::Commit() {
  if (!active_) {
    LOG_ERROR("Transaction not active");
    return false;
  }

  if (committed_) {
    LOG_WARN("Transaction already committed");
    return true;
  }

  if (!CommitTransaction(db_)) {
    return false;
  }

  committed_ = true;
  active_ = false;
  return true;
}

bool Transaction::Rollback() {
  if (!active_) {
    LOG_ERROR("Transaction not active");
    return false;
  }

  if (committed_) {
    LOG_WARN("Transaction already committed, cannot rollback");
    return false;
  }

  if (!RollbackTransaction(db_)) {
    return false;
  }

  active_ = false;
  return true;
}

}  // namespace Database
}  // namespace Quaton
