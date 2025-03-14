# Owner(s): ["oncall: distributed"]

import functools
import itertools
import sys
from unittest import mock

import torch
import torch.distributed as dist
import torch.nn as nn
from torch.testing._internal.common_distributed import (
    skip_if_lt_x_gpu,
)
from torch.testing._internal.common_fsdp import (
    DummyDDP,
    FSDPInitMode,
    FSDPTest,
    MixtureOfExperts,
    NestedWrappedModule,
    NestedWrappedModuleWithDelay,
    TransformerWithSharedParams,
    subtest_name
)
from torch.testing._internal.common_utils import (
    TEST_WITH_DEV_DBG_ASAN,
    instantiate_parametrized_tests,
    parametrize,
    run_tests,
)

from torch.distributed.fsdp import CPUOffload, MixedPrecision
from torch.distributed.fsdp.fully_sharded_data_parallel import BackwardPrefetch, ShardingStrategy


if not dist.is_available():
    print("Distributed not available, skipping tests", file=sys.stderr)
    sys.exit(0)

if TEST_WITH_DEV_DBG_ASAN:
    print(
        "Skip dev-asan as torch + multiprocessing spawn have known issues",
        file=sys.stderr,
    )
    sys.exit(0)

params = "cpu_offload,backward_prefetch,forward_prefetch,sharding_strategy"
cpu_offload_config = [CPUOffload(offload_params=True), CPUOffload(offload_params=False)]
backward_prefetch_config = [BackwardPrefetch.BACKWARD_PRE, BackwardPrefetch.BACKWARD_POST, None]
forward_prefetch_config = ["forward_prefetch", "no_forward_prefetch"]
sharding_strategy_config = [ShardingStrategy.SHARD_GRAD_OP, None, ShardingStrategy.NO_SHARD]
configs = list(itertools.product(cpu_offload_config,
                                 backward_prefetch_config,
                                 forward_prefetch_config,
                                 sharding_strategy_config))
test_name_mapping = {
    str(CPUOffload(offload_params=True)): "offload_true",
    str(CPUOffload(offload_params=False)): "offload_false",
    str(BackwardPrefetch.BACKWARD_PRE): "backward_prefetch_pre",
    str(BackwardPrefetch.BACKWARD_POST): "backward_prefetch_post",
    "forward_prefetch": "forward_prefetch",
    "no_forward_prefetch": "no_forward_prefetch",
    str(ShardingStrategy.SHARD_GRAD_OP): "shard_grad_op",
    str(ShardingStrategy.NO_SHARD): "no_shard",
}

subtest_name = functools.partial(subtest_name, test_name_mapping)


class TestParityWithDDP(FSDPTest):
    """
    Compare losses and parameter values after several updates when using
    PyTorch DDP vs. FullyShardedDataParallel.
    """

    def _get_init_modes_for_test(self, cpu_offload):
        modes = [
            FSDPInitMode.CUDA_AFTER,
            FSDPInitMode.CUDA_BEFORE
        ]
        # Note that FSDPInitMode.CUDA_NEVER works currently only with CPU
        # offload as we explicitly bring the param back to CUDA device. In
        # general, it will not work since we try to all_gather p.data which is
        # on CPU but NCCL only supports GPU.
        if cpu_offload.offload_params:
            modes.append(FSDPInitMode.CUDA_NEVER)

        return modes

    @skip_if_lt_x_gpu(2)
    @parametrize(params, configs, subtest_name)
    def test_nested_wrapped_model(self, cpu_offload, backward_prefetch, forward_prefetch, sharding_strategy):
        forward_prefetch = (forward_prefetch == "forward_prefetch")
        init_modes = self._get_init_modes_for_test(cpu_offload)
        for fsdp_init_mode in init_modes:
            with self.subTest(fsdp_init_mode=fsdp_init_mode):
                self._test_identical_outputs(
                    NestedWrappedModule,
                    fsdp_init_mode=fsdp_init_mode,
                    cpu_offload=cpu_offload,
                    backward_prefetch=backward_prefetch,
                    forward_prefetch=forward_prefetch,
                    sharding_strategy=sharding_strategy,
                )

    @skip_if_lt_x_gpu(2)
    @parametrize("cpu_offload", cpu_offload_config)
    @parametrize("sharding_strategy", sharding_strategy_config)
    @parametrize("mixed_precision", [True, False])
    def test_nested_wrapped_model_single_iteration_mixed_precision(
        self,
        cpu_offload,
        sharding_strategy,
        mixed_precision
    ):
        init_modes = self._get_init_modes_for_test(cpu_offload)
        mixed_precision = MixedPrecision(
            param_dtype=torch.float16,
            buffer_dtype=torch.float16,
            reduce_dtype=torch.float16,
        ) if mixed_precision else None
        for fsdp_init_mode in init_modes:
            with self.subTest(fsdp_init_mode=fsdp_init_mode):
                self._test_identical_outputs(
                    NestedWrappedModule,
                    # Only run one step for comparison, as usually grad scaler
                    # is needed to avoid NaN after first step.
                    num_steps=1,
                    fsdp_init_mode=fsdp_init_mode,
                    cpu_offload=cpu_offload,
                    sharding_strategy=sharding_strategy,
                    mixed_precision=mixed_precision,
                )

    @skip_if_lt_x_gpu(2)
    @parametrize(params, configs, subtest_name)
    @parametrize("clip_norm_type", [2.0, None])
    def test_nested_all_wrapped_model(
            self, cpu_offload, backward_prefetch, forward_prefetch, sharding_strategy, clip_norm_type):
        forward_prefetch = (forward_prefetch == "forward_prefetch")
        init_modes = self._get_init_modes_for_test(cpu_offload)
        for fsdp_init_mode in init_modes:
            with self.subTest(fsdp_init_mode=fsdp_init_mode):
                model_fn = functools.partial(NestedWrappedModule, wrap_everything=True)
                self._test_identical_outputs(
                    model_fn,
                    fsdp_init_mode=fsdp_init_mode,
                    cpu_offload=cpu_offload,
                    backward_prefetch=backward_prefetch,
                    forward_prefetch=forward_prefetch,
                    norm_type=clip_norm_type,
                    sharding_strategy=sharding_strategy,
                )

    @skip_if_lt_x_gpu(2)
    @parametrize(params, configs, subtest_name)
    @parametrize("clip_norm_type", [2.0, None])
    def test_transformer_parameterized(
            self, cpu_offload, backward_prefetch, forward_prefetch, sharding_strategy, clip_norm_type):
        forward_prefetch = (forward_prefetch == "forward_prefetch")
        init_modes = self._get_init_modes_for_test(cpu_offload)
        for fsdp_init_mode in init_modes:
            with self.subTest(fsdp_init_mode=fsdp_init_mode):
                self._test_identical_outputs(
                    TransformerWithSharedParams,
                    fsdp_init_mode=fsdp_init_mode,
                    cpu_offload=cpu_offload,
                    backward_prefetch=backward_prefetch,
                    forward_prefetch=forward_prefetch,
                    norm_type=clip_norm_type,
                    sharding_strategy=sharding_strategy,
                )

    @skip_if_lt_x_gpu(2)
    @parametrize(params, configs, subtest_name)
    def test_delayed_optim_step(self, cpu_offload, backward_prefetch, forward_prefetch, sharding_strategy):
        forward_prefetch = (forward_prefetch == "forward_prefetch")
        # We use a model with a long CUDA delay right before the optimizer step.
        # This tests our streams logic, and that we don't start the allgather
        # until after the optimization step completes.
        init_modes = self._get_init_modes_for_test(cpu_offload)
        for fsdp_init_mode in init_modes:
            with self.subTest(fsdp_init_mode=fsdp_init_mode):
                model_fn = functools.partial(
                    NestedWrappedModuleWithDelay, delay_after_loss_ms=250
                )
                self._test_identical_outputs(
                    model_fn,
                    fsdp_init_mode=fsdp_init_mode,
                    cpu_offload=cpu_offload,
                    backward_prefetch=backward_prefetch,
                    forward_prefetch=forward_prefetch,
                    sharding_strategy=sharding_strategy,
                )

    @skip_if_lt_x_gpu(2)
    @parametrize(params, configs, subtest_name)
    def test_delayed_reduce_scatter(self, cpu_offload, backward_prefetch, forward_prefetch, sharding_strategy):
        forward_prefetch = (forward_prefetch == "forward_prefetch")
        # We insert a delay in the torch.distributed._reduce_scatter_base op, so that
        # the post_backward_stream takes much longer than the backward pass.
        # This tests that we properly block at the end of the backward pass for
        # the reductions to finish.
        init_modes = self._get_init_modes_for_test(cpu_offload)
        for fsdp_init_mode in init_modes:
            with self.subTest(fsdp_init_mode=fsdp_init_mode):
                model_fn = functools.partial(
                    NestedWrappedModuleWithDelay, delay_before_reduction_ms=250
                )
                self._test_identical_outputs(
                    model_fn,
                    fsdp_init_mode=fsdp_init_mode,
                    cpu_offload=cpu_offload,
                    backward_prefetch=backward_prefetch,
                    forward_prefetch=forward_prefetch,
                    sharding_strategy=sharding_strategy,
                )

    def _dummy_ddp_fn(self, model):
        return DummyDDP(model)

    @skip_if_lt_x_gpu(2)
    @parametrize(params, configs, subtest_name)
    @parametrize("clip_norm_type", [2.0, None])
    def test_mixture_of_experts(
            self, cpu_offload, backward_prefetch, forward_prefetch, sharding_strategy, clip_norm_type):
        forward_prefetch = (forward_prefetch == "forward_prefetch")
        init_modes = self._get_init_modes_for_test(cpu_offload)
        for fsdp_init_mode in init_modes:
            with self.subTest(fsdp_init_mode=fsdp_init_mode):
                self._test_identical_outputs(
                    MixtureOfExperts,
                    # MixtureOfExperts implements custom reduce logic, so the reference
                    # behavior should use that logic instead of PyTorch DDP.
                    ref_ddp_fn=self._dummy_ddp_fn,
                    fsdp_init_mode=fsdp_init_mode,
                    cpu_offload=cpu_offload,
                    backward_prefetch=backward_prefetch,
                    forward_prefetch=forward_prefetch,
                    norm_type=clip_norm_type,
                    sharding_strategy=sharding_strategy,
                )

    @skip_if_lt_x_gpu(2)
    @parametrize(params, configs, subtest_name)
    def test_mixture_of_experts_with_delay_before_free(
            self, cpu_offload, backward_prefetch, forward_prefetch, sharding_strategy):
        forward_prefetch = (forward_prefetch == "forward_prefetch")
        init_modes = self._get_init_modes_for_test(cpu_offload)
        for fsdp_init_mode in init_modes:
            with self.subTest(fsdp_init_mode=fsdp_init_mode):
                model_fn = functools.partial(MixtureOfExperts, delay_before_free_ms=250)
                self._test_identical_outputs(
                    model_fn,
                    ref_ddp_fn=self._dummy_ddp_fn,
                    fsdp_init_mode=fsdp_init_mode,
                    cpu_offload=cpu_offload,
                    backward_prefetch=backward_prefetch,
                    forward_prefetch=forward_prefetch,
                    sharding_strategy=sharding_strategy,
                )


class TestParamInit(FSDPTest):
    @skip_if_lt_x_gpu(2)
    @parametrize("mixed_precision", [True, False])
    def test_param_change_after_init(self, mixed_precision):
        group = dist.distributed_c10d._get_default_group()
        # Establish reference behavior.
        mixed_precision = MixedPrecision() if mixed_precision else None
        config = {"mixed_precision": mixed_precision}
        model = self._get_wrapped_model(
            group, config=config, cuda_first=False
        )
        model.eval()  # no dropout for this test
        input = model.module.get_input(torch.device("cuda"))
        ref_output = model(*input)

        # Change the weights in place.
        model = self._get_wrapped_model(group, cuda_first=False)
        model.eval()  # no dropout for this test
        first_param = next(model.parameters())
        nn.init.normal_(first_param.data)
        new_output = model(*input)

        self.assertNotEqual(
            ref_output,
            new_output,
            msg="new_output did not reflect change to param after init",
        )


class TestHooks(FSDPTest):
    # They aspire to make sure that backward hooks are registered and used
    @skip_if_lt_x_gpu(2)
    @parametrize("cuda_first", [False, True])
    def test_output_backward_hooks(self, cuda_first):
        group = dist.distributed_c10d._get_default_group()
        model = self._get_wrapped_model(group, cuda_first=cuda_first)
        self._test_output_backward_hooks(model=model)

    @skip_if_lt_x_gpu(2)
    def test_backward_hooks_after_save(self):
        group = dist.distributed_c10d._get_default_group()
        model = self._get_wrapped_model(group, cuda_first=False)
        self._train_for_several_steps(model, num_steps=2, autocast=False)
        state_1 = model.state_dict()
        model.load_state_dict(state_1)
        self._test_output_backward_hooks(model=model)

    def _test_output_backward_hooks(self, model):
        optim = torch.optim.SGD(model.parameters(), lr=0.01, momentum=0.9)
        optim.zero_grad()
        # Inputs always cuda, as computation happes on CUDA device only
        input = model.module.get_input(torch.device("cuda"))
        output = model(*input)
        # this is pre-bwd hook
        self.assertEqual(len(output._backward_hooks), 1)
        loss = model.module.get_loss(input, output).cuda()
        loss.backward()
        # It doesn't get removed
        self.assertEqual(len(output._backward_hooks), 1)
        optim.step()
        self.assertEqual(len(output._backward_hooks), 1)

    @skip_if_lt_x_gpu(2)
    @parametrize("cuda_first", [False, True])
    @parametrize("mixed_precision", [True, False])
    def test_register_functions_called(self, cuda_first, mixed_precision):
        """Tests that _register_{pre|post}_backward_hooks called during forward."""
        group = dist.distributed_c10d._get_default_group()
        mixed_precision = MixedPrecision() if mixed_precision else None
        config = {"mixed_precision": mixed_precision}
        model = self._get_wrapped_model(
            group, config=config, cuda_first=cuda_first
        )
        input = model.module.get_input(torch.device("cuda"))
        model._register_post_backward_hooks = mock.MagicMock(return_value=None)
        model._register_pre_backward_hooks = mock.MagicMock(return_value=None)
        self.assertFalse(model._register_post_backward_hooks.called)
        self.assertFalse(model._register_pre_backward_hooks.called)
        model(*input)
        self.assertTrue(model._register_post_backward_hooks.called)
        self.assertTrue(model._register_pre_backward_hooks.called)


class TestNoGrad(FSDPTest):
    @skip_if_lt_x_gpu(2)
    @parametrize("mixed_precision", [True, False])
    def test_transformer_no_grad(self, mixed_precision):
        group = dist.distributed_c10d._get_default_group()
        mixed_precision = MixedPrecision(
            param_dtype=torch.float16,
            reduce_dtype=torch.float16,
            buffer_dtype=torch.float16,
        ) if mixed_precision else None
        config = {"mixed_precision": mixed_precision}
        model = self._get_wrapped_model(group, config=config, cuda_first=False)
        # Train model for a step
        self._train_for_several_steps(
            model,
            num_steps=1,
            autocast=False,
            mixed_precision=config["mixed_precision"]
        )

        model.eval()  # no dropout for this test

        # Eval in standard mode (i.e., without no_grad)
        input = model.module.get_input(torch.device("cuda"))
        ref_output = model(*input)

        # Eval with no_grad and compare
        with torch.no_grad():
            no_grad_output = model(*input)

        self.assertEqual(ref_output, no_grad_output)


instantiate_parametrized_tests(TestHooks)
instantiate_parametrized_tests(TestParityWithDDP)
instantiate_parametrized_tests(TestNoGrad)
instantiate_parametrized_tests(TestParamInit)

if __name__ == "__main__":
    run_tests()
