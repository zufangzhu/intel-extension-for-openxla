/* Copyright (c) 2023 Intel Corporation

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

#include "xla/service/gpu/spir_compiler.h"

#include <stdlib.h>

#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "tsl/platform/path.h"
#include "tsl/platform/status.h"
#include "tsl/util/env_var.h"
#include "xla/hlo/ir/hlo_opcode.h"
#include "xla/service/algebraic_simplifier.h"
#include "xla/service/call_inliner.h"
#include "xla/service/convert_mover.h"
#include "xla/service/dot_dimension_merger.h"
#include "xla/service/dump.h"
#include "xla/service/float_normalization.h"
#include "xla/service/float_support.h"
#include "xla/service/gpu/backend_configs.pb.h"
#include "xla/service/gpu/buffer_sharing.h"
#include "xla/service/gpu/cublas_cudnn.h"
#include "xla/service/gpu/cudnn_fused_conv_rewriter.h"
#include "xla/service/gpu/cudnn_fused_mha_rewriter.h"
#include "xla/service/gpu/cusolver_rewriter.h"
#include "xla/service/gpu/gpu_conv_padding_legalization.h"
#include "xla/service/gpu/gpu_conv_rewriter.h"
#include "xla/service/gpu/gpu_layout_assignment.h"
#include "xla/service/gpu/ir_emission_utils.h"
#include "xla/service/gpu/llvm_gpu_backend/gpu_backend_lib.h"
#include "xla/service/gpu/redundant_convert_mover.h"
#include "xla/service/gpu/target_constants.h"
#include "xla/service/gpu/triangular_solve_rewriter.h"
#include "xla/service/hlo_constant_folding.h"
#include "xla/service/hlo_cse.h"
#include "xla/service/hlo_dce.h"
#include "xla/service/hlo_pass_fix.h"
#include "xla/service/hlo_pass_pipeline.h"
#include "xla/service/hlo_verifier.h"
#include "xla/service/layout_normalization.h"
#include "xla/service/llvm_ir/llvm_util.h"
#include "xla/service/reshape_decomposer.h"
#include "xla/service/reshape_mover.h"
#include "xla/service/tuple_simplifier.h"
#include "xla/status_macros.h"
#include "xla/stream_executor/sycl/sycl_platform_id.h"
#include "xla/types.h"
#include "xla/util.h"

namespace xla {
namespace gpu {

class ConvBfloat16Support : public FloatSupport {
 public:
  explicit ConvBfloat16Support()
      : FloatSupport(BF16), is_conv_bf16_supported_(true) {}

  bool SupportsLowPrecisionOperand(const HloInstruction& hlo,
                                   int64_t operand_index) const override {
    return (hlo.opcode() != HloOpcode::kConvolution) || is_conv_bf16_supported_;
  }

  bool SupportsLowPrecisionOutput(const HloInstruction& hlo) const override {
    return (hlo.opcode() != HloOpcode::kConvolution) || is_conv_bf16_supported_;
  }

  bool SupportsMixedPrecisions(const HloInstruction& hlo) const override {
    // Skip all HLOs other than convolutions.
    return (hlo.opcode() != HloOpcode::kConvolution);
  }

 private:
  bool is_conv_bf16_supported_;
};

Status SPIRCompiler::OptimizeHloConvolutionCanonicalization(
    HloModule* hlo_module, se::GpuComputeCapability gpu_version,
    se::dnn::VersionInfo dnn_version,
    se::DeviceMemoryAllocator* device_allocator) {
  auto cuda_compute_capability =
      std::get<se::CudaComputeCapability>(gpu_version);
  // Convert convolutions into CustomCalls to onednn, then canonicalize them
  // (GpuConvPaddingLegalization). Also expand cuSolver calls.
  HloPassPipeline pipeline("conv_canonicalization");
  pipeline.AddInvariantCheckerDebug<HloVerifier>(
      /*layout_sensitive=*/false,
      /*allow_mixed_precision=*/false);

  // Convert upsupported bf16 convolutions to f32.
  ConvBfloat16Support conv_bf16_support;
  pipeline.AddPass<FloatNormalization>(&conv_bf16_support);

  pipeline.AddPass<GpusolverRewriter>();
  pipeline.AddPass<GpuConvRewriter>();
  pipeline.AddPass<CudnnFusedConvRewriter>(cuda_compute_capability);
  pipeline.AddPass<GpuConvPaddingLegalization>();

  // The conv padding/vectorization passes which we need to get rid of.  They
  // also leave behind unnecessary tuple/get-tuple-element pairs that
  // TupleSimplifier fixes.
  pipeline.AddPass<CallInliner>();
  pipeline.AddPass<TupleSimplifier>();

  AlgebraicSimplifierOptions algsimp_options;
  algsimp_options.set_enable_conv_operand_swap(false);
  algsimp_options.set_enable_unconditional_reduce_of_concat_replacement(false);
  pipeline.AddPass<HloPassFix<AlgebraicSimplifier>>(algsimp_options);

  // tf2xla bridge, DepthwiseConvolutionConverter, GpuConvRewriter, and
  // CudnnSimplifyPadding introduce reshapes and transposes.  Run ReshapeMover
  // to a fixed point.  Include algsimp because ReshapeMover relies on it.
  [&, &pipeline = pipeline.AddPass<HloPassFix<HloPassPipeline>>(
          "reshape_mover_after_conv_canonicalization")] {
    ReshapeMoverOptions reshape_mover_options;
    reshape_mover_options.reshape_of_1d_broadcast_is_cheap = true;
    pipeline.AddPass<HloPassFix<ReshapeMover>>(reshape_mover_options);
    pipeline.AddPass<AlgebraicSimplifier>(algsimp_options);
  }();

  // The reshapes and transposes can possibly be eliminated using
  // AlgebraicSimplifier. ConvertMover and ReshapeMover fight with each other.
  // ConvertMover wants to move some converts down the graph, but ReshapeMover
  // wants to move them up the graph. We run ConvertMover and algsimp to a fixed
  // point.
  [&, &pipeline = pipeline.AddPass<HloPassFix<HloPassPipeline>>(
          "simplify_after_conv_canonicalization")] {
    pipeline.AddPass<ConvertMover>();
    pipeline.AddPass<AlgebraicSimplifier>(algsimp_options);
  }();

  // GpuConvRewriter, GpuConvPaddingLegalization and
  // CudnnConvPadForTensorCores may add instructions which can be simplified
  // by constant folding.
  pipeline.AddPass<HloConstantFolding>();
  TF_RETURN_IF_ERROR(pipeline.Run(hlo_module).status());

  return OkStatus();
}

Status SPIRCompiler::OptimizeHloPostLayoutAssignment(
    HloModule* hlo_module, se::StreamExecutor* stream_exec,
    const CompileOptions& options, const TargetConfig& gpu_target_config,
    tsl::thread::ThreadPool* thread_pool) {
  HloPassPipeline pre_pipeline("spir post-layout_assignment part 1");

  // This needs to run before GemmRewriter, which is part of
  // OptimizeHloPostLayoutAssignment().
  auto cuda_compute_capability = std::get<se::CudaComputeCapability>(
      gpu_target_config.device_description.gpu_compute_capability());

  bool use_mha = true;
  TF_CHECK_OK(tsl::ReadBoolFromEnvVar("MHA", true, &use_mha));
  if (use_mha) {
    HloPassPipeline mha_fusion_pipeline("multi-headed attention fusion");
    const DebugOptions& debug_options = hlo_module->config().debug_options();
    // The LayoutAssignment pass may leave behind kCopy instructions which are
    // duplicate or NOPs, so remove them with algebraic simplification and CSE.
    AlgebraicSimplifierOptions alg_sim_options;
    alg_sim_options.set_supports_non_canonical_dots(false);
    alg_sim_options.set_is_layout_sensitive(true);
    alg_sim_options.set_enable_conv_operand_swap(false);
    // "slow" minmax means we propagate nan.
    alg_sim_options.set_minmax_propagate_nan(
        !hlo_module->config().debug_options().xla_gpu_enable_fast_min_max());
    alg_sim_options.set_enable_unconditional_reduce_of_concat_replacement(
        false);
    if (debug_options.xla_gpu_normalize_layouts()) {
      mha_fusion_pipeline.AddPass<ReshapeDecomposer>();
      mha_fusion_pipeline.AddPass<LayoutNormalization>();
    }
    mha_fusion_pipeline.AddPass<HloCSE>(/*is_layout_sensitive=*/true);
    mha_fusion_pipeline.AddPass<HloPassFix<AlgebraicSimplifier>>(
        alg_sim_options);
    mha_fusion_pipeline.AddPass<HloCSE>(/*is_layout_sensitive=*/true);

    // Rewrite Multi-Headed Attention modules to Fused MHA custom-calls.
    // SYCL: RedundantConvertMover causes UT failure
    // mha_fusion_pipeline.AddPass<RedundantConvertMover>();
    mha_fusion_pipeline.AddPass<HloDCE>();
    mha_fusion_pipeline.AddPass<CudnnFusedMHARewriter>(
        cuda_compute_capability, stream_exec);
    mha_fusion_pipeline.AddPass<AlgebraicSimplifier>(alg_sim_options);
    mha_fusion_pipeline.AddPass<HloDCE>();
    mha_fusion_pipeline.AddPass<HloCSE>(/*is_layout_sensitive=*/true,
                                        /*only_fusion_computations*/ false);
    TF_RETURN_IF_ERROR(mha_fusion_pipeline.Run(hlo_module).status());
  }

  pre_pipeline.AddPass<DotDimensionMerger>();

  // Padding a gemm operand that's a constant results in pad(constant).  Run
  // constant-folding to simplify this into a new constant.
  pre_pipeline.AddPass<HloConstantFolding>();
  TF_RETURN_IF_ERROR(pre_pipeline.Run(hlo_module).status());

  TF_RETURN_IF_ERROR(GpuCompiler::OptimizeHloPostLayoutAssignment(
      hlo_module, stream_exec, options, gpu_target_config, thread_pool));

  HloPassPipeline post_pipeline("spir post-layout_assignment part 2");

  // Transform TriangularSolve ops into custom-calls, so we can add temp
  // memory.
  post_pipeline.AddPass<TriangularSolveRewriter>();

  TF_RETURN_IF_ERROR(post_pipeline.Run(hlo_module).status());

  return OkStatus();
}

namespace {
// Try to load textual LLVM IR from files defined in the FLAGS. If
// successful, return the llvm::Module, otherwise return nullptr.
std::unique_ptr<llvm::Module> MaybeLoadLLVMFromFile(const HloModule* module,
                                                    llvm::Module* llvm_module) {
  // If the xla_gpu_llvm_ir_file option is set, be explicit if a file is used
  // and warn when a file is not used to ease catching typo in filename.
  if (module == nullptr) {
    return nullptr;
  }

  std::string prefix = xla::FilenameFor(*module, "", "");
  auto xla_gpu_llvm_ir_file =
      module->config().debug_options().xla_gpu_llvm_ir_file();
  auto matched_filename = absl::c_find_if(
      xla_gpu_llvm_ir_file, [prefix](const std::string& full_filename) {
        // To ease comparing many LLVM versions, accept different suffixes then
        // the original filename.
        return absl::StartsWith(tsl::io::Basename(full_filename), prefix);
      });
  if (!xla_gpu_llvm_ir_file.empty() &&
      matched_filename == std::end(xla_gpu_llvm_ir_file)) {
    VLOG(0) << "RunBackend() - For module with prefix '" << prefix
            << "', we did not found a LLVM file to load.";
  }

  if (matched_filename != std::end(xla_gpu_llvm_ir_file)) {
    VLOG(0) << "RunBackend() - Will load LLVM from file: " << *matched_filename;
    llvm::LLVMContext& context = llvm_module->getContext();
    llvm::SMDiagnostic err;
    std::unique_ptr<llvm::Module> loaded_module =
        llvm::parseIRFile(*matched_filename, err, context);

    if (!loaded_module) {
      err.print("ERR", llvm::errs());
      LOG(FATAL) << "Failed to load an LLVM file. It is probably invalid LLVM.";
    }
    // Overwrite the dumped not optimized LLVM to show which one will be used.
    llvm_ir::DumpIrIfEnabled(*module, *loaded_module, /*optimized=*/false);
    return loaded_module;
  }
  return nullptr;
}

}  // namespace

SPIRCompiler::SPIRCompiler()
    : GpuCompiler(stream_executor::sycl::kSyclPlatformId, spir::TargetTriple(),
                  spir::DataLayout()) {}

HloDataflowAnalysis::CanShareBuffer SPIRCompiler::GetCanShareBuffer() const {
  return &CanShareBufferHint;
}

StatusOr<std::pair<std::string, std::vector<uint8_t>>>
SPIRCompiler::CompileTargetBinary(const HloModuleConfig& module_config,
                                  llvm::Module* llvm_module,
                                  se::GpuComputeCapability gpu_version,
                                  bool relocatable,
                                  const HloModule* debug_module,
                                  const CompileOptions& options) {
  std::string libdevice_dir;
  VLOG(2) << "Libdevice dir = " << libdevice_dir << "\n";
  std::unique_ptr<llvm::Module> loaded_module =
      MaybeLoadLLVMFromFile(debug_module, llvm_module);
  llvm::Module* selected_module = nullptr;
  if (loaded_module) {
    selected_module = loaded_module.get();
  } else {
    selected_module = llvm_module;
  }

  std::string spir;
  if (debug_module) {
    XLA_SCOPED_LOGGING_TIMER("CompileTargetBinary - CompileToSpir");
    TF_ASSIGN_OR_RETURN(spir, spir::CompileToSpir(selected_module, gpu_version,
                                                  module_config.debug_options(),
                                                  libdevice_dir));
  }

  std::vector<uint8_t> spir_bin(spir.begin(), spir.end());
  return std::pair<std::string, std::vector<uint8_t>>("", std::move(spir_bin));
}

/*static*/ SPIRCompiler* SPIRCompiler::CreateSPIRCompiler() {
  static auto compiler = absl::make_unique<SPIRCompiler>();
  return compiler.get();
}

}  // namespace gpu
}  // namespace xla
