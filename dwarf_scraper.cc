#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDebug>
#include <QLoggingCategory>
#include <QThreadPool>
#include <QtLogging>

#include "flat_layout_scraper.hh"
#include "pool.hh"
#include "scraper.hh"
#include "utils.hh"

namespace fs = std::filesystem;

namespace {

/**
 * Maps command line arguments to an internal identifier for
 * a specific scraper.
 */
enum class ScraperID { FlatLayout, Unset };

std::ostream &operator<<(std::ostream &os, const ScraperID &value) {
  switch (value) {
  case ScraperID::FlatLayout:
    os << "flat-layout";
    break;
  default:
    os << "<unknown-scraper>";
  }
  return os;
}

/**
 * Helper context for the scraping session
 */
class Driver {
public:
  Driver(unsigned long workers, fs::path db_file,
         std::optional<std::string> path_strip_prefix)
      : pool_(workers), sm_(db_file), strip_prefix_(path_strip_prefix) {}

  void addTarget(fs::path target, ScraperID scraper_id) {
    auto source = std::make_unique<cheri::DwarfSource>(target);
    std::unique_ptr<cheri::DwarfScraper> scraper;
    switch (scraper_id) {
    case ScraperID::FlatLayout:
      scraper =
          std::make_unique<cheri::FlatLayoutScraper>(sm_, std::move(source));
      break;
    default:
      qCritical() << "Unexpected scraper ID";
      throw std::invalid_argument("Invalid value for scraper_id");
    }
    scraper->setStripPrefix(strip_prefix_);
    results_.emplace_back(pool_.schedule(std::move(scraper)));
  }

  void waitComplete() { pool_.wait(); }

  bool report() {
    int has_error = false;
    for (auto &fut : results_) {
      auto result = fut.get();
      qInfo() << result;
      has_error = has_error || (result.errors.size() > 0);
      for (auto &err : result.errors) {
        qCritical() << "Reason: " << err;
      }
    }
    return has_error;
  }

private:
  /* Thread pool where work is submitted */
  cheri::ThreadPool pool_;
  /* Vector of future results */
  std::vector<std::future<cheri::ScraperResult>> results_;
  /* Storage manager */
  cheri::StorageManager sm_;
  /* File path prefix to strip */
  std::optional<std::string> strip_prefix_;
};

} // namespace

int main(int argc, char **argv) {
  using namespace cheri;

  QCoreApplication app(argc, argv);
  QCoreApplication::setApplicationName("dwarf-scraper");
  QCoreApplication::setApplicationVersion("1.0");

  QCommandLineParser parser;
  parser.setApplicationDescription("CHERI debug info scraping tool");
  auto help = parser.addHelpOption();
  auto version = parser.addVersionOption();
  QCommandLineOption verbose("verbose", "Enable verbose output");
  parser.addOption(verbose);

  QCommandLineOption clean("clean", "Wipe the database clean before running");
  parser.addOption(clean);

  QCommandLineOption prefix(
      "prefix", "Path prefix to strip from the source file paths", "PREFIX");
  parser.addOption(prefix);

  QCommandLineOption threads("threads", "Use specified number of threads",
                             "THREADS");
  threads.setDefaultValue(QString::number(std::thread::hardware_concurrency()));
  parser.addOption(threads);

  QCommandLineOption database("database",
                              "Database file to store the information "
                              "(defaults to cheri-dwarf.sqlite)",
                              "PATH");
  database.setDefaultValue("cheri-dwarf.sqlite");
  parser.addOption(database);

  QCommandLineOption input_path(QStringList() << "i" << "input",
                                "Specify input file path", "PATH");
  parser.addOption(input_path);

  QCommandLineOption read_input("read-input", "Read input files list from file",
                                "PATH");
  parser.addOption(read_input);

  parser.addPositionalArgument(
      "scraper", "Select scraper to run. Valid values are 'flat-layout'");

  parser.process(app);

  if (parser.isSet(version)) {
    parser.showVersion();
    return 0;
  }

  if (parser.isSet(help)) {
    parser.showHelp(0);
  }

  if (!parser.isSet(verbose)) {
    QLoggingCategory::setFilterRules("*.debug=false\n");
  }

  auto args = parser.positionalArguments();
  if (args.count() < 1) {
    qCritical() << "Missing positional argument 'scraper'";
    parser.showHelp(1);
  }
  auto scraper_name = args.at(0);
  ScraperID scraper_id = ScraperID::Unset;
  if (scraper_name == "flat-layout") {
    scraper_id = ScraperID::FlatLayout;
  }
  if (scraper_id == ScraperID::Unset) {
    qCritical() << "Invalid scraper name '" << scraper_name << "'"
                << "Must be one of {'flat-layout'}";
    parser.showHelp(1);
  }

  bool ok;
  int opt_workers = parser.value(threads).toInt(&ok);
  if (!ok) {
    qCritical() << "Invalid value for option --threads, must be an integer:"
                << parser.value(threads);
    parser.showHelp(/*exitCode=*/1);
  }

  auto opt_database = fs::path(parser.value(database).toStdString());
  if (parser.isSet(clean)) {
    qDebug() << "Wiping database" << opt_database;
    if (fs::exists(opt_database)) {
      fs::remove(opt_database);
    }
  }

  std::optional<std::string> opt_prefix;
  if (parser.isSet(prefix)) {
    opt_prefix = parser.value(prefix).toStdString();
  }

  qDebug() << "Initialize thread pool with" << opt_workers << "workers";
  Driver ctx(opt_workers, opt_database, opt_prefix);

  if (parser.isSet(read_input)) {
    auto input_list = fs::path(parser.value(read_input).toStdString());
    qDebug() << "Reading target files from" << input_list;
    std::ifstream target_stream(input_list);
    std::string target;
    while (std::getline(target_stream, target)) {
      ctx.addTarget(target, scraper_id);
    }
    target_stream.close();
  } else if (parser.isSet(input_path)) {
    qDebug() << "Reading target files from --input args";
    auto input_list = parser.values(input_path);
    for (auto path : input_list) {
      ctx.addTarget(path.toStdString(), scraper_id);
    }
  } else {
    qCritical()
        << "No input methods specified, use either --input or --read-input.";
  }
  ctx.waitComplete();
  bool has_error = ctx.report();

  return has_error;
}
