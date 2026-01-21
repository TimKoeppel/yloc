#include "interface_impl.h"
#include "c2c_adapter.h"
#include <yloc/affinity.h>
#include <yloc/modules/adapter.h>

#include <yloc/yloc.h>

#include <memory>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <pthread.h>
#include <algorithm>

using namespace yloc;
using namespace std::string_literals;

    uint64_t measure_c2c_latency(yloc::Graph &g, vertex_t v1, vertex_t v2)
    {
        auto mask1 = g[v1].get<AffinityMask>("cpu_affinity_mask").value();
        auto mask2 = g[v2].get<AffinityMask>("cpu_affinity_mask").value();

        // Convert to cpu_set_t
        cpu_set_t cs1 = mask1;
        cpu_set_t cs2 = mask2;

        // Ping-pong using two threads to measure round-trip latency.
        constexpr size_t rounds = 10000;
        std::atomic<int> ping{0};
        std::atomic<int> pong{0};
        std::atomic<uint64_t> total_rtt_ns{0};

        auto thread_fn1 = [&]() {
            // pin this thread to cs1
            pthread_t tid = pthread_self();
            pthread_setaffinity_np(tid, sizeof(cpu_set_t), &cs1);

            // record start time before rounds
            auto start = std::chrono::high_resolution_clock::now();
            uint64_t start_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(start.time_since_epoch()).count();

            for (int i = 1; i <= static_cast<int>(rounds); ++i) {
                // signal ping
                ping.store(i, std::memory_order_seq_cst);

                // busy-wait for pong
                while (pong.load(std::memory_order_seq_cst) != i) {
                }
            }

            // record end time after all rounds
            auto end = std::chrono::high_resolution_clock::now();
            uint64_t end_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end.time_since_epoch()).count();

            // total time for all round-trips
            uint64_t total_time_ns = end_ns - start_ns;
            total_rtt_ns.store(total_time_ns, std::memory_order_seq_cst);
        };

        auto thread_fn2 = [&]() {
            // pin this thread to cs2
            pthread_t tid = pthread_self();
            pthread_setaffinity_np(tid, sizeof(cpu_set_t), &cs2);

            for (int i = 1; i <= static_cast<int>(rounds); ++i) {
                // wait for ping
                while (ping.load(std::memory_order_seq_cst) != i) {
                    ;
                }

                // signal pong
                pong.store(i, std::memory_order_seq_cst);
            }
        };

        // reset counters
        ping.store(0);
        pong.store(0);
        total_rtt_ns.store(0);

        std::thread t2(thread_fn2);
        std::thread t1(thread_fn1);

        t1.join();
        t2.join();

        // compute average one-way latency
        uint64_t total_rtt = total_rtt_ns.load(std::memory_order_seq_cst);
        uint64_t avg_rtt_ns = total_rtt / rounds;
        uint64_t one_way_ns = avg_rtt_ns / 2;

        return one_way_ns; // nanoseconds
    }

    void module_c2c_init()
    {
        yloc::Graph &g = yloc::root_graph();

        std::function<bool(vertex_t)> predicate = [&](vertex_t v) { return g[v].is_a<PhysicalCore>(); };
        auto cores = boost::make_filtered_graph(g, boost::keep_all{}, predicate);

        // measurement code
        for (auto v1 : yloc::vertex_range(cores)) {
            for (auto v2 : yloc::vertex_range(cores)) {
                if (v1 < v2) {
                    auto c2c_edge = boost::add_edge(v1, v2, Edge{edge_type::C2C_MEASUREMENT}, g);

                    /* measure c2c latency between v1 and v2 */
                    auto latency = measure_c2c_latency(g, v1, v2);
                    /* add adapter for latency */
                    /* set measurement value of latency in c2c_edge */
                    auto adapter = std::make_shared<C2CAdapter>(latency);
                    g[c2c_edge.first].add_adapter(adapter);
                }
            }
        }
    }

    yloc_status_t ModuleC2C::init_graph(Graph &g)
    {
        module_c2c_init();
        return YLOC_STATUS_SUCCESS;
    }