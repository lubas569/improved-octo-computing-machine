HALIDE=$(dirname $0)/../../..
echo "Using Halide in " $HALIDE

export CXX="ccache c++"

# Best single set of params for master on the benchmarking machine, found with grid search on the runtime pipelines
# There are already baked into src/AutoSchedule.cpp as the default
# export HL_MACHINE_PARAMS=32,24000000,160

export HL_PERMIT_FAILED_UNROLL=1
export HL_WEIGHTS_DIR=${HALIDE}/apps/autoscheduler/weights
export HL_TARGET=x86-64-avx2

# no random dropout
export HL_RANDOM_DROPOUT=100

# greedy
# export HL_BEAM_SIZE=1
# export HL_NUM_PASSES=1

# beam search
export HL_BEAM_SIZE=32
export HL_NUM_PASSES=5

# ablation where we restrict to old space
# export HL_NO_SUBTILING=1

# ablation where instead of coarse to fine, we just enlarge the beam
# export HL_BEAM_SIZE=160
# export HL_NUM_PASSES=1

APPS="bilateral_grid local_laplacian nl_means lens_blur camera_pipe stencil_chain harris hist max_filter unsharp interpolate_generator conv_layer mat_mul_generator iir_blur_generator resnet_50_blockwise bgu"

# Uncomment when there's a change that wouldn't be picked up by the Makefiles (e.g. new weights)
for app in ${APPS}; do make -C ${HALIDE}/apps/${app} clean; done

make -j -C ${HALIDE}/apps/bilateral_grid bin/filter
make -j -C ${HALIDE}/apps/local_laplacian bin/process
make -j -C ${HALIDE}/apps/nl_means bin/process
make -j -C ${HALIDE}/apps/lens_blur bin/process 
make -j -C ${HALIDE}/apps/camera_pipe bin/process 
make -j -C ${HALIDE}/apps/stencil_chain bin/process 
make -j -C ${HALIDE}/apps/harris bin/filter
make -j -C ${HALIDE}/apps/hist bin/filter
make -j -C ${HALIDE}/apps/max_filter bin/filter
make -j -C ${HALIDE}/apps/unsharp bin/filter
make -j -C ${HALIDE}/apps/interpolate_generator bin/filter
make -j -C ${HALIDE}/apps/conv_layer bin/process
make -j -C ${HALIDE}/apps/mat_mul_generator bin/filter
make -j -C ${HALIDE}/apps/iir_blur_generator bin/process
make -j -C ${HALIDE}/apps/resnet_50_blockwise test
make -j -C ${HALIDE}/apps/bgu bin/process

# benchmark everything
for app in ${APPS}; do make -C ${HALIDE}/apps/${app} test; done
