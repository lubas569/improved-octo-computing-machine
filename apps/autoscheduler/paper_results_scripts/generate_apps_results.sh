#!/bin/bash

if [ $# -ne 1 ]; then
    echo "Usage: $0 mode [greedy|beam_search]"
    exit
fi

HALIDE=$(dirname $0)/../../..
echo "Using Halide in " $HALIDE

MODE=${1}

if [ $MODE == "greedy" ]; then
    BEAM_SIZE=1
    NUM_PASSES=1
elif [ $MODE == "beam_search" ]; then
    BEAM_SIZE=32
    NUM_PASSES=5
else
    echo "Unknown mode: ${MODE}"
    exit
fi

echo "Using ${MODE} mode with beam_size=${BEAM_SIZE} and num_passes=${NUM_PASSES}"

export HL_BEAM_SIZE=${BEAM_SIZE}
export HL_NUM_PASSES=${NUM_PASSES}

export CXX="ccache ${CXX}"

# Best single set of params for master on the benchmarking machine, found with grid search on the runtime pipelines
# There are already baked into src/AutoSchedule.cpp as the default
export HL_MACHINE_PARAMS=80,24000000,160

export HL_PERMIT_FAILED_UNROLL=1
export HL_WEIGHTS_DIR=${HALIDE}/apps/autoscheduler/gpu_weights
export HL_TARGET=host-cuda

# no random dropout
export HL_RANDOM_DROPOUT=100


RESULTS_DIR="results"/${MODE}

mkdir -p ${RESULTS_DIR}
rm -rf ${RESULTS_DIR}/*

# ablation where we restrict to old space
# export HL_NO_SUBTILING=1

# ablation where instead of coarse to fine, we just enlarge the beam
#export HL_BEAM_SIZE=160
#export HL_NUM_PASSES=1

APPS="bilateral_grid local_laplacian nl_means lens_blur camera_pipe stencil_chain harris hist max_filter unsharp interpolate_generator conv_layer mat_mul_generator iir_blur_generator resnet_50_blockwise bgu"

# Uncomment when there's a change that wouldn't be picked up by the Makefiles (e.g. new weights)
for app in ${APPS}; do make -C ${HALIDE}/apps/${app} clean; done

for app in ${APPS}; do
    echo "Compile" $app
    # The apps sadly do not use consistent names for their test binary, but they're all either 'filter' or 'process'
    if [ -f ${HALIDE}/apps/${app}/filter.cpp ]; then
        make -j32 -C ${HALIDE}/apps/${app} bin/filter 2>${RESULTS_DIR}/stderr_${app}.txt >${RESULTS_DIR}/stdout_${app}.txt &
    else
        make -j32 -C ${HALIDE}/apps/${app} bin/process 2>${RESULTS_DIR}/stderr_${app}.txt >${RESULTS_DIR}/stdout_${app}.txt &
    fi
done
make -C ${HALIDE}/apps/resnet_50_blockwise bin/pytorch_weights/ok > /dev/null 2> /dev/null
wait

# benchmark everything
for app in ${APPS}; do
    echo "Bench" $app
    make -C ${HALIDE}/apps/${app} test > ${RESULTS_DIR}/results_${app}.txt;
done

# Report results
echo "App, Manual (ms), Baseline (ms), Autoscheduler (ms)" >> ${RESULTS_DIR}/results.txt
for app in ${APPS}; do
    if [ $app == "resnet_50_blockwise" ]; then
        M="-1" # There's no manual schedule for resnet
        C=$(grep "schedule_type=classic_auto_schedule"
        ${RESULTS_DIR}/results_resnet_50_blockwise.txt | cut -d" " -f 4 | cut -dm -f 1)
        A=$(grep "schedule_type=_auto_schedule" ${RESULTS_DIR}/results_resnet_50_blockwise.txt | cut -d" " -f 4)
    else
        M=$(grep "Manual" ${RESULTS_DIR}/results_${app}.txt -m 1 | cut -d" " -f3 | cut -dm -f 1)
        C=$(grep "Simple" ${RESULTS_DIR}/results_${app}.txt -m 1 | cut -d" " -f4 | cut -dm -f 1)
        A=$(grep "Auto" ${RESULTS_DIR}/results_${app}.txt -m 1 | cut -d" " -f3 | cut -dm -f 1)
    fi
    echo "$app, $M, $C, $A" >> ${RESULTS_DIR}/results.txt
done

