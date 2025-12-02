/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2023-2025 Alfredo Mazzinghi
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#pragma once

#include <filesystem>
#include <mutex>
#include <sstream>
#include <stdexcept>

#include <QLoggingCategory>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

namespace cheri {

Q_DECLARE_LOGGING_CATEGORY(storage)

class DBError : public std::runtime_error {
public:
  DBError(QSqlError err)
      : std::runtime_error(
            (err.nativeErrorCode() + ": " + err.text()).toStdString()),
        error(err) {}

  QSqlError error;
};

/**
 * Manage database interface for a scraper.
 */
class StorageManager {
public:
  StorageManager(std::filesystem::path db_path);
  StorageManager(const StorageManager &s) = delete;
  ~StorageManager();

  /**
   * Get the database connection instance for the current worker thread.
   * Using different connections for each thread should allow to run
   * sqlite in the multithreaded mode.
   * See https://www.sqlite.org/threadsafe.html
   */
  QSqlDatabase &getWorkerStorage();

  QSqlQuery query(const std::string &expr);
  QSqlQuery prepare(const std::string &expr);
  void transaction(std::function<void(StorageManager &sm)> fn);

private:
  std::mutex transaction_mutex_;
  std::filesystem::path db_path_;
};

} /* namespace cheri */
