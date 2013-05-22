// Copyright (c) 2013, Cloudera, inc.
//
// Base test class, with various utility functions.
#ifndef KUDU_UTIL_TEST_UTIL_H
#define KUDU_UTIL_TEST_UTIL_H

#include <gtest/gtest.h>
#include "util/env.h"
#include "util/test_macros.h"
#include "gutil/gscoped_ptr.h"
#include "gutil/stringprintf.h"
#include "gutil/strings/util.h"

DECLARE_bool(test_leave_files);

namespace kudu {

class KuduTest : public ::testing::Test {
public:
  KuduTest() :
    env_(new EnvWrapper(Env::Default()))
  {}

  // env passed in from subclass, for tests that run in-memory
  explicit KuduTest(Env *env) :
    env_(env)
  {}

  virtual void SetUp() {
    const ::testing::TestInfo* const test_info =
      ::testing::UnitTest::GetInstance()->current_test_info();

    env_->GetTestDirectory(&test_dir_);

    test_dir_ += StringPrintf(
      "/%s.%s.%ld",
      StringReplace(test_info->test_case_name(), "/", "_", true).c_str(),
      test_info->name(),
      time(NULL));

    ASSERT_STATUS_OK(env_->CreateDir(test_dir_));
  }

  virtual void TearDown() {
    if (FLAGS_test_leave_files) {
      LOG(INFO) << "-----------------------------------------------";
      LOG(INFO) << "--test_leave_files specified, leaving files in " << test_dir_;
    } else if (HasFatalFailure()) {
      LOG(INFO) << "-----------------------------------------------";
      LOG(INFO) << "Had fatal failures, leaving test files at " << test_dir_;
    } else {
      env_->DeleteRecursively(test_dir_);
    }
  }

protected:
  string GetTestPath(const string &relative_path) {
    CHECK(!test_dir_.empty()) << "Call SetUp() first";
    return env_->JoinPathSegments(test_dir_, relative_path);
  }

  gscoped_ptr<Env> env_;
  string test_dir_;
};

} // namespace kudu
#endif
