/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/xla/service/hlo_scheduling.h"

#include <memory>
#include <string>

#include "tensorflow/compiler/xla/service/hlo_computation.h"
#include "tensorflow/compiler/xla/service/hlo_instruction.h"
#include "tensorflow/compiler/xla/service/hlo_opcode.h"
#include "tensorflow/compiler/xla/service/hlo_ordering.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/tests/hlo_test_base.h"
#include "tensorflow/compiler/xla/types.h"
#include "tensorflow/compiler/xla/xla_data.pb.h"

namespace xla {
namespace {

class MinimumMemoryForSequenceTest : public HloTestBase {};

TEST_F(MinimumMemoryForSequenceTest, MultiComputation) {
  auto module = CreateNewModule();
  const Shape scalar_shape = ShapeUtil::MakeShape(xla::F32, {});
  const Shape tuple_shape =
      ShapeUtil::MakeTupleShape({scalar_shape, scalar_shape});

  auto cond_builder = HloComputation::Builder("WhileCond");
  // Tuple param: 24 bytes (each elem has 8 byte pointer, 4 byte element)
  HloInstruction* cond_param = cond_builder.AddInstruction(
      HloInstruction::CreateParameter(0, tuple_shape, "cond_param"));
  HloInstruction* cond_iter = cond_builder.AddInstruction(
      HloInstruction::CreateGetTupleElement(scalar_shape, cond_param, 0));
  HloInstruction* cond_data = cond_builder.AddInstruction(
      HloInstruction::CreateGetTupleElement(scalar_shape, cond_param, 1));
  // Free cond_param[] (16 bytes), Alloc PRED[] (1 byte)
  HloInstruction* cond_lt = cond_builder.AddInstruction(
      HloInstruction::CreateBinary(ShapeUtil::MakeShape(PRED, {}),
                                   HloOpcode::kLt, cond_iter, cond_data));
  HloComputation* cond_computation =
      module->AddEmbeddedComputation(cond_builder.Build());

  auto body_builder = HloComputation::Builder("WhileBody");
  // Tuple param: 24 bytes (each elem has 8 byte pointer, 4 byte element)
  HloInstruction* body_param = body_builder.AddInstruction(
      HloInstruction::CreateParameter(0, tuple_shape, "body_param"));
  HloComputation* body_computation =
      module->AddEmbeddedComputation(body_builder.Build());

  auto builder = HloComputation::Builder(TestName());
  // Entry params: 8 bytes (4 bytes per param), TOTAL=8
  HloInstruction* iter = builder.AddInstruction(
      HloInstruction::CreateParameter(0, scalar_shape, "param_iter"));
  HloInstruction* data = builder.AddInstruction(
      HloInstruction::CreateParameter(1, scalar_shape, "param_data"));
  // Tuple: 16 bytes (8 bytes per pointer), TOTAL=24
  HloInstruction* tuple =
      builder.AddInstruction(HloInstruction::CreateTuple({iter, data}));
  // While: 8 bytes (4 bytes per element), TOTAL=32
  // Both cond and body use a max of 24 bytes, TOTAL=56
  HloInstruction* while_op = builder.AddInstruction(HloInstruction::CreateWhile(
      tuple_shape, cond_computation, body_computation, tuple));
  HloComputation* entry_computation =
      module->AddEntryComputation(builder.Build());

  auto size_fn = [](const LogicalBuffer& buffer) {
    return ShapeUtil::ByteSizeOf(buffer.shape(), /*pointer_size=*/8);
  };

  SequentialHloOrdering::HloModuleSequence module_sequence;
  module_sequence[cond_computation] = {cond_param, cond_iter, cond_data,
                                       cond_lt};
  module_sequence[body_computation] = {body_param};
  module_sequence[entry_computation] = {iter, data, tuple, while_op};
  EXPECT_EQ(56,
            MinimumMemoryForSequence(module_sequence, size_fn).ValueOrDie());
}

}  // namespace
}  // namespace xla
