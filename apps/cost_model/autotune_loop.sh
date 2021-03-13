set -x

# Make sure Halide is built
# make -C ../../ distrib -j

# Build the generator to autotune. This script will be autotuning the
# autoscheduler's cost model training pipeline, which is large enough
# to be interesting.
GENERATOR=./cost_model.generator
PIPELINE=halide_autoscheduler_train_cost_model 
c++ ../../src/CostModelGenerator.cpp -std=c++11 -DCOMPILING_SEPARATELY -I ../../include/ ../../lib/libHalide.a  -o cost_model.generator -ldl -lpthread -lz -fno-rtti -fno-exceptions ../../tools/GenGen.cpp

# Build some tools we need. Should really add root-level makefile targets for these.
c++ ../../tools/augment_sample.cpp -o augment_sample
c++ ../../tools/train_cost_model.cpp -std=c++11 -I ../../src -L ../../bin -lHalide -o train_cost_model

# A batch of this many samples is built in parallel, and then
# benchmarked serially. Set to number of cores.
BATCH_SIZE=80

# Build a single sample of the pipeline with a random schedule
make_sample() {
    D=${1}
    mkdir -p ${D}
    rm -f "${D}/sample.sample"
    if [[ $D == */0 ]]; then
        # Sample 0 in each batch is best effort beam search, with no randomness
        HL_SEED=${2} HL_FEATURE_FILE=${D}/sample.sample HL_WEIGHTS_DIR=${PWD}/weights HL_RANDOM_DROPOUT=100 HL_BEAM_SIZE=50 ${GENERATOR} -g ${PIPELINE} -o ${D} target=host-new_autoscheduler auto_schedule=true 2> ${D}/compile_log.txt
    else
        # The other samples are random probes biased by the cost model
        HL_SEED=${2} HL_FEATURE_FILE=${D}/sample.sample HL_WEIGHTS_DIR=${PWD}/weights HL_RANDOM_DROPOUT=90 HL_BEAM_SIZE=1 ${GENERATOR} -g ${PIPELINE} -o ${D} target=host-new_autoscheduler auto_schedule=true 2> ${D}/compile_log.txt
    fi
    
    c++ -std=c++11 -DHL_RUNGEN_FILTER_HEADER="\"${D}/${PIPELINE}.h\"" -I ../../include ../../tools/RunGenMain.cpp ../../tools/RunGenStubs.cpp  ${D}/*.a -o ${D}/bench -ljpeg -ldl -lpthread -lz -lpng    
}

# Benchmark one of the random samples
benchmark_sample() {
    D=${1}
    HL_NUM_THREADS=16 numactl -N0 ${D}/bench --verbose --output_extents=estimate --default_input_buffers=random:0:estimate_then_auto --default_input_scalars=estimate --benchmarks=all --benchmark_min_time=1 ${RUNGEN_ARGS} | tee ${D}/bench.txt

    # Add the runtime, pipeline id, and schedule id to the feature file
    R=$(cut -d' ' -f8 < ${D}/bench.txt)
    P=0
    S=$2
    ./augment_sample ${D}/sample.sample $R $P $S
}

# Don't clobber existing samples
FIRST=$(ls samples | cut -d_ -f2 | sort -n | tail -n1)

for ((i=$((FIRST+1));i<1000000;i++)); do
    # Compile a batch of samples using the generator in parallel
    DIR=${PWD}/samples/batch_${i}

    for ((b=0;b<${BATCH_SIZE};b++)); do
        echo Compiling sample $b
        S=$(printf "%d%02d" $i $b)
        make_sample "${DIR}/${b}" $S 1 &
    done
    wait
    
    # benchmark them serially using rungen
    for ((b=0;b<${BATCH_SIZE};b++)); do
        echo Benchmarking sample $b
        S=$(printf "%d%02d" $i $b)
        benchmark_sample "${DIR}/${b}" $S
    done
    
    # retrain model weights on all samples seen so far
    echo Retraining model...
    find samples | grep sample$ | HL_WEIGHTS_DIR=weights LD_LIBRARY_PATH=../../bin ./train_cost_model 1000
    
done
