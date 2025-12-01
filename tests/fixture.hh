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

#include <QCoreApplication>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>

namespace cheri {

// Common in-memory storage initialization
class TestStorage : public ::testing::Test {
protected:
  std::unique_ptr<DwarfScraper> setupScraper(std::filesystem::path src) {
    auto source = std::make_unique<DwarfSource>(src);
    return std::make_unique<FlatLayoutScraper>(*sm_, std::move(source));
  }

  ScraperResult execScraper(DwarfScraper *scraper) {
    std::stop_source dummy_stop_src;
    scraper->initSchema();
    scraper->run(dummy_stop_src.get_token());
    return scraper->result();
  }

  ssize_t selectedRows(QSqlQuery &q) {
    ssize_t count = -1;
    if (q.last()) {
      count = q.at() + 1;
      q.first();
      q.previous();
    }
    return count;
  }

  void SetUp() override {
    int dummy_argc = 0;
    app_ = std::make_unique<QCoreApplication>(dummy_argc, nullptr);
    QCoreApplication::setApplicationName("dwarf-scanner-test");
    QCoreApplication::setApplicationVersion("1.0");

    std::filesystem::path dummy(":memory:");
    sm_ = std::make_unique<StorageManager>(dummy);
  }

  std::unique_ptr<QCoreApplication> app_;
  std::unique_ptr<StorageManager> sm_;
};

} // namespace cheri
