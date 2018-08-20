#include "core/session/inference_session.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <thread>
#include <fstream>

#include "core/common/logging/logging.h"
#include "core/common/profiler.h"
#include "core/framework/execution_provider.h"
#include "core/framework/op_kernel.h"
#include "core/framework/session_state.h"
#include "core/graph/graph.h"
#include "core/graph/model.h"
#include "core/graph/op.h"
#include "core/providers/cpu/cpu_execution_provider.h"
#include "core/providers/cpu/math/element_wise_ops.h"
#include "core/framework/tensorprotoutils.h"
#include "core/session/IOBinding.h"

#include "test/capturing_sink.h"
#include "test/test_environment.h"
#include "test_utils.h"
#include "gtest/gtest.h"

using namespace std;
using namespace onnx;
using namespace ::Lotus::Logging;
using namespace LotusIR;

namespace Lotus {
namespace Test {
static bool Compare(const InputDefList& f_arg, const InputDefList& s_arg);
static void VerifyOutputs(const std::vector<MLValue>& fetches,
                          const std::vector<int64_t>& expected_dims,
                          const std::vector<float>& expected_values);
static const std::string MODEL_URI = "testdata/mul_1.pb";
static const std::string MODEL_URI_NO_OPSET = "testdata/mul_1.pb.noopset";
//static const std::string MODEL_URI = "./testdata/squeezenet/model.onnx"; // TODO enable this after we've weights?

static void CreateMatMulModel(std::unique_ptr<LotusIR::Model>& p_model, ProviderType provider_type) {
  // Generate the input & output def lists
  p_model = std::make_unique<LotusIR::Model>("test");
  LotusIR::Graph& graph = p_model->MainGraph();

  TypeProto tensor_float;
  tensor_float.mutable_tensor_type()->set_elem_type(TensorProto_DataType_FLOAT);

  std::vector<LotusIR::NodeArg*> input_defs;
  auto& input_arg_a = graph.GetOrCreateNodeArg("A", &tensor_float);
  input_defs.push_back(&input_arg_a);

  auto& input_arg_b = graph.GetOrCreateNodeArg("B", &tensor_float);
  input_defs.push_back(&input_arg_b);

  std::vector<LotusIR::NodeArg*> output_defs;
  auto& output_arg = graph.GetOrCreateNodeArg("Y", &tensor_float);
  output_defs.push_back(&output_arg);

  // Create a simple model
  auto& node = *graph.AddNode("node1", "MatMul", "MatMul", input_defs, output_defs, nullptr, LotusIR::kOnnxDomain);
  if (provider_type == kCpuExecutionProvider) {
    node.SetExecutionProviderType(provider_type);
  } else {
#ifdef USE_CUDA
    node.SetExecutionProviderType(provider_type);
#endif
  }
  Status status = graph.Resolve();
  ASSERT_TRUE(status.IsOK());
}

void VerifyOutputs(const std::vector<MLValue>& fetches,
                   const std::vector<int64_t>& expected_dims,
                   const std::vector<float>& expected_values) {
  ASSERT_EQ(1, fetches.size());
  auto& rtensor = fetches.front().Get<Tensor>();
  TensorShape expected_shape(expected_dims);
  ASSERT_EQ(expected_shape, rtensor.Shape());
  const std::vector<float> found(rtensor.Data<float>(), rtensor.Data<float>() + expected_values.size());
  ASSERT_EQ(expected_values, found);
}

void RunModel(InferenceSession& session_object,
              const RunOptions& run_options,
              bool is_preallocate_output_vec = false) {
  // prepare inputs
  std::vector<int64_t> dims_mul_x = {3, 2};
  std::vector<float> values_mul_x = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
  MLValue ml_value;
  CreateMLValue<float>(TestCPUExecutionProvider()->GetAllocator(), dims_mul_x, values_mul_x, &ml_value);
  NameMLValMap feeds;
  feeds.insert(std::make_pair("X", ml_value));

  // prepare outputs
  std::vector<std::string> output_names;
  output_names.push_back("Y");
  std::vector<MLValue> fetches;

  if (is_preallocate_output_vec) {
    fetches.resize(output_names.size());
    for (auto& elem : fetches) {
      CreateMLValue<float>(TestCPUExecutionProvider()->GetAllocator(), dims_mul_x, values_mul_x, &elem);
    }
  }

  // prepare expected inputs and outputs
  std::vector<int64_t> expected_dims_mul_y = {3, 2};
  std::vector<float> expected_values_mul_y = {1.0f, 4.0f, 9.0f, 16.0f, 25.0f, 36.0f};

  // Now run
  Common::Status st = session_object.Run(run_options, feeds, output_names, &fetches);
  if (!st.IsOK()) {
    std::cout << "Run returned status: " << st.ErrorMessage() << std::endl;
  }
  ASSERT_TRUE(st.IsOK());
  VerifyOutputs(fetches, expected_dims_mul_y, expected_values_mul_y);
}

void RunModelWithBindingMatMul(InferenceSession& session_object,
                               const RunOptions& run_options,
                               ProviderType bind_provider_type,
                               bool is_preallocate_output_vec = false,
                               ProviderType allocation_provider = kCpuExecutionProvider) {
  unique_ptr<IOBinding> io_binding;
  Status st = session_object.NewIOBinding(&io_binding);
  ASSERT_TRUE(st.IsOK());
  auto input_allocator = io_binding->GetCPUAllocator(bind_provider_type);

  // prepare inputs
  std::vector<float> values_mul_x = {0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f, 10.0f, 11.0f};
  /*
      0 1 2 3     0 1 2
      4 5 6 7     3 4 5
      8 9 10 11   6 7 8
      9 10 11
      */
  // bind one input to cpu allocator from bind_provider_type, and another on user provided CPU memory
  // so both code pathes are covered
  MLValue input_ml_value_A;
  std::vector<int64_t> dims_mul_x_A = {3, 4};
  CreateMLValue<float>(input_allocator, dims_mul_x_A, values_mul_x, &input_ml_value_A);

  MLValue input_ml_value_B;
  std::vector<int64_t> dims_mul_x_B = {4, 3};
  CreateMLValue<float>(TestCPUExecutionProvider()->GetAllocator(), dims_mul_x_B, values_mul_x, &input_ml_value_B);

  io_binding->BindInput("A", input_ml_value_A);
  io_binding->BindInput("B", input_ml_value_B);

  // prepare outputs
  std::vector<int64_t> expected_output_dims = {3, 3};
  MLValue output_ml_value;
  if (is_preallocate_output_vec) {
    if (allocation_provider == kCpuExecutionProvider) {
      AllocateMLValue<float>(TestCPUExecutionProvider()->GetAllocator(), expected_output_dims, &output_ml_value);
    } else if (allocation_provider == kCudaExecutionProvider) {
#ifdef USE_CUDA
      AllocateMLValue<float>(TestCudaExecutionProvider()->GetAllocator(), expected_output_dims, &output_ml_value);
#endif
    } else {
      LOTUS_THROW("Unsupported provider");
    }
  }
  io_binding->BindOutput("Y", output_ml_value);
  ASSERT_TRUE(io_binding->SynchronizeInputs().IsOK());

  // prepare expected inputs and outputs
  std::vector<float> expected_values_mul_y = {42, 48, 54, 114, 136, 158, 186, 224, 262};

  // Now run
  st = session_object.Run(run_options, *io_binding.get());

  std::cout << "Run returned status: " << st.ErrorMessage() << std::endl;
  ASSERT_TRUE(st.IsOK());

  if (is_preallocate_output_vec &&
      allocation_provider == kCudaExecutionProvider) {
#ifdef USE_CUDA
    // in this case we need to copy the tensor from cuda to cpu
    vector<MLValue>& outputs = io_binding->GetOutputs();
    ASSERT_EQ(1, outputs.size());
    auto& rtensor = outputs.front().Get<Tensor>();
    auto element_type = rtensor.DataType();
    auto& shape = rtensor.Shape();
    auto cpu_allocator = TestCPUExecutionProvider()->GetAllocator();
    void* buffer = cpu_allocator->Alloc(element_type->Size() * shape.Size());
    LOTUS_ENFORCE(buffer);
    std::unique_ptr<Tensor> cpu_tensor = std::make_unique<Tensor>(element_type,
                                                                  shape,
                                                                  buffer,
                                                                  cpu_allocator->Info(),
                                                                  cpu_allocator);
    st = TestCudaExecutionProvider()->CopyTensor(rtensor, *cpu_tensor.get());
    ASSERT_TRUE(st.IsOK());
    MLValue ml_value;
    ml_value.Init(cpu_tensor.release(),
                  DataTypeImpl::GetType<Tensor>(),
                  DataTypeImpl::GetType<Tensor>()->GetDeleteFunc());
    VerifyOutputs({ml_value}, expected_output_dims, expected_values_mul_y);
#endif
  } else {
    if (allocation_provider == kCudaExecutionProvider) {
#ifdef USE_CUDA
      TestCudaExecutionProvider()->Sync();
#endif
    }
    VerifyOutputs(io_binding->GetOutputs(), expected_output_dims, expected_values_mul_y);
  }
}

TEST(InferenceSessionTests, NoTimeout) {
  SessionOptions so;

  so.session_logid = "InferenceSessionTests.NoTimeout";

  InferenceSession session_object{so, &DefaultLoggingManager()};
  ASSERT_TRUE(session_object.Load(MODEL_URI).IsOK());
  ASSERT_TRUE(session_object.Initialize().IsOK());

  RunOptions run_options;
  run_options.run_tag = "one session/one tag";
  RunModel(session_object, run_options);
}

TEST(InferenceSessionTests, DisableCPUArena) {
  SessionOptions so;

  so.session_logid = "InferenceSessionTests.DisableCPUArena";
  so.enable_cpu_mem_arena = false;

  InferenceSession session_object{so, &DefaultLoggingManager()};
  ASSERT_TRUE(session_object.Load(MODEL_URI).IsOK());
  ASSERT_TRUE(session_object.Initialize().IsOK());

  RunOptions run_options;
  run_options.run_tag = "one session/one tag";
  RunModel(session_object, run_options);
}

static bool Compare(const InputDefList& f_arg, const InputDefList& s_arg) {
  if (f_arg.size() != s_arg.size()) {
    cout << "Sizes differ: f_arg size: " << f_arg.size() << " s_arg size: " << s_arg.size() << endl;
    return false;
  }

  for (auto i = 0; i < f_arg.size(); ++i) {
    const LotusIR::NodeArg* x = f_arg[i];
    const LotusIR::NodeArg* y = s_arg[i];
    if ((x->Shape() == nullptr) ^ (y->Shape() == nullptr)) {
      return false;
    } else {
      if (!x->Shape()) {
        continue;
      } else {
        vector<int64_t> x_shape = Utils::GetTensorShapeFromTensorShapeProto(*x->Shape());
        vector<int64_t> y_shape = Utils::GetTensorShapeFromTensorShapeProto(*y->Shape());
        if (x->Name() == y->Name() && x_shape == y_shape && *x->Type() == *y->Type()) {
          continue;
        } else {
          return false;
        }
      }
    }
  }

  return true;
}

TEST(InferenceSessionTests, ModelMetadata) {
  SessionOptions so;

  so.session_logid = "InferenceSessionTests.ModelMetadata";
  InferenceSession session_object{so, &DefaultLoggingManager()};
  string model_uri = "testdata/squeezenet/model.onnx";
  ASSERT_TRUE(session_object.Load(model_uri).IsOK());

  std::shared_ptr<LotusIR::Model> p_model;
  Status st = LotusIR::Model::Load(model_uri, p_model);
  ASSERT_TRUE(st.IsOK());
  const LotusIR::Graph& graph = p_model->MainGraph();

  // 1. first test the model meta
  {
    auto retval = session_object.GetModelMetadata();
    ASSERT_TRUE(retval.first.IsOK());
    const ModelMetadata* m = retval.second;
    ASSERT_TRUE(m->custom_metadata_map == p_model->MetaData() &&
                m->description == p_model->DocString() &&
                m->domain == p_model->Domain() &&
                m->graph_name == graph.Name() &&
                m->producer_name == p_model->ProducerName() &&
                m->version == p_model->ModelVersion());
  }

  {
    // 2. test inputs
    auto& inputs = graph.GetInputs();
    auto weights = graph.GetAllInitializedTensors();

    // skip the weights
    InputDefList inputs_no_weights;
    for (auto& elem : inputs) {
      if (weights.find(elem->Name()) != weights.end()) {
        continue;
      } else {
        inputs_no_weights.push_back(elem);
      }
    }

    auto retval = session_object.GetInputs();
    cout << "weights size: " << weights.size()
         << " inputs.size(): " << inputs.size()
         << " from session: " << retval.second->size() << endl;
    ASSERT_TRUE(retval.first.IsOK());
    ASSERT_TRUE(Compare(inputs_no_weights, *retval.second));
  }

  // 3. test outputs
  {
    auto retval = session_object.GetOutputs();
    ASSERT_TRUE(retval.first.IsOK());

    auto& outputs = graph.GetOutputs();
    retval = session_object.GetOutputs();
    ASSERT_TRUE(retval.first.IsOK());
    ASSERT_TRUE(Compare(outputs, *retval.second));
  }
}

TEST(InferenceSessionTests, CheckRunLogger) {
  SessionOptions so;

  so.session_logid = "CheckRunLogger";

  // create CapturingSink. LoggingManager will own it, but as long as the logging_manager
  // is around our pointer stays valid.
  auto capturing_sink = new CapturingSink();

  auto logging_manager = std::make_unique<Logging::LoggingManager>(
      std::unique_ptr<ISink>(capturing_sink), Logging::Severity::kVERBOSE, false, LoggingManager::InstanceType::Temporal);

  InferenceSession session_object{so, logging_manager.get()};
  ASSERT_TRUE(session_object.Load(MODEL_URI).IsOK());
  ASSERT_TRUE(session_object.Initialize().IsOK());

  RunOptions run_options;
  run_options.run_tag = "RunTag";
  RunModel(session_object, run_options);

#ifdef _DEBUG
  // check for some VLOG output to make sure tag was correct. VLOG is not enabled in release build
  auto& msgs = capturing_sink->Messages();
  std::copy(msgs.begin(), msgs.end(), std::ostream_iterator<std::string>(std::cout, "\n"));
  bool have_log_entry_with_run_tag =
      (std::find_if(msgs.begin(), msgs.end(),
                    [&run_options](std::string msg) { return msg.find(run_options.run_tag) != string::npos; }) != msgs.end());

  ASSERT_TRUE(have_log_entry_with_run_tag);
#endif
}

TEST(InferenceSessionTests, CheckRunProfilerWithSessionOptions) {
  SessionOptions so;

  so.session_logid = "CheckRunProfiler";
  so.enable_profiling = true;
  so.profile_file_prefix = "lotus_profile_test";

  InferenceSession session_object(so);
  ASSERT_TRUE(session_object.Load(MODEL_URI).IsOK());
  ASSERT_TRUE(session_object.Initialize().IsOK());

  RunOptions run_options;
  run_options.run_tag = "RunTag";

  RunModel(session_object, run_options);
  std::string profile_file = session_object.EndProfiling();

  std::ifstream profile(profile_file);
  ASSERT_TRUE(profile);
  std::string line;

  std::vector<std::string> tags = {"pid", "dur", "ts", "ph", "X", "name", "args"};
  int count = 0;
  while (std::getline(profile, line)) {
    if (count == 0) {
      ASSERT_TRUE(line.find("[") != string::npos);
    } else if (count <= 7) {
      for (auto& s : tags) {
        ASSERT_TRUE(line.find(s) != string::npos);
      }
    } else {
      ASSERT_TRUE(line.find("]") != string::npos);
    }

    if (count == 1) {
      ASSERT_TRUE(line.find("model_loading_uri") != string::npos);
    }
    count++;
  }
}

TEST(InferenceSessionTests, CheckRunProfilerWithStartProfile) {
  SessionOptions so;

  so.session_logid = "CheckRunProfiler";

  InferenceSession session_object(so);
  ASSERT_TRUE(session_object.Load(MODEL_URI).IsOK());
  ASSERT_TRUE(session_object.Initialize().IsOK());

  RunOptions run_options;
  run_options.run_tag = "RunTag";

  session_object.StartProfiling("lotus_profile_custom");
  RunModel(session_object, run_options);
  std::string profile_file = session_object.EndProfiling();

  std::ifstream profile(profile_file);
  std::string line;

  std::vector<std::string> tags = {"pid", "dur", "ts", "ph", "X", "name", "args"};
  int count = 0;
  while (std::getline(profile, line)) {
    if (count == 0) {
      ASSERT_TRUE(line.find("[") != string::npos);
    } else if (count <= 5) {
      for (auto& s : tags) {
        ASSERT_TRUE(line.find(s) != string::npos);
      }
    } else {
      ASSERT_TRUE(line.find("]") != string::npos);
    }

    if (count == 1) {
      ASSERT_TRUE(line.find("mul_1_fence_before") != string::npos);
    }
    count++;
  }
}

TEST(InferenceSessionTests, MultipleSessionsNoTimeout) {
  SessionOptions session_options;

  session_options.session_logid = "InferenceSessionTests.MultipleSessionsNoTimeout";
  InferenceSession session_object{session_options, &DefaultLoggingManager()};
  ASSERT_TRUE(session_object.Load(MODEL_URI).IsOK());
  ASSERT_TRUE(session_object.Initialize().IsOK());

  std::thread thread1{[&session_object]() {
    RunOptions run_options;
    run_options.run_tag = "one session/thread 1";
    RunModel(session_object, run_options);
  }};

  std::thread thread2{[&session_object]() {
    RunOptions run_options;
    run_options.run_tag = "one session/thread 2";
    RunModel(session_object, run_options);
  }};

  thread1.join();
  thread2.join();
}

TEST(InferenceSessionTests, PreAllocateOutputVector) {
  SessionOptions so;

  so.session_logid = "InferenceSessionTests.PreAllocateOutputVector";

  InferenceSession session_object{so, &DefaultLoggingManager()};
  ASSERT_TRUE(session_object.Load(MODEL_URI).IsOK());
  ASSERT_TRUE(session_object.Initialize().IsOK());

  RunOptions run_options;
  run_options.run_tag = "InferenceSessionTests.PreAllocateOutputVector";
  bool is_preallocate_output_vec = true;
  RunModel(session_object, run_options, is_preallocate_output_vec);
}

TEST(InferenceSessionTests, ConfigureVerbosityLevel) {
  SessionOptions so;

  so.session_logid = "ConfigureVerbosityLevel";
  so.session_log_verbosity_level = 1;

  // create CapturingSink. LoggingManager will own it, but as long as the logging_manager
  // is around our pointer stays valid.
  auto capturing_sink = new CapturingSink();

  auto logging_manager = std::make_unique<Logging::LoggingManager>(
      std::unique_ptr<ISink>(capturing_sink),
      Logging::Severity::kVERBOSE,
      false,
      LoggingManager::InstanceType::Temporal);

  InferenceSession session_object{so, logging_manager.get()};
  ASSERT_TRUE(session_object.Load(MODEL_URI).IsOK());
  ASSERT_TRUE(session_object.Initialize().IsOK());

  RunOptions run_options;
  run_options.run_tag = "ConfigureVerbosityLevel";
  run_options.run_log_verbosity_level = 1;
  RunModel(session_object, run_options);

#ifdef _DEBUG
  // check for some VLOG output to make sure tag was correct. VLOG is not enabled in release build
  auto& msgs = capturing_sink->Messages();
  std::copy(msgs.begin(), msgs.end(), std::ostream_iterator<std::string>(std::cout, "\n"));
  bool have_log_entry_with_vlog_session_msg =
      (std::find_if(msgs.begin(), msgs.end(),
                    [&run_options](std::string msg) { return msg.find("Adding input argument with name") != string::npos; }) !=
       msgs.end());

  ASSERT_TRUE(have_log_entry_with_vlog_session_msg);

  bool have_log_entry_with_vlog_run_msg =
      (std::find_if(msgs.begin(), msgs.end(),
                    [&run_options](std::string msg) { return msg.find("Size of execution plan vector") != string::npos; }) !=
       msgs.end());

  ASSERT_TRUE(have_log_entry_with_vlog_run_msg);
#endif
}

TEST(InferenceSessionTests, TestWithIstream) {
  SessionOptions so;

  so.session_logid = "InferenceSessionTests.TestWithIstream";

  InferenceSession session_object{so};

  std::ifstream model_file_stream(MODEL_URI, ios::in | ios::binary);
  ASSERT_TRUE(model_file_stream.good());
  ASSERT_TRUE(session_object.Load(model_file_stream).IsOK());
  ASSERT_TRUE(session_object.Initialize().IsOK());

  RunOptions run_options;
  run_options.run_tag = "InferenceSessionTests.TestWithIstream";
  RunModel(session_object, run_options);
}

TEST(InferenceSessionTests, TestRegisterExecutionProvider) {
  SessionOptions so;

  so.session_logid = "InferenceSessionTests.TestWithIstream";

  InferenceSession session_object{so};
  CPUExecutionProviderInfo epi;
  ASSERT_TRUE(session_object.RegisterExecutionProvider(std::make_unique<CPUExecutionProvider>(epi)).IsOK());

  std::ifstream model_file_stream(MODEL_URI, ios::in | ios::binary);
  ASSERT_TRUE(model_file_stream.good());
  ASSERT_TRUE(session_object.Load(model_file_stream).IsOK());
  ASSERT_TRUE(session_object.Initialize().IsOK());

  RunOptions run_options;
  run_options.run_tag = "InferenceSessionTests.TestWithIstream";
  RunModel(session_object, run_options);
}

TEST(InferenceSessionTests, TestModelProtoInterface) {
  SessionOptions so;

  so.session_logid = "InferenceSessionTests.TestModelProtoInterface";

  InferenceSession session_object{so};
  std::ifstream model_file_stream(MODEL_URI, ios::in | ios::binary);
  ModelProto model_proto;
  ASSERT_TRUE(LotusIR::Model::Load(model_file_stream, &model_proto).IsOK());
  ASSERT_TRUE(session_object.Load(model_proto).IsOK());
  ASSERT_TRUE(session_object.Initialize().IsOK());

  RunOptions run_options;
  run_options.run_tag = "InferenceSessionTests.TestModelProtoInterface";
  RunModel(session_object, run_options);
}

TEST(InferenceSessionTests, TestModelProtoInterfaceMultipleLoadFailure) {
  SessionOptions so;

  so.session_logid = "InferenceSessionTests.TestModelProtoInterfaceMultipleLoadFailure";

  InferenceSession session_object{so};
  std::ifstream model_file_stream(MODEL_URI, ios::in | ios::binary);
  ModelProto model_proto;
  ASSERT_TRUE(LotusIR::Model::Load(model_file_stream, &model_proto).IsOK());
  ASSERT_TRUE(session_object.Load(model_proto).IsOK());
  ASSERT_FALSE(session_object.Load(model_proto).IsOK());
  ASSERT_TRUE(session_object.Initialize().IsOK());
  ASSERT_TRUE(session_object.Initialize().IsOK());

  RunOptions run_options;
  run_options.run_tag = "InferenceSessionTests.TestModelProtoInterfaceMultipleLoadFailure";
  RunModel(session_object, run_options);
}

static void TestBindHelper(const std::string& log_str,
                           ProviderType bind_provider_type,
                           ProviderType run_provider_type,
                           bool preallocate_output,
                           ProviderType allocation_provider = kCpuExecutionProvider) {
  SessionOptions so;

  so.session_logid = "InferenceSessionTests." + log_str;
  so.session_log_verbosity_level = 1;  // change to 1 for detailed logging

  InferenceSession session_object{so, &DefaultLoggingManager()};

  if (bind_provider_type == kCudaExecutionProvider || run_provider_type == kCudaExecutionProvider) {
#ifdef USE_CUDA
    CUDAExecutionProviderInfo epi;
    epi.device_id = 0;
    EXPECT_TRUE(session_object.RegisterExecutionProvider(std::make_unique<CUDAExecutionProvider>(epi)).IsOK());
#endif
  }

  std::unique_ptr<Model> p_model;
  CreateMatMulModel(p_model, run_provider_type);

  std::stringstream s1;
  p_model->ToProto().SerializeToOstream(&s1);
  ASSERT_TRUE(session_object.Load(s1).IsOK());
  ASSERT_TRUE(session_object.Initialize().IsOK());

  RunOptions run_options;
  run_options.run_log_verbosity_level = so.session_log_verbosity_level;
  run_options.run_tag = so.session_logid;
  RunModelWithBindingMatMul(session_object,
                            run_options,
                            bind_provider_type,
                            preallocate_output,
                            allocation_provider);
}

TEST(InferenceSessionTests, TestBindCpu) {
  TestBindHelper("TestBindCpu",
                 kCpuExecutionProvider,
                 kCpuExecutionProvider,
                 false /* don't preallocate output */);
}

TEST(InferenceSessionTests, TestIOBindingReuse) {
  SessionOptions so;
  InferenceSession session_object(so);
  std::unique_ptr<Model> p_model;
  CreateMatMulModel(p_model, kCpuExecutionProvider);

  std::stringstream s1;
  p_model->ToProto().SerializeToOstream(&s1);
  ASSERT_TRUE(session_object.Load(s1).IsOK());
  ASSERT_TRUE(session_object.Initialize().IsOK());
  unique_ptr<IOBinding> io_binding;
  Status st = session_object.NewIOBinding(&io_binding);
  ASSERT_TRUE(st.IsOK());

  MLValue ml_value1;
  vector<float> v1{2.};
  CreateMLValue<float>(TestCPUExecutionProvider()->GetAllocator(), {1}, v1, &ml_value1);
  io_binding->BindOutput("foo", ml_value1);
  ASSERT_TRUE(io_binding->GetOutputs().size() == 1);
  auto span = io_binding->GetOutputs()[0].Get<Tensor>().DataAsSpan<float>();
  ASSERT_TRUE(static_cast<size_t>(span.size()) == v1.size());
  for (int i = 0; i < v1.size(); ++i) {
    ASSERT_TRUE(v1[i] == span[i]);
  }

  MLValue ml_value2;
  vector<float> v2{3.};
  CreateMLValue<float>(TestCPUExecutionProvider()->GetAllocator(), {1}, v2, &ml_value2);
  io_binding->BindOutput("foo", ml_value2);
  ASSERT_TRUE(io_binding->GetOutputs().size() == 1);
  span = io_binding->GetOutputs()[0].Get<Tensor>().DataAsSpan<float>();
  ASSERT_TRUE(static_cast<size_t>(span.size()) == v2.size());
  for (int i = 0; i < v2.size(); ++i) {
    ASSERT_TRUE(v2[i] == span[i]);
  }
}

TEST(InferenceSessionTests, InvalidInputTypeOfTensorElement) {
  SessionOptions so;

  so.session_logid = "InferenceSessionTests.InvalidInputTypeOfTensorElement";

  InferenceSession session_object{so, &DefaultLoggingManager()};
  ASSERT_TRUE(session_object.Load(MODEL_URI).IsOK());
  ASSERT_TRUE(session_object.Initialize().IsOK());

  RunOptions run_options;
  run_options.run_tag = so.session_logid;

  // prepare inputs
  std::vector<int64_t> dims_mul_x = {3, 2};
  std::vector<int64_t> values_mul_x = {1, 2, 3, 4, 5, 6};
  MLValue ml_value;
  CreateMLValue<int64_t>(TestCPUExecutionProvider()->GetAllocator(), dims_mul_x, values_mul_x, &ml_value);
  NameMLValMap feeds;
  feeds.insert(std::make_pair("X", ml_value));

  // prepare outputs
  std::vector<std::string> output_names;
  output_names.push_back("Y");
  std::vector<MLValue> fetches;

  // prepare expected inputs and outputs
  std::vector<int64_t> expected_dims_mul_y = {3, 2};
  std::vector<float> expected_values_mul_y = {1.0f, 4.0f, 9.0f, 16.0f, 25.0f, 36.0f};

  // Now run
  Common::Status st = session_object.Run(run_options, feeds, output_names, &fetches);
  if (!st.IsOK()) {
    std::cout << "Run returned status: " << st.ErrorMessage() << std::endl;
  }
  ASSERT_TRUE(!st.IsOK());
}

TEST(InferenceSessionTests, TestModelProtoUniquePtrInterface) {
  SessionOptions so;

  so.session_logid = "InferenceSessionTests.TestModelProtoUniquePtrInterface";

  InferenceSession session_object{so};
  std::ifstream model_file_stream(MODEL_URI, ios::in | ios::binary);
  auto p_model_proto = std::make_unique<ModelProto>();
  ASSERT_TRUE(LotusIR::Model::Load(model_file_stream, p_model_proto.get()).IsOK());
  ASSERT_TRUE(session_object.Load(std::move(p_model_proto)).IsOK());
  ASSERT_TRUE(session_object.Initialize().IsOK());

  RunOptions run_options;
  run_options.run_tag = so.session_logid;
  RunModel(session_object, run_options);
}

#ifdef USE_CUDA

TEST(InferenceSessionTests, TestBindCuda) {
  TestBindHelper("TestBindCuda",
                 kCudaExecutionProvider,
                 kCudaExecutionProvider,
                 false /* don't preallocate output */);
}

TEST(InferenceSessionTests, TestBindCudaPreallocateOutputOnCuda) {
  TestBindHelper("TestBindCudaPreallocateOutputOnCuda",
                 kCudaExecutionProvider,
                 kCudaExecutionProvider,
                 true /* preallocate output on GPU */,
                 kCudaExecutionProvider);
}

TEST(InferenceSessionTests, TestBindCudaPreallocateOutputOnCpu) {
  TestBindHelper("TestBindCudaPreallocateOutputOnCpu",
                 kCudaExecutionProvider,
                 kCudaExecutionProvider,
                 true /* preallocate output on CPU */,
                 kCpuExecutionProvider);
}

TEST(InferenceSessionTests, TestBindCudaPreallocateOutputOnCpu2) {
  TestBindHelper("TestBindCudaPreallocateOutputOnCpu2",
                 kCudaExecutionProvider,
                 kCpuExecutionProvider,
                 true /* preallocate output on CPU */,
                 kCpuExecutionProvider);
}

#endif

TEST(InferenceSessionTests, ModelWithoutOpset) {
  SessionOptions so;

  so.session_logid = "InferenceSessionTests.ModelWithoutOpset";

  InferenceSession session_object{so, &DefaultLoggingManager()};
  Status retval = session_object.Load(MODEL_URI_NO_OPSET);
  ASSERT_FALSE(retval.IsOK());
  if (!retval.IsOK()) {
    ASSERT_TRUE(retval.ErrorMessage().find("Missing opset in the model") != std::string::npos);
  }
}
}  // namespace Test
}  // namespace Lotus
