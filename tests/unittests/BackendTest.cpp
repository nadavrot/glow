/**
 * Copyright (c) 2017-present, Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "glow/ExecutionEngine/ExecutionEngine.h"
#include "glow/Graph/Graph.h"
#include "glow/IR/IRBuilder.h"

#include "gtest/gtest.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Casting.h"

using namespace glow;

class BackendTest : public ::testing::TestWithParam<BackendKind> {
public:
  ExecutionEngine EE_{GetParam()};
};

TEST(Interpreter, NotImplementedSave) {
  // Interpreter backend does not support a save method.
  // Exercise it and make sure that it fails.
  ExecutionEngine EE;
  auto &mod = EE.getModule();

  // Create a few nodes to make sure IR can be normally generated.
  Function *F = mod.createFunction("main");
  F->createSave("save", mod.createVariable(ElemKind::FloatTy, {2}, "A",
                                           VisibilityKind::Public, false));

  EXPECT_DEATH(EE.save(CompilationMode::Infer, F, "output", "network"), "");
}

TEST(Interpreter, profileQuantizationForANetwork) {
  ExecutionEngine EE;
  auto &mod = EE.getModule();
  Function *F = mod.createFunction("main");
  Tensor inputs(ElemKind::FloatTy, {1, 4});
  inputs.getHandle() = {1, 1.2f, 0.5f, 1.3f};

  auto *A = mod.createVariable(ElemKind::FloatTy, {1, 4}, "A",
                               VisibilityKind::Public);
  auto *Ex = mod.createVariable(ElemKind::FloatTy, {1, 4}, "E",
                                VisibilityKind::Public);
  Node *O = F->createFullyConnected("fc", A, 4);
  O = F->createRELU("relu", O);
  O = F->createRegression("reg", O, Ex);

  F = ::glow::profileQuantization(F);

  EE.compile(CompilationMode::Infer, F);

  // TODO: Verify histogram itself, for now just verify min and max.
  // Run inference first time and capture tensor stats.
  EE.run({A}, {&inputs});

  QuantizationProfileNode *profile{nullptr};
  // Find QPN for node A.
  for (auto &node : F->getNodes()) {
    if (QuantizationProfileNode *QPN =
            llvm::dyn_cast<QuantizationProfileNode>(&node)) {
      Node *observedNode = node.getNthInput(0).getNode();
      if (observedNode == A) {
        profile = QPN;
        break;
      }
    }
  }

  EXPECT_TRUE(profile != nullptr);

  auto CI = profile->getComputationInfoVar()->getHandle<float>();
  float min = CI.raw(0);
  float max = CI.raw(1);
  EXPECT_NEAR(0.5, min, 0.00001);
  EXPECT_NEAR(1.3, max, 0.00001);

  // Run inference for the second time with new min and max.
  inputs.getHandle() = {0.2f, 1.6f, 0.5f, 1.3f};
  EE.run({A}, {&inputs});
  min = CI.raw(0);
  max = CI.raw(1);
  EXPECT_NEAR(0.2, min, 0.00001);
  EXPECT_NEAR(1.6, max, 0.00001);
}

TEST_P(BackendTest, simpleInference) {
  Tensor inputs(ElemKind::FloatTy, {1, 32, 32, 3});

  auto &mod = EE_.getModule();
  Function *F = mod.createFunction("main");
  F->setName("interpret");
  auto *input = mod.createVariable(ElemKind::FloatTy, {1, 32, 32, 3}, "input",
                                   VisibilityKind::Public);

  auto *ex = mod.createVariable(ElemKind::Int64ITy, {1, 1}, "exp");

  auto *CV0 = F->createConv("conv1", input, 16, 5, 1, 2, 1);
  auto *RL0 = F->createRELU("relu1", CV0);
  auto *MP0 = F->createMaxPool("pool1", RL0, 2, 2, 0);

  auto *CV1 = F->createConv("conv2", MP0, 20, 5, 1, 2, 1);
  auto *RL1 = F->createRELU("relu2", CV1);
  auto *MP1 = F->createMaxPool("pool2", RL1, 2, 2, 0);

  auto *CV2 = F->createConv("conv3", MP1, 20, 5, 1, 2, 1);
  auto *RL2 = F->createRELU("relu3", CV2);
  auto *MP2 = F->createMaxPool("pool3", RL2, 2, 2, 0);

  auto *FCL1 = F->createFullyConnected("fc", MP2, 10);
  auto *RL3 = F->createRELU("relu4", FCL1);
  auto *SM = F->createSoftMax("sm", RL3, ex);
  F->createSave("ret", SM);

  EE_.compile(CompilationMode::Infer, F);

  EE_.run({input}, {&inputs});
}

TEST_P(BackendTest, debugPrint) {
  Tensor input{0.0, 1.0, 2.0, 3.0};
  Module mod;
  Function *F = mod.createFunction("main");
  auto *IV = mod.createVariable("input", input, VisibilityKind::Public, false);
  (void)IV;

  auto IR = llvm::make_unique<IRFunction>(F);
  IR->generateIR();
  IRBuilder(IR.get()).createDebugPrintInst("print", *IR->getWeights().begin());

  std::unique_ptr<Backend> backend(createBackend(GetParam()));
  auto function = backend->compile(std::move(IR));
  function->execute({}, {});
}

/// This test checks that we can compile a function without depending on the
/// graph representation. We compile some function and then delete the function.
/// Later we execute the code and check that things work.
TEST_P(BackendTest, decoupleCodegenFromGraph) {
  Module mod;
  Function *F = mod.createFunction("main");
  auto *X = mod.createVariable(ElemKind::FloatTy, {3}, "X");
  X->getPayload().getHandle() = {1., 2., 3.};
  auto *pow = F->createPow("Pow1", X, 2.0);
  auto *save = F->createSave("save", pow);
  Variable *res = save->getVariable();
  EE_.compile(CompilationMode::Infer, F);

  // Erase all of the functions to ensure that the compiled code does not
  // depend on the graph.
  mod.eraseFunctions();

  // We can run the compiled code without having the graph representation
  // around.
  EE_.run({}, {});

  auto HX = res->getPayload().getHandle();
  EXPECT_NEAR(HX.at({0}), 1, 1E-5);
  EXPECT_NEAR(HX.at({1}), 4, 1E-5);
  EXPECT_NEAR(HX.at({2}), 9, 1E-5);
}

/// Check that we can pass information to the execution engine using Placeholder
/// variables and read it back using Save nodes (in variables).
TEST(Placeholder, simplePlaceholderValue) {
  Tensor data{99.0, 35.0, 2.0, 3.0};

  ExecutionEngine EE{BackendKind::Interpreter};
  auto &mod = EE.getModule();

  Function *F = mod.createFunction("main");
  auto *input = mod.createPlaceholder(ElemKind::FloatTy, {4}, "input");
  SaveNode *S = F->createSave("ret", input);

  EE.compile(CompilationMode::Infer, F);

  EE.run({input}, {&data});

  auto &res = S->getVariable()->getPayload();
  EXPECT_TRUE(res.isEqual(data));
}

/// Check that we can pass information to the execution engine using Placeholder
/// variables and the runBatch API.
TEST(Placeholder, runBatchTest) {
  // The input contains two slices of 4 floats each.
  Tensor data(ElemKind::FloatTy, {4, 4});
  // Fill the array with the pattern: [0 1 2 3; 10, 11, 12, 13; 20 21 22 23 ...]
  for (size_t i = 0; i < 4; i++) {
    for (size_t j = 0; j < 4; j++) {
      data.getHandle().at({i, j}) = i * 10 + j;
    }
  }

  ExecutionEngine EE{BackendKind::Interpreter};
  auto &mod = EE.getModule();

  Function *F = mod.createFunction("main");
  auto *input = mod.createPlaceholder(ElemKind::FloatTy, {1, 4}, "input");
  SaveNode *S = F->createSave("ret", input);

  EE.compile(CompilationMode::Infer, F);

  // Run the batch for 2 iterations:
  EE.runBatch(2, {input}, {&data});

  Tensor expected{10, 11, 12, 13};
  auto EH = expected.getHandle();
  auto RH = S->getVariable()->getPayload().getHandle();

  for (size_t i = 0; i < 4; i++) {
    EXPECT_EQ(RH.raw(i), EH.raw(i));
  }
}

INSTANTIATE_TEST_CASE_P(Interpreter, BackendTest,
                        ::testing::Values(BackendKind::Interpreter));

#ifdef GLOW_WITH_CPU
INSTANTIATE_TEST_CASE_P(JIT, BackendTest, ::testing::Values(BackendKind::CPU));
#endif

#ifdef GLOW_WITH_OPENCL
INSTANTIATE_TEST_CASE_P(OpenCL, BackendTest,
                        ::testing::Values(BackendKind::OpenCL));
#endif
