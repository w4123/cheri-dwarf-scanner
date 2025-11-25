#pragma once

#include <concepts>
#include <filesystem>
#include <future>

#include <QDebug>
#include <QThreadPool>
#include <QtLogging>

#include "scraper.hh"

namespace cheri {

/**
 * Thread pool interface.
 *
 * An simple thread pool that supports graceful shutdown.
 */
class ThreadPool {
public:
  explicit ThreadPool(unsigned long workers) {
    pool_.setMaxThreadCount(workers);
  }

  std::future<ScraperResult> schedule(std::unique_ptr<DwarfScraper> scraper) {
    std::promise<ScraperResult> promise;
    auto result = promise.get_future();
    auto token = stop_state_.get_token();

    pool_.start([s = std::move(scraper), p = std::move(promise),
                 token]() mutable {
      try {
        s->initSchema();
        s->run(token);
        qInfo() << "Scraper" << s->name() << "completed job for"
                << s->source().getPath().string();
        p.set_value(s->result());
      } catch (std::exception &ex) {
        qCritical() << "DWARF scraper failed for"
                    << s->source().getPath().string() << "reason " << ex.what();
        p.set_exception(std::current_exception());
      }
    });
    return result;
  }

  void wait() { pool_.waitForDone(); }

  void cancel() {
    pool_.clear();
    stop_state_.request_stop();
  }

private:
  std::stop_source stop_state_;
  QThreadPool pool_;
};

} /* namespace cheri */
