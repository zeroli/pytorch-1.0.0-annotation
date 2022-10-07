#!/usr/bin/env python

from __future__ import absolute_import, division, print_function
import os
import sys
import subprocess
import argparse
from functools import reduce

from pyHIPIFY import hipify_python

parser = argparse.ArgumentParser(description='Top-level script for HIPifying, filling in most common parameters')
parser.add_argument(
    '--out-of-place-only',
    action='store_true',
    help="Whether to only run hipify out-of-place on source files")
args = parser.parse_args()

amd_build_dir = os.path.dirname(os.path.realpath(__file__))
proj_dir = os.path.join(os.path.dirname(os.path.dirname(amd_build_dir)))

includes = [
    "caffe2/operators/*",
    "caffe2/sgd/*",
    "caffe2/image/*",
    "caffe2/transforms/*",
    "caffe2/video/*",
    "caffe2/distributed/*",
    "caffe2/queue/*",
    "binaries/*",
    "caffe2/**/*_test*",
    "caffe2/core/*",
    "caffe2/db/*",
    "caffe2/utils/*",
    "c10/cuda/*",
    # PyTorch paths
    # Keep this synchronized with is_pytorch_file in hipify_python.py
    "aten/*",
    "torch/*",
]

ignores = [
    "caffe2/operators/depthwise_3x3_conv_op_cudnn.cu",
    "caffe2/operators/pool_op_cudnn.cu",
    '**/hip/**',
    "aten/src/ATen/core/*",
]

json_file = ""  # Yeah, don't ask me why the default is ""...
if not args.out_of_place_only:
    # List of operators currently disabled (PyTorch only)
    json_file = os.path.join(amd_build_dir, "disabled_features.json")

    # Apply patch files in place (PyTorch only)
    patch_folder = os.path.join(amd_build_dir, "patches")
    for filename in os.listdir(os.path.join(amd_build_dir, "patches")):
        subprocess.Popen(["git", "apply", os.path.join(patch_folder, filename)], cwd=proj_dir)

    # Make various replacements inside AMD_BUILD/torch directory
    ignore_files = ["csrc/autograd/profiler.h", "csrc/autograd/profiler.cpp",
                    "csrc/cuda/cuda_check.h"]
    for root, _directories, files in os.walk(os.path.join(proj_dir, "torch")):
        for filename in files:
            if filename.endswith(".cpp") or filename.endswith(".h"):
                source = os.path.join(root, filename)
                # Disabled files
                if reduce(lambda result, exclude: source.endswith(exclude) or result, ignore_files, False):
                    continue
                # Update contents.
                with open(source, "r+") as f:
                    contents = f.read()
                    contents = contents.replace("USE_CUDA", "USE_ROCM")
                    contents = contents.replace("CUDA_VERSION", "0")
                    f.seek(0)
                    f.write(contents)
                    f.truncate()
                    f.flush()
                    os.fsync(f)

hipify_python.hipify(
    project_directory=proj_dir,
    output_directory=proj_dir,
    includes=includes,
    ignores=ignores,
    out_of_place_only=args.out_of_place_only,
    json_settings=json_file,
    add_static_casts_option=True)
