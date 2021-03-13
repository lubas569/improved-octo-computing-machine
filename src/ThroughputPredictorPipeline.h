#include <algorithm>
#include <fstream>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <arpa/inet.h>

#include "Buffer.h"
#include "Generator.h"
#include "BoundaryConditions.h"
#include "Type.h"

extern "C" int halide_autoscheduler_cost_model(int32_t num_stages,
                                               int32_t batch_size,
                                               int32_t num_cores,
                                               // Inputs
                                               halide_buffer_t *pipeline_features,
                                               halide_buffer_t *schedule_features,
                                               halide_buffer_t *pipeline_mean,
                                               halide_buffer_t *pipeline_std,
                                               halide_buffer_t *schedule_mean,
                                               halide_buffer_t *schedule_std,
                                               halide_buffer_t *head1_filter,
                                               halide_buffer_t *head1_bias,
                                               halide_buffer_t *head2_filter,
                                               halide_buffer_t *head2_bias,
                                               halide_buffer_t *filter1,
                                               halide_buffer_t *bias1,
                                               // Unused
                                               float learning_rate,
                                               int timestep,
                                               halide_buffer_t *true_runtime,
                                               // Output
                                               halide_buffer_t *prediction,
                                               halide_buffer_t *loss);

extern "C" int halide_autoscheduler_train_cost_model(int32_t _num_stages,
                                                     int32_t _batch_size,
                                                     int32_t _num_cores,
                                                     // Inputs
                                                     halide_buffer_t *pipeline_features,
                                                     halide_buffer_t *schedule_features,
                                                     halide_buffer_t *pipeline_mean,
                                                     halide_buffer_t *pipeline_std,
                                                     halide_buffer_t *schedule_mean,
                                                     halide_buffer_t *schedule_std,
                                                     halide_buffer_t *head1_filter,
                                                     halide_buffer_t *head1_bias,
                                                     halide_buffer_t *head2_filter,
                                                     halide_buffer_t *head2_bias,
                                                     halide_buffer_t *filter1,
                                                     halide_buffer_t *bias1,
                                                     float learning_rate,
                                                     int timestep,
                                                     halide_buffer_t *true_runtime,
                                                     // Outputs
                                                     halide_buffer_t *d_loss_d_head1_filter,
                                                     halide_buffer_t *d_loss_d_head1_bias,
                                                     halide_buffer_t *d_loss_d_head2_filter,
                                                     halide_buffer_t *d_loss_d_head2_bias,
                                                     halide_buffer_t *d_loss_d_filter1,
                                                     halide_buffer_t *d_loss_d_bias1,
                                                     halide_buffer_t *prediction,
                                                     halide_buffer_t *loss);

extern "C" float halide_internal_weights_pipeline_mean[];
extern "C" int halide_internal_weights_pipeline_mean_length;
extern "C" float halide_internal_weights_pipeline_std[];
extern "C" int halide_internal_weights_pipeline_std_length;
extern "C" float halide_internal_weights_schedule_mean[];
extern "C" int halide_internal_weights_schedule_mean_length;
extern "C" float halide_internal_weights_schedule_std[];
extern "C" int halide_internal_weights_schedule_std_length;

extern "C" float halide_internal_weights_head1_conv1_bias[];
extern "C" int halide_internal_weights_head1_conv1_bias_length;
extern "C" float halide_internal_weights_head1_conv1_weight[];
extern "C" int halide_internal_weights_head1_conv1_weight_length;
extern "C" float halide_internal_weights_head2_conv1_bias[];
extern "C" int halide_internal_weights_head2_conv1_bias_length;
extern "C" float halide_internal_weights_head2_conv1_weight[];
extern "C" int halide_internal_weights_head2_conv1_weight_length;
extern "C" float halide_internal_weights_trunk_conv1_bias[];
extern "C" int halide_internal_weights_trunk_conv1_bias_length;
extern "C" float halide_internal_weights_trunk_conv1_weight[];
extern "C" int halide_internal_weights_trunk_conv1_weight_length;

namespace Halide {
namespace Internal {
namespace AutoScheduleModel {

Runtime::Buffer<float> buffer_from_file(const std::string &filename, const std::vector<int> &shape) {
    Runtime::Buffer<float> buf(shape);
    assert_file_exists(filename);

    std::ifstream i(filename.c_str());
    i.read((char *)(buf.data()), buf.size_in_bytes());

    return buf;
}

void buffer_to_file(const Runtime::Buffer<float> &buf, const std::string &filename) {
    {
        std::ofstream o(filename.c_str());
        o.write((const char *)(buf.data()), buf.size_in_bytes());
    }
    assert_file_exists(filename);
}

struct Stats {
    Runtime::Buffer<float> pipeline_mean;
    Runtime::Buffer<float> pipeline_std;
    Runtime::Buffer<float> schedule_mean;
    Runtime::Buffer<float> schedule_std;
};

struct Weights {
    Runtime::Buffer<float> head1_filter;
    Runtime::Buffer<float> head1_bias;

    Runtime::Buffer<float> head2_filter;
    Runtime::Buffer<float> head2_bias;

    Runtime::Buffer<float> conv1_filter;
    Runtime::Buffer<float> conv1_bias;
};

class ThroughputPredictorPipeline {
    std::string weights_dir;
    Weights weights;
    Stats stats;
    Runtime::Buffer<float> schedule_feat_queue, pipeline_feat_queue, costs;
    Runtime::Buffer<double *> cost_ptrs;
    int cursor, num_stages, num_cores = 0;

    std::string weights_server_hostname;
    int weights_server_port = 0;
    int weights_server_experiment_id = 0;

 public:

    ThroughputPredictorPipeline() {
        weights_dir = get_env_variable("HL_WEIGHTS_DIR");
        load_weights();
        load_stats();

        weights_server_hostname = get_env_variable("HL_WEIGHTS_SERVER_HOSTNAME");
        if (!weights_server_hostname.empty()) {
            weights_server_port = std::atoi(get_env_variable("HL_WEIGHTS_SERVER_PORT").c_str());
            weights_server_experiment_id = std::atoi(get_env_variable("HL_WEIGHTS_SERVER_EXPERIMENT_ID").c_str());
            debug(0) << "Using weights server " << weights_server_hostname << ":" << weights_server_port << "/" << weights_server_experiment_id << "\n";
            send_weights_to_weights_server();
        }
    }

    void set_pipeline_features(const Runtime::Buffer<float> &pipeline_feats) {
        pipeline_feat_queue = pipeline_feats;
    }

    void set_num_cores(int n) {
        num_cores = n;
    }

    void enqueue(int ns, Runtime::Buffer<float> *schedule_feats, double *cost_ptr) {
        num_stages = ns;

        // We know the most stages that will ever be enqueued from the schedule features
        internal_assert(pipeline_feat_queue.data()) << "Call set_schedule_features before calling enqueue\n";
        const int max_num_stages = pipeline_feat_queue.dim(2).extent();
        internal_assert(num_stages <= max_num_stages)
            << "schedule features has more stages (" << num_stages
            << ") than pipeline features (" << max_num_stages << ")\n";

        const int batch_size = 1024;
        if (!schedule_feat_queue.data() ||
            schedule_feat_queue.dim(2).extent() < max_num_stages) {
            internal_assert(cursor == 0);
            schedule_feat_queue = Runtime::Buffer<float>(batch_size, 26, max_num_stages);
            if (!costs.data()) {
                internal_assert(!cost_ptrs.data());
                costs = Runtime::Buffer<float>(batch_size);
                cost_ptrs = Runtime::Buffer<double *>(batch_size);
            }
        }

        if (cursor == batch_size) {
            evaluate_costs();
        }

        *schedule_feats = schedule_feat_queue.sliced(0, cursor);
        cost_ptrs(cursor) = cost_ptr;

        cursor++;
    }

    // Backprop state. To run ADAM we need a running average of the
    // gradients and gradients squared. We add an outer dimension of
    // size 3 to the new weight outputs to track this state. So buf(_,
    // 0) is the new weight, buf(_, 1) is the ADAM running average of
    // the first moment, and buf(_, 2) is the ADAM running average of
    // the second moment.
    Runtime::Buffer<float>
        head1_filter_update, head1_bias_update,
        head2_filter_update, head2_bias_update,
        conv1_filter_update, conv1_bias_update;
    int timestep = 0;

    float backprop(Runtime::Buffer<const float> true_runtimes, float learning_rate) {
        internal_assert(cursor != 0);
        internal_assert(pipeline_feat_queue.data());
        internal_assert(schedule_feat_queue.data());

        auto loss = Runtime::Buffer<float>::make_scalar();

        if (!head1_filter_update.data()) {
            auto weight_update_buffer = [](const Runtime::Buffer<float> &w) {
                std::vector<int> size;
                for (int i = 0; i < w.dimensions(); i++) {
                    size.push_back(w.dim(i).extent());
                }
                size.push_back(4);
                auto buf = Runtime::Buffer<float>(size);
                buf.fill(0.0f);
                return buf;
            };

            head1_filter_update = weight_update_buffer(weights.head1_filter);
            head1_bias_update = weight_update_buffer(weights.head1_bias);
            head2_filter_update = weight_update_buffer(weights.head2_filter);
            head2_bias_update = weight_update_buffer(weights.head2_bias);
            conv1_filter_update = weight_update_buffer(weights.conv1_filter);
            conv1_bias_update = weight_update_buffer(weights.conv1_bias);
            timestep = 0;
        }

        Runtime::Buffer<float> dst = costs.cropped(0, 0, cursor);

        halide_autoscheduler_train_cost_model(num_stages,
                                              cursor,
                                              num_cores,
                                              pipeline_feat_queue,
                                              schedule_feat_queue,
                                              stats.pipeline_mean,
                                              stats.pipeline_std,
                                              stats.schedule_mean,
                                              stats.schedule_std,
                                              weights.head1_filter, weights.head1_bias,
                                              weights.head2_filter, weights.head2_bias,
                                              weights.conv1_filter, weights.conv1_bias,
                                              learning_rate, timestep++,
                                              true_runtimes,
                                              head1_filter_update, head1_bias_update,
                                              head2_filter_update, head2_bias_update,
                                              conv1_filter_update, conv1_bias_update,
                                              dst,
                                              loss);


        double err = 0;
        for (int i = 0; i < cursor; i++) {
            internal_assert(cost_ptrs(i)) << "Cost queue entry was null: " << i << "\n";
            *(cost_ptrs(i)) = dst(i);
            double delta = (true_runtimes(i) - dst(i)) / true_runtimes(0);
            err += delta * delta;
        }
        err /= cursor;
        err = std::sqrt(err);


        if (!weights_server_hostname.empty()) {
            // Send gradients, receive new weights
            send_gradients_to_weights_server();
            get_weights_from_weights_server();
        } else {
            // Update weights locally
            auto update_weight = [](const Runtime::Buffer<float> &src, Runtime::Buffer<float> &dst) {
                dst.copy_from(src.sliced(src.dimensions()-1, 0));
                /*
                double grad_mag = 0, weight_mag = 0;
                auto grad = src.sliced(src.dimensions() - 1, 3);
                grad.for_each_value([&](float f) {grad_mag += f*f;});
                auto weight = src.sliced(src.dimensions() - 1, 0);
                weight.for_each_value([&](float f) {weight_mag += f*f;});
                debug(0) << std::sqrt(grad_mag / grad.number_of_elements()) << " "
                << std::sqrt(weight_mag / weight.number_of_elements()) << "\n";
                */

            };
            update_weight(head1_filter_update, weights.head1_filter);
            update_weight(head1_bias_update, weights.head1_bias);
            update_weight(head2_filter_update, weights.head2_filter);
            update_weight(head2_bias_update, weights.head2_bias);
            update_weight(conv1_filter_update, weights.conv1_filter);
            update_weight(conv1_bias_update, weights.conv1_bias);
        }

        internal_assert(cursor != 0);

        return err;
    }

    void evaluate_costs() {
        if (cursor == 0 || !schedule_feat_queue.data()) return;

        internal_assert(pipeline_feat_queue.data());
        internal_assert(schedule_feat_queue.data());

        Runtime::Buffer<float> dst = costs.cropped(0, 0, cursor);

        auto loss = Runtime::Buffer<float>::make_scalar();

        halide_autoscheduler_cost_model(num_stages,
                                        cursor,
                                        num_cores,
                                        pipeline_feat_queue,
                                        schedule_feat_queue,
                                        stats.pipeline_mean,
                                        stats.pipeline_std,
                                        stats.schedule_mean,
                                        stats.schedule_std,
                                        weights.head1_filter, weights.head1_bias,
                                        weights.head2_filter, weights.head2_bias,
                                        weights.conv1_filter, weights.conv1_bias,
                                        0.0f, 0, nullptr,
                                        dst, loss);

        for (int i = 0; i < cursor; i++) {
            internal_assert(cost_ptrs(i)) << "Cost queue entry was null: " << i << "\n";
            *(cost_ptrs(i)) = dst(i);
        }

        cursor = 0;
    }

    Runtime::Buffer<float> zero_pad(const Runtime::Buffer<float> &src, const std::vector<int> &new_size) {
        Runtime::Buffer<float> dst(new_size);
        dst.fill(0.0f);
        debug(0) << "Src shape: ";
        for (int i = 0; i < src.dimensions(); i++) {
            debug(0) << src.dim(i).extent() << " ";
        }
        debug(0) << "\nDst shape: ";
        for (int i = 0; i < dst.dimensions(); i++) {
            debug(0) << dst.dim(i).extent() << " ";
        }
        debug(0) << "\n";
        internal_assert(src.dimensions() == dst.dimensions());
        for (int i = 0; i < dst.dimensions(); i++) {
            internal_assert(src.dim(i).extent() <= dst.dim(i).extent());
        }
        dst.copy_from(src);
        return dst;
    }

    void load_weights() {

        internal_assert(!weights_dir.empty());

        if (weights_dir.empty()) {
            weights.head1_filter = Runtime::Buffer<float>(halide_internal_weights_head1_conv1_weight, 24, 56, 7);
            //internal_assert(halide_internal_weights_head1_conv1_weight_length == (int)weights.head1_filter.size_in_bytes());

            weights.head1_bias = Runtime::Buffer<float>(halide_internal_weights_head1_conv1_bias, 24);
            //internal_assert(halide_internal_weights_head1_conv1_bias_length == (int)weights.head1_bias.size_in_bytes());

            weights.head2_filter = Runtime::Buffer<float>(halide_internal_weights_head2_conv1_weight, 24, 26);
            //internal_assert(halide_internal_weights_head2_conv1_weight_length == (int)weights.head2_filter.size_in_bytes());

            weights.head2_bias = Runtime::Buffer<float>(halide_internal_weights_head2_conv1_bias, 24);
            //internal_assert(halide_internal_weights_head2_conv1_bias_length == (int)weights.head2_bias.size_in_bytes());

            weights.conv1_filter = Runtime::Buffer<float>(halide_internal_weights_trunk_conv1_weight, 16, 48, 3);
            //internal_assert(halide_internal_weights_trunk_conv1_weight_length == (int)weights.conv1_filter.size_in_bytes());

            weights.conv1_bias = Runtime::Buffer<float>(halide_internal_weights_trunk_conv1_bias, 16);
            //internal_assert(halide_internal_weights_trunk_conv1_bias_length == (int)weights.conv1_bias.size_in_bytes());
        } else {
            weights.head1_filter = buffer_from_file(weights_dir + "/head1_conv1_weight.data", {24, 56, 7});
            weights.head1_bias = buffer_from_file(weights_dir + "/head1_conv1_bias.data", {24});

            weights.head2_filter = buffer_from_file(weights_dir + "/head2_conv1_weight.data", {24, 26});
            weights.head2_bias = buffer_from_file(weights_dir + "/head2_conv1_bias.data", {24});

            weights.conv1_filter = buffer_from_file(weights_dir + "/trunk_conv1_weight.data", {16, 48, 3});
            weights.conv1_bias = buffer_from_file(weights_dir + "/trunk_conv1_bias.data", {16});
        }

        if (get_env_variable("HL_RANDOMIZE_WEIGHTS") == "1") {
            // Fill the weights with random values
            for_each_weight([](Runtime::Buffer<float> &w) {
                    w.for_each_value([](float &f) {
                            f = ((float)rand()) / RAND_MAX - 0.5f;
                        });
                });
        }

        // The following code is for resizing the weights to a larger size with zero-padding

        #if 0
        // Now reshuffle the weights in memory and zero-pad them out
        // to the size expected by the current cost model generator
        // (which is larger than the size of the checked-in weights)
        weights.head1_filter = zero_pad(weights.head1_filter, {24, 56, 7});
        weights.head1_bias = zero_pad(weights.head1_bias, {24});

        weights.head2_filter = zero_pad(weights.head2_filter, {24, 26});
        weights.head2_bias = zero_pad(weights.head2_bias, {24});

        weights.conv1_filter = zero_pad(weights.conv1_filter, {16, 48, 3});
        weights.conv1_bias = zero_pad(weights.conv1_bias, {16});

        save_weights();
        #endif
    }

    void load_stats() {
        if (weights_dir.empty()) {
            stats.pipeline_mean = Runtime::Buffer<float>(halide_internal_weights_pipeline_mean, 56, 7);
            internal_assert(halide_internal_weights_pipeline_mean_length == (int)stats.pipeline_mean.size_in_bytes());

            stats.pipeline_std = Runtime::Buffer<float>(halide_internal_weights_pipeline_std,  56, 7);
            internal_assert(halide_internal_weights_pipeline_std_length == (int)stats.pipeline_std.size_in_bytes());

            stats.schedule_mean = Runtime::Buffer<float>(halide_internal_weights_schedule_mean, 26);
            internal_assert(halide_internal_weights_schedule_mean_length == (int)stats.schedule_mean.size_in_bytes());

            stats.schedule_std = Runtime::Buffer<float>(halide_internal_weights_schedule_std, 26);
            internal_assert(halide_internal_weights_schedule_std_length == (int)stats.schedule_std.size_in_bytes());
        } else {
            stats.pipeline_mean = buffer_from_file(weights_dir + "/pipeline_mean.data", {56, 7});
            stats.pipeline_std = buffer_from_file(weights_dir + "/pipeline_std.data", {56, 7});
            stats.schedule_mean = buffer_from_file(weights_dir + "/schedule_mean.data", {26});
            stats.schedule_std = buffer_from_file(weights_dir + "/schedule_std.data", {26});
        }
    }

    void save_weights() {
        if (weights_dir.empty()) return;

        buffer_to_file(weights.head1_filter, weights_dir + "/head1_conv1_weight.data");
        buffer_to_file(weights.head1_bias, weights_dir + "/head1_conv1_bias.data");
        buffer_to_file(weights.head2_filter, weights_dir + "/head2_conv1_weight.data");
        buffer_to_file(weights.head2_bias, weights_dir + "/head2_conv1_bias.data");
        buffer_to_file(weights.conv1_filter, weights_dir + "/trunk_conv1_weight.data");
        buffer_to_file(weights.conv1_bias, weights_dir + "/trunk_conv1_bias.data");
    }


    struct TCPConnection {
        int fd = 0;

        TCPConnection(const std::string &server, int port) {
            sockaddr_in serv_addr {0};
            int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            internal_assert(sock >= 0) << "Socket creation error";
            fd = sock;

            int option = 1;
            setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));

            serv_addr.sin_family = AF_INET;
            serv_addr.sin_port = htons(port);

            // Convert IPv4 and IPv6 addresses from text to binary form
            int err = inet_pton(AF_INET, server.c_str(), &serv_addr.sin_addr);
            internal_assert(err == 0) << "Invalid address";

            err = connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr));
            if (err != 0) {
                perror("Error connecting to weights server");
            }
            internal_assert(err == 0) << "Connection failed\n";
        }

        void send(const uint8_t *data, ssize_t len) {
            ssize_t sent = ::send(fd, data, len, 0);
            internal_assert(sent == len) << "Failed to send everything: " << sent << "/" << len;
        }

        void recv(uint8_t *data, ssize_t len) {
            ssize_t received = 0;
            while (received < len) {
                ssize_t r = ::recv(fd, data + received, len - received, 0);
                internal_assert(r > 0) << "Failed to receive bytes: " << r;
                received += r;
            }
        }

        ~TCPConnection() {
            shutdown(fd, SHUT_RDWR);
            close(fd);
        }
    };

    template<typename F>
    void for_each_weight(F f) {
        f(weights.head1_filter);
        f(weights.head1_bias);
        f(weights.head2_filter);
        f(weights.head2_bias);
        f(weights.conv1_filter);
        f(weights.conv1_bias);
    }

    template<typename F>
    void for_each_gradient(F f) {
        auto slice_and_call_f = [&](const Runtime::Buffer<float> &buf) {
            f(buf.sliced(buf.dimensions()-1, 3));
        };
        slice_and_call_f(head1_filter_update);
        slice_and_call_f(head1_bias_update);
        slice_and_call_f(head2_filter_update);
        slice_and_call_f(head2_bias_update);
        slice_and_call_f(conv1_filter_update);
        slice_and_call_f(conv1_bias_update);
    }

    void send_weights_to_weights_server() {
        // debug(0) << "Sending weights to weights server...\n";
        auto conn = TCPConnection(weights_server_hostname, weights_server_port);

        ssize_t total_size_of_weights = 0;

        for_each_weight([&](const Runtime::Buffer<float> &w) {
                total_size_of_weights += w.size_in_bytes();
            });

        int header[4] = {7582946, 1, weights_server_experiment_id, (int)total_size_of_weights};
        conn.send((const uint8_t *)header, sizeof(header));
        for_each_weight([&](const Runtime::Buffer<float> &w) {
                conn.send((const uint8_t *)(w.data()), w.size_in_bytes());
            });
        // debug(0) << "Sent.\n";
    }

    void send_gradients_to_weights_server() {
        // debug(0) << "Sending gradients to weights server...\n";
        auto conn = TCPConnection(weights_server_hostname, weights_server_port);

        ssize_t total_size_of_weights = 0;

        for_each_gradient([&](const Runtime::Buffer<float> &w) {
                total_size_of_weights += w.size_in_bytes();
            });

        int header[4] = {7582946, 2, weights_server_experiment_id, (int)total_size_of_weights};
        conn.send((const uint8_t *)header, sizeof(header));
        for_each_gradient([&](const Runtime::Buffer<float> &w) {
                conn.send((const uint8_t *)(w.data()), w.size_in_bytes());
            });
        // debug(0) << "Sent.\n";
    }

    void get_weights_from_weights_server() {
        // debug(0) << "Getting weights from weights server...\n";
        auto conn = TCPConnection(weights_server_hostname, weights_server_port);

        ssize_t total_size_of_weights = 0;

        for_each_weight([&](const Runtime::Buffer<float> &w) {
                total_size_of_weights += w.size_in_bytes();
            });

        int header[4] = {7582946, 0, weights_server_experiment_id, (int)total_size_of_weights};
        conn.send((const uint8_t *)header, sizeof(header));
        for_each_weight([&](Runtime::Buffer<float> &w) {
                conn.recv((uint8_t *)(w.data()), w.size_in_bytes());
            });
        // debug(0) << "Received.\n";
    }

    // Discard any enqueued but unevaluated schedules
    void reset() {
        cursor = 0;
    }

};

}
}
}
