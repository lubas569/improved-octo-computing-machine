#include <cmath>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>
#include <map>
#include <iostream>
#include <fstream>
#include <random>

#include "CostModel.h"
#include "NetworkSize.h"

namespace {

using namespace Halide;

using std::vector;
using std::string;
using std::map;
using std::set;

const int models = 1;

struct Sample {
    vector<float> runtimes;
    double prediction[models];
    string filename;
    int32_t schedule_id;
    uint64_t schedule_hash;
    Runtime::Buffer<float> schedule_features;
};

struct Pipeline {
    int32_t pipeline_id;
    int32_t num_stages;
    Runtime::Buffer<float> pipeline_features;
    uint64_t pipeline_hash;
};

struct PipelineSample {
    map<uint64_t, Sample> schedules;
    uint64_t fastest_schedule;
    float fastest_runtime{1e30f};
};

uint64_t hash_floats(uint64_t h, float *begin, float *end) {
    while (begin != end) {
        uint32_t bits = *((uint32_t *)begin);
        // From boost
        h ^= (bits + 0x9e3779b9 + (h<<6) + (h>>2));
        begin++;
    }
    return h;
}

bool ends_with(const string &str, const string &suffix) {
    if (str.size() < suffix.size()) return false;
    size_t off = str.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); i++) {
        if (str[off+i] != suffix[i]) return false;
    }
    return true;
}

// Load all the samples, reading filenames from stdin
void load_samples(map<int, PipelineSample>& training_set, map<int, PipelineSample>& validation_set, map<int, Pipeline>& pipelines, bool verbose_mode, bool partition_schedules) {
    vector<float> scratch(10 * 1024 * 1024);

    int best = -1;
    float best_runtime = 1e20f;
    string best_path;

    int truncated_samples = 0;
    int empty_samples = 0;

    std::cout << "Loading samples...\n";

    size_t num_read = 0, num_unique = 0;
    while (!std::cin.eof()) {
        string s;
        std::cin >> s;
        if (s.empty()) {
            continue;
        }
        if (!ends_with(s, ".sample")) {
            std::cout << "Skipping file: " << s << "\n";
            continue;
        }

        std::ifstream file(s);
        file.read((char *)(scratch.data()), scratch.size() * sizeof(float));
        const size_t floats_read = file.gcount() / sizeof(float);
        const size_t num_features = floats_read - 3;
        const size_t features_per_stage = head2_w + (head1_w + 1) * head1_h;
        file.close();
        // Note we do not check file.fail(). The various failure cases
        // are handled below by checking the number of floats read. We
        // expect truncated files if the benchmarking or
        // autoscheduling procedure crashes and want to filter them
        // out with a warning.

        if (floats_read == scratch.size()) {
            std::cout << "Too-large sample: " << s << " " << floats_read << "\n";
            continue;
        }
        if (num_features % features_per_stage != 0) {
            if (verbose_mode) {
                std::cout << "Truncated sample: " << s << " " << floats_read << "\n";
            }
            ++truncated_samples;
            continue;
        }

        if (num_features <= 0) {
            if (verbose_mode) {
                std::cout << "Empty sample: " << s << " " << floats_read << "\n";
            }
            ++empty_samples;
            continue;
        }

        const size_t num_stages = num_features / features_per_stage;

        const float runtime = scratch[num_features];
        if (runtime > 100000) { // Don't try to predict runtime over 100s
            std::cout << "Implausible runtime in ms: " << runtime << "\n";
            continue;
        }
        // std::cout << "Runtime: " << runtime << "\n";

        const int pipeline_id = *((int32_t *)(&scratch[num_features + 1]));
        const int schedule_id = *((int32_t *)(&scratch[num_features + 2]));

        if (runtime < best_runtime) {
            best_runtime = runtime;
            best = schedule_id;
            best_path = s;
        }

        Pipeline &p = pipelines[pipeline_id];

        if (p.pipeline_features.data() == nullptr) {
            p.pipeline_id = pipeline_id;
            p.num_stages = (int)num_stages;
            p.pipeline_features = Runtime::Buffer<float>(head1_w, head1_h, num_stages);
            for (size_t i = 0; i < num_stages; i++) {
                for (int x = 0; x < head1_w; x++) {
                    for (int y = 0; y < head1_h; y++) {
                        float f = scratch[i * features_per_stage + (x + 1) * 7 + y + head2_w];
                        if (f < 0 || std::isnan(f)) {
                            std::cout << "Negative or NaN pipeline feature: " << s << " " << x << " " << y << " " << i << " " << f << "\n";
                        }
                        p.pipeline_features(x, y, i) = f;
                    }
                }
            }

            p.pipeline_hash = hash_floats(0, p.pipeline_features.begin(), p.pipeline_features.end());
        }

        uint64_t schedule_hash = 0;
        for (size_t i = 0; i < num_stages; i++) {
            schedule_hash =
                hash_floats(schedule_hash,
                            &scratch[i * features_per_stage],
                            &scratch[i * features_per_stage + head2_w]);
        }

        uint64_t hash = partition_schedules ? schedule_hash : p.pipeline_hash;

        // Whether or not a pipeline/schedule is part of the validation set
        // can't be a call to rand. It must be a fixed property of a
        // hash of some aspect of it.  This way you don't accidentally
        // do a training run where a validation set member was in the
        // training set of a previous run. The id of the fastest
        // schedule will do as a hash.
        PipelineSample &ps = ((hash & 7) == 0) ? validation_set[pipeline_id] : training_set[pipeline_id];

        auto it = ps.schedules.find(schedule_hash);
        if (it != ps.schedules.end()) {
            // Keep the smallest runtime at the front
            float best = it->second.runtimes[0];
            if (runtime < best) {
                it->second.runtimes.push_back(best);
                it->second.runtimes[0] = runtime;
                it->second.filename = s;
            } else {
                it->second.runtimes.push_back(runtime);
            }
            if (runtime < ps.fastest_runtime) {
                ps.fastest_runtime = runtime;
                ps.fastest_schedule = schedule_hash;
            }
        } else {
            Sample sample;
            sample.schedule_hash = schedule_hash;
            sample.filename = s;
            sample.runtimes.push_back(runtime);
            for (int i = 0; i < models; i++) {
                sample.prediction[i] = 0.0;
            }
            sample.schedule_id = schedule_id;
            sample.schedule_features = Runtime::Buffer<float>(head2_w, num_stages);

            bool ok = true;
            for (size_t i = 0; i < num_stages; i++) {
                for (int x = 0; x < head2_w; x++) {
                    float f = scratch[i * features_per_stage + x];
                    if (f < 0 || f > 1e14 || std::isnan(f)) {
                        std::cout << "Negative or implausibly large schedule feature: " << i << " " << x << " " << f << "\n";
                        // Something must have overflowed
                        ok = false;
                    }
                    sample.schedule_features(x, i) = f;
                }
                /*
                if (sample.schedule_features(0, i) != sample.schedule_features(1, i)) {
                    std::cout << "Rejecting sliding window schedule for now\n";
                    ok = false;
                }
                */
            }
            if (ok) {
                if (runtime < ps.fastest_runtime) {
                    ps.fastest_runtime = runtime;
                    ps.fastest_schedule = schedule_hash;
                }
                ps.schedules.emplace(schedule_hash, std::move(sample));
                num_unique++;
            }
        }
        num_read++;

        if (num_read % 10000 == 0) {
            std::cout << "Samples loaded: " << num_read << " valid (" << num_unique << " unique); " << truncated_samples << " truncated; " << empty_samples << " empty\n";
        }
    }

    std::cout << "Samples loaded: " << num_read << " valid (" << num_unique << " unique); " << truncated_samples << " truncated; " << empty_samples << " empty\n";

    // If the training set is empty, we are likely training on a single pipeline
    if (training_set.empty()) {
        training_set.swap(validation_set);
    }

    // Check the noise level
    for (const auto &pipe : training_set) {
        double variance_sum = 0;
        size_t count = 0;
        // Compute the weighted average of variances across all samples
        for (const auto &p : pipe.second.schedules) {
            if (p.second.runtimes.empty()) {
                std::cerr << "Empty runtimes for schedule: " << p.first << "\n";
                abort();
            }
            if (verbose_mode) {
                std::cout << "Unique sample: " << p.second.filename << " : " << p.second.runtimes[0] << "\n";
            }
            if (p.second.runtimes.size() > 1) {
                // Compute variance from samples
                double mean = 0;
                for (float f : p.second.runtimes) {
                    mean += f;
                }
                mean /= p.second.runtimes.size();
                double variance = 0;
                for (float f : p.second.runtimes) {
                    f -= mean;
                    variance += f * f;
                }
                variance_sum += variance;
                count += p.second.runtimes.size() - 1;
            }
        }
        if (count > 0) {
            double stddev = std::sqrt(variance_sum / count);
            std::cout << "Noise level: " << stddev << "\n";
        }
    }

    std::cout << "Distinct pipelines: " << training_set.size() << "\n";

    std::ostringstream o;
    o << "Best runtime is " << best_runtime << ", from schedule id "<< best << " in file " << best_path << "\n";
    std::cout << o.str();
    if (char *e = getenv("HL_BEST_SCHEDULE_FILE")) {
        if (e && *e) {
            std::ofstream f(e, std::ios_base::trunc);
            f << o.str();
            f.close();
            assert(!f.fail());
        }
    }
}

string getenv_safe(const char *key) {
    const char *value = getenv(key);
    if (!value) value = "";
    return value;
}

void print_statistics(const map<int, PipelineSample>& training_set, const map<int, PipelineSample>& validation_set) {
    int64_t num_training_set_schedules = 0;
    int64_t num_val_set_schedules = 0;

    for (const auto& ps : training_set) {
        num_training_set_schedules += ps.second.schedules.size();
    }

    for (const auto& ps : validation_set) {
        num_val_set_schedules += ps.second.schedules.size();
    }

    std::cout << "Training set: "
        << training_set.size()
        << " pipelines, "
        << num_training_set_schedules
        << " schedules. Validation set: "
        << validation_set.size()
        << " pipelines, "
        << num_val_set_schedules
        << " schedules.\n";
}

void save_predictions(const map<int, PipelineSample>& samples, const string& filename) {
    std::ostringstream out;

    for (const auto &p : samples) {
        for (const auto &sched : p.second.schedules) {
            out << sched.second.filename << ", " << sched.second.prediction[0] << ", " << sched.second.runtimes[0] << "\n";
        }
    }

    std::ofstream file(filename, std::ios_base::trunc);
    file << out.str();
    file.close();
    assert(!file.fail());

    std::cout << "Predictions saved to: " << filename << "\n";
}


}  // namespace

int main(int argc, char **argv) {

    using std::string;

    string verbose_mode_str = getenv_safe("VERBOSE");
    bool verbose_mode = verbose_mode_str == "1";

    string partition_schedules_mode_str = getenv_safe("PARTITION_SCHEDULES");
    bool partition_schedules_mode = partition_schedules_mode_str == "1";

    string predictions_file = getenv_safe("PREDICTIONS_FILE");
    bool predict_only = !predictions_file.empty();

    if (predict_only) {
        std::cout << "Predicting only (no training)\n";
    }

    map<int, PipelineSample> samples;
    map<int, PipelineSample> validation_set;
    map<int, Pipeline> pipelines;
    load_samples(samples, validation_set, pipelines, verbose_mode, partition_schedules_mode);
    print_statistics(samples, validation_set);

    if (samples.empty()) {
        std::cout << "No samples found. Exiting.\n";
        return 0;
    }

    string randomize_weights_str = getenv_safe("HL_RANDOMIZE_WEIGHTS");
    bool randomize_weights = randomize_weights_str == "1";
    string weights_in_dir = getenv_safe("HL_WEIGHTS_DIR");
    string weights_out_dir = getenv_safe("HL_WEIGHTS_OUT_DIR");
    if (weights_out_dir.empty()) {
        weights_out_dir = weights_in_dir;
    }

    // Iterate through the pipelines
    vector<std::unique_ptr<CostModel>> tpp;
    for (int i = 0; i < models; i++) {
        tpp.emplace_back(CostModel::make_default(weights_in_dir, weights_out_dir, randomize_weights));
    }

    int num_cores = atoi(getenv_safe("HL_NUM_THREADS").c_str());
    std::cout << "Using num cores = " << num_cores << "\n";

    int epochs = atoi(argv[1]);
    if (predict_only) {
        epochs = 1;
    }

    std::cout.setf(std::ios::fixed, std::ios::floatfield);
    std::cout.precision(4);

    auto seed = time(NULL);
    std::mt19937 rng((uint32_t) seed);

    std::cout << "Iterating over " << samples.size() << " pipelines using seed = " << seed << "\n";

    std::vector<float> rates;
    if (argc == 2) {
        rates.push_back(0.0001f);
    } else {
        for (int i = 2; i < argc; i++) {
            rates.push_back(std::atof(argv[i]));
        }
    }

    std::ifstream file(weights_out_dir + "/best/rate");
    float best_rate_overall = 0.f;
    if (file) {
        std::string line;
        std::getline(file, line);
        best_rate_overall = stof(line);
    }
    std::cout << "Initial best validation rate: " << best_rate_overall << std::endl;
    file.close();

    for (float learning_rate : rates) {
        float loss_sum[models] = {0}, loss_sum_counter[models] = {0};
        float correct_ordering_rate_sum[models] = {0};
        float correct_ordering_rate_count[models] = {0};
        float v_correct_ordering_rate_sum[models] = {0};
        float v_correct_ordering_rate_count[models] = {0};

        for (int e = 0; e < epochs; e++) {
            int counter = 0;

            float worst_miss = 0;
            uint64_t worst_miss_pipeline_id = 0;
            uint64_t worst_miss_schedule_id = 0;

            struct Inversion {
                int pipeline_id;
                string f1, f2;
                float p1, p2;
                float r1, r2;
                float badness = 0;
            } worst_inversion;

            #pragma omp parallel for
            for (int model = 0; model < models; model++) {
                for (int train = 0; train < 2; train++) {
                    auto &tp = tpp[model];

                    for (auto &p : train ? samples : validation_set) {
                        if (models > 1 && rng() & 1) continue; // If we are training multiple models, allow them to diverge.
                        if (p.second.schedules.size() < 8 && !predict_only) {
                            continue;
                        }
                        tp->reset();

                        const auto& pipeline = pipelines[p.first];
                        tp->set_pipeline_features(pipeline.pipeline_features, num_cores);

                        size_t max_batch_size = predict_only ? p.second.schedules.size() : 1024;
                        size_t batch_size = std::min(max_batch_size, p.second.schedules.size());

                        size_t fastest_idx = 0;
                        Runtime::Buffer<float> runtimes(batch_size);

                        size_t first = 0;
                        if (p.second.schedules.size() > max_batch_size) {
                            first = rng() % (p.second.schedules.size() - max_batch_size);
                        }

                        auto it = p.second.schedules.begin();
                        std::advance(it, first);
                        for (size_t j = 0; j < batch_size; j++) {
                            auto &sched = it->second;
                            Runtime::Buffer<float> buf;
                            tp->enqueue(pipeline.num_stages, &buf, &sched.prediction[model]);
                            runtimes(j) = sched.runtimes[0];
                            if (runtimes(j) < runtimes(fastest_idx)) {
                                fastest_idx = j;
                            }
                            buf.copy_from(sched.schedule_features);
                            it++;
                        }

                        float loss = 0.0f;
                        if (train && !predict_only) {
                            loss = tp->backprop(runtimes, learning_rate);
                            assert(!std::isnan(loss));
                            loss_sum[model] += loss;
                            loss_sum_counter[model] ++;

                            auto it = p.second.schedules.begin();
                            std::advance(it, first);
                            for (size_t j = 0; j < batch_size; j++) {
                                auto &sched = it->second;
                                float m = sched.runtimes[0] / (sched.prediction[model] + 1e-10f);
                                if (m > worst_miss) {
                                    worst_miss = m;
                                    worst_miss_pipeline_id = p.first;
                                    worst_miss_schedule_id = it->first;
                                }
                                it++;
                            }
                        } else {
                            tp->evaluate_costs();
                        }

                        if (true) {
                            int good = 0, bad = 0;
                            for (auto &sched : p.second.schedules) {
                                auto &ref = p.second.schedules[p.second.fastest_schedule];
                                if (sched.second.prediction[model] == 0) continue;
                                assert(sched.second.runtimes[0] >= ref.runtimes[0]);
                                float runtime_ratio = sched.second.runtimes[0] / ref.runtimes[0];
                                if (runtime_ratio <= 1.3f) continue; // Within 30% of the runtime of the best
                                if (sched.second.prediction[model] >= ref.prediction[model]) {
                                    good++;
                                } else {
                                    if (train) {
                                        float badness = (sched.second.runtimes[0] - ref.runtimes[0]) * (ref.prediction[model] - sched.second.prediction[model]);
                                        badness /= (ref.runtimes[0] * ref.runtimes[0]);
                                        if (badness > worst_inversion.badness) {
                                            worst_inversion.pipeline_id = p.first;
                                            worst_inversion.badness = badness;
                                            worst_inversion.r1 = ref.runtimes[0];
                                            worst_inversion.r2 = sched.second.runtimes[0];
                                            worst_inversion.p1 = ref.prediction[model];
                                            worst_inversion.p2 = sched.second.prediction[model];
                                            worst_inversion.f1 = ref.filename;
                                            worst_inversion.f2 = sched.second.filename;
                                        }
                                    }
                                    bad++;
                                }
                            }
                            if (train) {
                                correct_ordering_rate_sum[model] += good;
                                correct_ordering_rate_count[model] += good + bad;
                            } else {
                                v_correct_ordering_rate_sum[model] += good;
                                v_correct_ordering_rate_count[model] += good + bad;
                            }
                        }
                    }
                }

                counter++;
            }

            std::cout << "Loss: ";
            for (int model = 0; model < models; model++) {
                std::cout << loss_sum[model] / loss_sum_counter[model] << " ";
                loss_sum[model] *= 0.9f;
                loss_sum_counter[model] *= 0.9f;
            }
            if (models > 1) std::cout << "\n";
            std::cout << " Rate: ";
            int best_model = 0;
            float best_rate = 0;
            for (int model = 0; model < models; model++) {
                float rate = correct_ordering_rate_sum[model] / correct_ordering_rate_count[model];
                std::cout << rate << " ";
                correct_ordering_rate_sum[model] *= 0.9f;
                correct_ordering_rate_count[model] *= 0.9f;

                rate = v_correct_ordering_rate_sum[model] / v_correct_ordering_rate_count[model];
                if (rate > best_rate) {
                    best_model = model;
                    best_rate = rate;
                }
                std::cout << rate << " ";
                v_correct_ordering_rate_sum[model] *= 0.9f;
                v_correct_ordering_rate_count[model] *= 0.9f;
            }

            if (models > 1) std::cout << "\n";

            if (!predict_only && samples.count(worst_miss_pipeline_id)) {
                std::cout << " Worst: " << worst_miss << " " << samples[worst_miss_pipeline_id].schedules[worst_miss_schedule_id].filename << "\n";
                // samples[worst_miss_pipeline_id].schedules.erase(worst_miss_schedule_id);
            } else {
                std::cout << "\n";
            }

            if (worst_inversion.badness > 0) {
                std::cout << "Worst inversion:\n"
                          << worst_inversion.f1 << " predicted: " << worst_inversion.p1 << " actual: " << worst_inversion.r1 << "\n"
                          << worst_inversion.f2 << " predicted: " << worst_inversion.p2 << " actual: " << worst_inversion.r2 << "\n";
                if (samples.size() > 5000) {
                    // For robustness during training on large numbers
                    // of random pipelines, we discard poorly
                    // performing samples from the training set
                    // only. Some of them are weird degenerate
                    // pipelines.
                    samples.erase(worst_inversion.pipeline_id);
                }
            }

            if (!predict_only && best_rate > best_rate_overall) {
                best_rate_overall = best_rate;
                std::cout << "Saving best weights " << best_rate_overall << "\n";
                tpp[best_model]->save_best_weights(best_rate_overall);
            }

            if (!predict_only) {
                tpp[best_model]->save_weights();
            }

            if (!predict_only && loss_sum[best_model] < 1e-5f) {
                std::cout << "Zero loss, returning early\n";
                return 0;
            }
        }
    }

    if (predict_only) {
        save_predictions(samples, predictions_file);
    }

    // tpp.save_weights();

    return 0;
}
