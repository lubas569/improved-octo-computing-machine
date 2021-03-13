#!/bin/bash

function find_halide() {
    local -n halide_root_ref=$1
    local dir=$(pwd)

    for i in {1..5}; do
        if [[ -f ${dir}/distrib/include/Halide.h ]]; then
            halide_root_ref=$(cd ${dir}; pwd)
            echo "Using Halide in ${halide_root_ref}"
            return 0
        fi
        dir=${dir}/..
    done

    echo "Unable to find Halide. Try re-running $(basename $0) from somewhere in the
    Halide tree."
    exit
}

function make_dir_path_absolute() {
    local -r path=$1
    local -n absolute_path_ref=$2
    absolute_path_ref=$(cd ${path}; pwd)
}

function make_file_path_absolute() {
    local -r path=$1
    local -n converted_path_ref=$2
    converted_path_ref=$(cd $(dirname ${path}); pwd)/$(basename ${path})
}

function get_autoscheduler_dir() {
    local -r halide_root=$1
    local -n autoscheduler_dir_ref=$2
    autoscheduler_dir_ref=${halide_root}/apps/autoscheduler
}

function get_autoscheduler_bin_dir() {
    local -r halide_root=$1
    local -n autoscheduler_bin_dir_ref=$2
    get_autoscheduler_dir $halide_root autoscheduler_dir
    autoscheduler_bin_dir_ref=${autoscheduler_dir}/bin
}

function build_train_cost_model() {
    local -r halide_root=$1
    get_autoscheduler_dir $halide_root autoscheduler_dir

    echo
    echo "Building train_cost_model"
    make -C ${autoscheduler_dir}/../autoscheduler ../autoscheduler/bin/train_cost_model
    echo
}

function train_cost_model() {
    local -r halide_root=$1
    local -r samples_dir=$2
    local -r weights_dir=$3
    local -r num_cores=$4
    local -r num_epochs=$5
    local -r best_schedule_file=$6
    local -r predictions_file=$7

    get_autoscheduler_bin_dir ${halide_root} autosched_bin

    find ${samples_dir} | grep sample$ | HL_NUM_THREADS=${num_cores} HL_WEIGHTS_DIR=${weights_dir} HL_BEST_SCHEDULE_FILE=${best_schedule_file} PREDICTIONS_FILE=${predictions_file} ${autosched_bin}/train_cost_model ${num_epochs} 0.0001
}

function find_equal_predicted_pairs() {
    local -r limit=$1
    sort ${2} -k2 -n | awk -F', ' -f find_equal_predicted_pairs.awk | sort -k6 -n -r | head -n ${limit}
}

function find_similar_predicted_pairs() {
    local -r limit=$1
    sort ${2} -k2 -n | awk -F', ' -f find_similar_predicted_pairs.awk | sort -k9 -n -r | head -n ${limit}
}

function get_timeout_cmd() {
    local -n timeout_cmd_ref=$1

    timeout_cmd_ref="timeout"
    if [ $(uname -s) = "Darwin" ] && ! which $timeout_cmd_ref 2>&1 >/dev/null; then
        # OSX doesn't have timeout; gtimeout is equivalent and available via Homebrew
        timeout_cmd_ref="gtimeout"
        if ! which $timeout_cmd_ref 2>&1 >/dev/null; then
            echo "Can't find the command 'gtimeout'. Run 'brew install coreutils' to install it."
            exit 1
        fi
    fi
}

function profile_gpu_sample() {
    local -r sample_dir=$1
    local -r output_dir=$2

    local -r num_cores=80
    local -r timeout=60s
    get_timeout_cmd timeout_cmd

    if [ ! -f ${sample_dir}/bench ]; then
        echo "${sample_dir}/bench not found."
        return 1
    fi

    local -r batch_id=$(basename $(dirname ${sample_dir}))
    local -r sample_id=$(basename ${sample_dir})
    local -r prefix=${batch_id}_sample_${sample_id}

    nvprof_timeline_cmd="HL_NUM_THREADS=${num_cores} \
        ${timeout_cmd} -k ${timeout} ${timeout} \
        nvprof -f --output-profile ${output_dir}/${prefix}_timeline.nvprof \
        ${sample_dir}/bench \
        --output_extents=estimate \
        --default_input_buffers=random:0:estimate_then_auto \
        --default_input_scalars=estimate \
        --benchmarks=all \
        --benchmark_max_iters=1"

    local -r nvprof_metrics_cmd="HL_NUM_THREADS=${num_cores} \
        ${timeout_cmd} -k ${timeout} ${timeout} \
        nvprof -f --analysis-metrics -o ${output_dir}/${prefix}_metrics.nvprof \
        ${sample_dir}/bench \
        --output_extents=estimate \
        --default_input_buffers=random:0:estimate_then_auto \
        --default_input_scalars=estimate \
        --benchmarks=all \
        --benchmark_max_iters=1"

    eval "${nvprof_timeline_cmd} && ${nvprof_metrics_cmd}"
    return 0
}
