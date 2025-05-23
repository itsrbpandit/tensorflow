load("//tensorflow:strict.default.bzl", "py_strict_library")
load("//tensorflow:tensorflow.default.bzl", "tf_py_strict_test")
load("//tensorflow/core/platform:distribute.bzl", "distribute_py_strict_test")

package(
    # copybara:uncomment default_applicable_licenses = ["//tensorflow:license"],
    default_visibility = ["//tensorflow:internal"],
    licenses = ["notice"],
)

py_strict_library(
    name = "cluster_coordinator",
    srcs = ["cluster_coordinator.py"],
    deps = [
        ":coordinator_context",
        ":metric_utils",
        ":remote_value",
        ":utils",
        ":values",
        ":watchdog",
        "//tensorflow/python/eager:cancellation",
        "//tensorflow/python/eager:context",
        "//tensorflow/python/eager:def_function",
        "//tensorflow/python/eager:executor",
        "//tensorflow/python/eager:function",
        "//tensorflow/python/framework:errors",
        "//tensorflow/python/framework:func_graph",
        "//tensorflow/python/framework:ops",
        "//tensorflow/python/platform:tf_logging",
        "//tensorflow/python/util:nest",
        "//tensorflow/python/util:tf_export",
        "@six_archive//:six",
    ],
)

py_strict_library(
    name = "coordinator_context",
    srcs = [
        "coordinator_context.py",
    ],
    deps = [
        ":remote_value",
        "//tensorflow/core:protos_all_py",
        "//tensorflow/python/framework:dtypes",
        "//tensorflow/python/framework:ops",
        "//tensorflow/python/framework:tensor",
        "//tensorflow/python/util:compat",
        "//tensorflow/python/util:tf_export",
    ],
)

py_strict_library(
    name = "remote_value",
    srcs = ["remote_value.py"],
    deps = ["//tensorflow/python/util:tf_export"],
)

py_strict_library(
    name = "values",
    srcs = ["values.py"],
    deps = [
        ":remote_value",
        "//tensorflow/python/data/ops:dataset_ops",
        "//tensorflow/python/data/ops:options",
        "//tensorflow/python/distribute:input_lib",
        "//tensorflow/python/eager:context",
        "//tensorflow/python/eager:def_function",
        "//tensorflow/python/eager:function",
        "//tensorflow/python/framework:composite_tensor",
        "//tensorflow/python/framework:errors",
        "//tensorflow/python/framework:ops",
        "//tensorflow/python/framework:type_spec",
        "//tensorflow/python/ops:array_ops",
        "//tensorflow/python/ops:dataset_ops_gen",
        "//tensorflow/python/ops:experimental_dataset_ops_gen",
        "//tensorflow/python/ops:variable_scope",
        "//tensorflow/python/util:nest",
        "//tensorflow/python/util:tf_export",
    ],
)

distribute_py_strict_test(
    name = "cluster_coordinator_test",
    srcs = ["cluster_coordinator_test.py"],
    shard_count = 50,
    tags = [
        # "multi_gpu", # TODO(b/287692888): re-enable once the 2gpu test passes.
        "no_oss",  # TODO(b/214432000): Very flaky under Docker
        "no_pip",
        "noasan",  # TODO(b/171040359): Flaky timeout, even if maximum shards
        "notpu",
        "notsan",  # TODO(b/171040359): Flaky timeout, even if maximum shards
    ],
    xla_enable_strict_auto_jit = False,  # TODO(b/291174864)
    xla_tags = [
        "no_cuda_asan",  # Race condition on async test
    ],
    deps = [
        ":cluster_coordinator",
        ":coordinator_context",
        ":remote_value",
        "//tensorflow/python/compat:v2_compat",
        "//tensorflow/python/data/ops:dataset_ops",
        "//tensorflow/python/distribute:distribute_lib",
        "//tensorflow/python/distribute:distribute_utils",
        "//tensorflow/python/distribute:input_lib",
        "//tensorflow/python/distribute:multi_worker_test_base",
        "//tensorflow/python/distribute:parameter_server_strategy_v2",
        "//tensorflow/python/distribute:sharded_variable",
        "//tensorflow/python/distribute/cluster_resolver:base_cluster_resolver_py",
        "//tensorflow/python/eager:cancellation",
        "//tensorflow/python/eager:def_function",
        "//tensorflow/python/eager:test",
        "//tensorflow/python/framework:constant_op",
        "//tensorflow/python/framework:dtypes",
        "//tensorflow/python/framework:errors",
        "//tensorflow/python/framework:random_seed",
        "//tensorflow/python/framework:sparse_tensor",
        "//tensorflow/python/framework:test_lib",
        "//tensorflow/python/ops:array_ops",
        "//tensorflow/python/ops:check_ops",
        "//tensorflow/python/ops:embedding_ops",
        "//tensorflow/python/ops:math_ops",
        "//tensorflow/python/ops:random_ops",
        "//tensorflow/python/ops:resource_variable_ops",
        "//tensorflow/python/ops:variable_scope",
        "//tensorflow/python/ops:variables",
        "//tensorflow/python/platform:tf_logging",
        "//tensorflow/python/training:coordinator",
        "//tensorflow/python/training:server_lib",
        "@absl_py//absl/testing:parameterized",
    ],
)

py_strict_library(
    name = "fault_tolerance_test_base",
    srcs = ["fault_tolerance_test_base.py"],
    deps = [
        ":cluster_coordinator",
        "//tensorflow/python/data/ops:dataset_ops",
        "//tensorflow/python/distribute:multi_worker_test_base",
        "//tensorflow/python/distribute:parameter_server_strategy_v2",
        "//tensorflow/python/distribute:test_util",
        "//tensorflow/python/distribute/cluster_resolver:base_cluster_resolver_py",
        "//tensorflow/python/eager:context",
        "//tensorflow/python/eager:def_function",
        "//tensorflow/python/framework:dtypes",
        "//tensorflow/python/framework:errors",
        "//tensorflow/python/framework:ops",
        "//tensorflow/python/framework:tensor",
        "//tensorflow/python/ops:array_ops",
        "//tensorflow/python/ops:check_ops",
        "//tensorflow/python/ops:math_ops",
        "//tensorflow/python/ops:random_ops",
        "//tensorflow/python/ops:variables",
        "//tensorflow/python/platform:tf_logging",
        "//tensorflow/python/training:coordinator",
        "//tensorflow/python/training:server_lib",
    ],
)

tf_py_strict_test(
    name = "fault_tolerance_test",
    srcs = ["fault_tolerance_test.py"],
    shard_count = 41,  # = number of tests, so one shard = one test
    tags = [
        "no_oss",  # TODO(b/219580021)
        "noasan",  # Multi-process runner does not work with test sanitizers
        "nomac",  # TODO(b/177065434)
        "nomsan",
    ],
    deps = [
        ":cluster_coordinator",
        ":fault_tolerance_test_base",
        "//tensorflow/python/compat:v2_compat",
        "//tensorflow/python/distribute:multi_process_runner",
        "//tensorflow/python/distribute:multi_worker_test_base",
        "//tensorflow/python/distribute:parameter_server_strategy_v2",
        "//tensorflow/python/distribute/cluster_resolver:base_cluster_resolver_py",
        "//tensorflow/python/eager:test",
        "//tensorflow/python/training:coordinator",
        "//tensorflow/python/training:server_lib",
    ],
)

tf_py_strict_test(
    name = "fault_tolerance_coordination_service_test",
    srcs = ["fault_tolerance_coordination_service_test.py"],
    shard_count = 41,
    tags = [
        # Inherit tags from fault_tolerance_test
        "no_oss",  # TODO(b/219580021)
        "noasan",  # Multi-process runner does not work with test sanitizers
        "nomac",  # TODO(b/177065434)
        "nomsan",  # TODO(b/290211396)
        "notap",
    ],
    deps = [
        ":cluster_coordinator",
        ":fault_tolerance_test_base",
        "//tensorflow/python/compat:v2_compat",
        "//tensorflow/python/distribute:multi_process_runner",
        "//tensorflow/python/eager:def_function",
        "//tensorflow/python/eager:test",
    ],
)

py_strict_library(
    name = "metric_utils",
    srcs = ["metric_utils.py"],
    deps = [
        "//tensorflow/python/eager:monitoring",
        "//tensorflow/python/util:tf_contextlib",
    ],
)

tf_py_strict_test(
    name = "metric_utils_test",
    srcs = ["metric_utils_test.py"],
    shard_count = 3,
    deps = [
        ":cluster_coordinator",
        ":metric_utils",
        "//tensorflow/python/distribute:multi_process_runner",
        "//tensorflow/python/distribute:multi_worker_test_base",
        "//tensorflow/python/distribute:parameter_server_strategy_v2",
        "//tensorflow/python/distribute/cluster_resolver:base_cluster_resolver_py",
        "//tensorflow/python/eager:context",
        "//tensorflow/python/eager:def_function",
        "//tensorflow/python/eager:test",
        "//tensorflow/python/ops:math_ops",
        "//tensorflow/python/ops:random_ops",
        "//tensorflow/python/training:coordinator",
        "//tensorflow/python/training:server_lib",
    ],
)

py_strict_library(
    name = "utils",
    srcs = ["utils.py"],
    deps = [
        "//tensorflow/python/platform:tf_logging",
        "//tensorflow/python/training:server_lib",
    ],
)

py_strict_library(
    name = "remote_eager_lib",
    visibility = ["//visibility:public"],
)

py_strict_library(
    name = "watchdog",
    srcs = ["watchdog.py"],
    deps = ["@absl_py//absl/logging"],
)

tf_py_strict_test(
    name = "watchdog_test",
    srcs = ["watchdog_test.py"],
    tags = [
        "nomac",  # TODO(b/239433962)
    ],
    deps = [
        ":watchdog",
        "//tensorflow/python/eager:test",
        "@absl_py//absl/testing:parameterized",
    ],
)

tf_py_strict_test(
    name = "get_task_states_test",
    srcs = ["get_task_states_test.py"],
    shard_count = 3,
    tags = [
        "no_oss",  # TODO(b/219580021)
        "nomac",  # TODO(b/177065434)
    ],
    deps = [
        ":cluster_coordinator",
        ":utils",
        "//tensorflow/core:protos_all_py",
        "//tensorflow/python/compat:v2_compat",
        "//tensorflow/python/distribute:multi_process_runner",
        "//tensorflow/python/distribute:multi_worker_test_base",
        "//tensorflow/python/distribute:parameter_server_strategy_v2",
        "//tensorflow/python/distribute/cluster_resolver:base_cluster_resolver_py",
        "//tensorflow/python/eager:context",
        "//tensorflow/python/eager:test",
        "//tensorflow/python/framework:errors",
        "//tensorflow/python/training:server_lib",
    ],
)
