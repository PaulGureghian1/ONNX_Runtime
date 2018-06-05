#pragma once
#include <string>
#include <vector>
#include <core/platform/env_time.h>
#include <cstring>

//result of a single test run: 1 model with 1 test dataset
enum class EXECUTE_RESULT {
  SUCCESS = 0,
  UNKNOWN_ERROR = -1,
  WITH_EXCEPTION = -2,
  RESULT_DIFFERS = -3,
  SHAPE_MISMATCH = -4,
  TYPE_MISMATCH = -5,
  NOT_SUPPORT = -6,
  LOAD_MODEL_FAILED = -7,
  INVALID_GRAPH = -8,
  INVALID_ARGUMENT = -9,
};

struct TestCaseResult {
 public:
  TestCaseResult(size_t count, EXECUTE_RESULT result, const std::string& node_name1) : excution_result_(count, result), node_name(node_name1) {
    Lotus::SetTimeSpecToZero(&spent_time_);
  }

  void SetResult(size_t task_id, EXECUTE_RESULT r);

  std::vector<EXECUTE_RESULT> GetExcutionResult() const {
    return excution_result_;
  }

  Lotus::TIME_SPEC GetSpentTime() const {
    return spent_time_;
  }

  void SetSpentTime(const Lotus::TIME_SPEC& input) const {
    memcpy((void*)&spent_time_, &input, sizeof(input));
  }

  //only valid for single node tests;
  const std::string node_name;

 private:
  Lotus::TIME_SPEC spent_time_;
  std::vector<EXECUTE_RESULT> excution_result_;
};