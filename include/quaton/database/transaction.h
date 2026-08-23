#ifndef QUATON_DATABASE_TRANSACTION_H
#define QUATON_DATABASE_TRANSACTION_H
#pragma once

#include <sqlite3.h>

#include <functional>

namespace Quaton {
namespace Database {

/**
 * @brief Begin database transaction
 * @param db SQLite database handle
 * @return true if transaction starts successfully, false otherwise
 */
bool BeginTransaction(sqlite3* db);

/**
 * @brief Commit database transaction
 * @param db SQLite database handle
 * @return true if transaction commits successfully, false otherwise
 */
bool CommitTransaction(sqlite3* db);

/**
 * @brief Rollback database transaction
 * @param db SQLite database handle
 * @return true if transaction rolls back successfully, false otherwise
 */
bool RollbackTransaction(sqlite3* db);

/**
 * @class Transaction
 * @brief RAII transaction guard
 */
class Transaction {
 public:
  /**
   * @brief Constructor - begins transaction
   * @param db SQLite database handle
   */
  explicit Transaction(sqlite3* db);

  /**
   * @brief Destructor - rolls back if not committed
   */
  ~Transaction();

  // Disable copy operations
  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;

  /**
   * @brief Commit transaction
   * @return true if commit succeeds, false otherwise
   */
  bool Commit();

  /**
   * @brief Rollback transaction
   * @return true if rollback succeeds, false otherwise
   */
  bool Rollback();

  /**
   * @brief Check if transaction is active
   * @return true if transaction is active, false otherwise
   */
  bool IsActive() const { return active_; }

 private:
  sqlite3* db_;     ///< Database handle
  bool active_;     ///< Transaction active flag
  bool committed_;  ///< Transaction committed flag
};

/**
 * @brief Execute function within transaction
 * @tparam Func Function type
 * @param db SQLite database handle
 * @param func Function to execute (should return bool for success)
 * @return true if transaction succeeds, false otherwise
 */
template <typename Func>
bool ExecuteInTransaction(sqlite3* db, Func&& func) {
  Transaction txn(db);
  if (!txn.IsActive()) {
    return false;
  }

  if (!func()) {
    return false;
  }

  return txn.Commit();
}

}  // namespace Database
}  // namespace Quaton

#endif  // QUATON_DATABASE_TRANSACTION_H
