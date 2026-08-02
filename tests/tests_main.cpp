/**
 * @file tests/tests_main.cpp
 * @brief Entry point definition.
 */
#include "tests_common.h"
#include "tests_environment.h"
#include "tests_events.h"

#include <filesystem>
#include <iostream>

namespace {
  bool remove_stale_coverage_data() {
    std::error_code error;
    const std::filesystem::path test_build_directory {SUNSHINE_TEST_BIN_DIR};

    for (std::filesystem::recursive_directory_iterator entry {test_build_directory, error}, end;
         entry != end && !error;
         entry.increment(error)) {
      if (entry->is_regular_file() && entry->path().extension() == ".gcda") {
        std::filesystem::remove(entry->path(), error);
        if (error) {
          break;
        }
      }
    }

    if (error) {
      std::cerr << "Failed to clean stale coverage data from " << test_build_directory << ": " << error.message() << '\n';
      return false;
    }

    return true;
  }
}  // namespace

int main(int argc, char **argv) {
  if (!remove_stale_coverage_data()) {
    return 1;
  }

  testing::InitGoogleTest(&argc, argv);
  testing::AddGlobalTestEnvironment(new SunshineEnvironment);
  testing::UnitTest::GetInstance()->listeners().Append(new SunshineEventListener);
  return RUN_ALL_TESTS();
}
