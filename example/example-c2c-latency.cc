#include <boost/graph/filtered_graph.hpp>
#include <boost/graph/graph_utility.hpp> // print_graph
#include <boost/graph/graphviz.hpp>      // write_graphviz
#include <boost/property_map/property_map.hpp>

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

/////////////////////////////////////////////
// MODULE C2C START
/////////////////////////////////////////////

namespace yloc
{
    class C2CAdapter : public Adapter
    {
    public:
        C2CAdapter(uint64_t latency, uint64_t bandwidth) : m_latency{latency}, m_bandwidth{bandwidth} {};

        std::string to_string() const override
        {
            std::stringstream ss;
            /* TODO: string representation of edge */
            return ss.str();
        }

        std::optional<uint64_t> latency() const override
        {
            return m_latency;
        }

        std::optional<uint64_t> bandwidth() const override
        {
            return m_bandwidth;
        }

    private:
        uint64_t m_latency;   // measured latency
        uint64_t m_bandwidth; // measured bandwidth
    };

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
        std::atomic<uint64_t> reply_ts_ns{0};
        std::atomic<uint64_t> acc_rtt_ns{0};

        auto thread_fn1 = [&]() {
            // pin this thread to cs1
            pthread_t tid = pthread_self();
            pthread_setaffinity_np(tid, sizeof(cpu_set_t), &cs1);

            for (int i = 1; i <= static_cast<int>(rounds); ++i) {
                // record timestamp, signal ping
                auto now = std::chrono::high_resolution_clock::now();
                uint64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
                // write ping id (i) and wait for reply
                ping.store(i, std::memory_order_seq_cst);

                // busy-wait for pong
                while (pong.load(std::memory_order_seq_cst) != i) {
                }

                // read reply timestamp
                uint64_t t_reply = reply_ts_ns.load(std::memory_order_seq_cst);
                // compute RTT and accumulate
                uint64_t rtt = t_reply > ns ? (t_reply - ns) : 0;
                acc_rtt_ns.fetch_add(rtt, std::memory_order_seq_cst);
            }
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

                    // on receive, record timestamp and signal pong
                    auto now = std::chrono::high_resolution_clock::now();
                    uint64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
                    // store timestamp for requester
                    reply_ts_ns.store(ns, std::memory_order_seq_cst);
                    pong.store(i, std::memory_order_seq_cst);
            }
        };

        // reset counters
        ping.store(0);
        pong.store(0);
        reply_ts_ns.store(0);

        std::thread t2(thread_fn2);
        std::thread t1(thread_fn1);

        t1.join();
        t2.join();

        // acc_rtt_ns accumulated sum of RTTs (ns); compute average one-way latency
        uint64_t total_rtt_ns = acc_rtt_ns.load(std::memory_order_seq_cst);
        uint64_t avg_rtt_ns = total_rtt_ns / rounds;
        uint64_t one_way_ns = avg_rtt_ns / 2;

        return one_way_ns; // nanoseconds
    }

    uint64_t measure_c2c_bandwidth(yloc::Graph &g, vertex_t v1, vertex_t v2)
    {
        auto mask1 = g[v1].get<AffinityMask>("cpu_affinity_mask").value();
        auto mask2 = g[v2].get<AffinityMask>("cpu_affinity_mask").value();
        cpu_set_t cs1 = mask1;
        cpu_set_t cs2 = mask2;

        // Triad benchmark: A = B + scalar * C
        // We'll run two threads pinned to the two cores, each operating on disjoint halves
        const size_t cache_capacity = 32000; 
        const size_t total_elems = cache_capacity/24; // ~64MB per array (double)
        const size_t elems_per_thread = total_elems / 2;
        const size_t iterations = 5;
        const double scalar = 3.0;

        // allocate arrays (single allocation for A,B,C)
        std::vector<double> A(total_elems);
        std::vector<double> B(total_elems);
        std::vector<double> C(total_elems);

        // initialize
        std::fill(B.begin(), B.end(), 1.0);
        std::fill(C.begin(), C.end(), 2.0);

        std::atomic<uint64_t> bw_acc_bytes_per_s{0};

        auto triad_worker = [&](size_t offset, size_t len, cpu_set_t cs) {
            // pin
            pthread_t tid = pthread_self();
            pthread_setaffinity_np(tid, sizeof(cpu_set_t), &cs);

            // warmup
            for (size_t i = offset; i < offset + len; ++i) {
                A[i] = B[i] + scalar * C[i];
            }

            // timed iterations
            auto t0 = std::chrono::high_resolution_clock::now();
            for (size_t it = 0; it < iterations; ++it) {
                for (size_t i = offset; i < offset + len; ++i) {
                    A[i] = B[i] + scalar * C[i];
                }
            }
            auto t1 = std::chrono::high_resolution_clock::now();

            double elapsed_s = std::chrono::duration_cast<std::chrono::duration<double>>(t1 - t0).count();

            // bytes per element: read B (8), read C (8), write A (8) = 24 bytes
            uint64_t bytes = static_cast<uint64_t>(len) * 24ull * static_cast<uint64_t>(iterations);
            // compute bytes/s for this worker and accumulate into integer atomic
            uint64_t bw = static_cast<uint64_t>(static_cast<double>(bytes) / elapsed_s);
            bw_acc_bytes_per_s.fetch_add(bw, std::memory_order_seq_cst);
        };

        // spawn two workers
        std::thread w1(triad_worker, 0ul, elems_per_thread, cs1);
        std::thread w2(triad_worker, elems_per_thread, elems_per_thread, cs2);

        w1.join();
        w2.join();

        // bytes_processed now holds sum of bytes/s from both workers
        uint64_t agg_bw_bytes_per_s = bw_acc_bytes_per_s.load(std::memory_order_seq_cst);
        // convert to bits/s
        uint64_t agg_bw_bits_per_s = agg_bw_bytes_per_s * 8ull;
        return agg_bw_bits_per_s;
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

                    /* measure c2c latency / bandwidth between v1 and v2 */
                    auto latency = measure_c2c_latency(g, v1, v2);
                    auto bandwidth = measure_c2c_bandwidth(g, v1, v2);

                    /* add adapter for latency / bandwidth */
                    /* set measurement value of latency / bandwidth in c2c_edge */
                    auto adapter = std::make_shared<C2CAdapter>(latency, bandwidth);
                    g[c2c_edge.first].add_adapter(adapter);
                }
            }
        }
    }

} // namespace yloc

/////////////////////////////////////////////
// MODULE C2C END
/////////////////////////////////////////////

int main(int argc, char *argv[])
{
    yloc::init();

    yloc::module_c2c_init(); // TODO: this should done inside the module initialization instead

    yloc::Graph &g = yloc::root_graph();

    std::function<bool(vertex_t)> predicate = [&](vertex_t v) { return g[v].is_a<PhysicalCore>(); };
    auto cores = boost::make_filtered_graph(g, boost::keep_all{}, predicate);

    // print values using yloc
    for (auto v1 : yloc::vertex_range(cores)) {
        for (auto v2 : yloc::vertex_range(cores)) {
            if (v1 < v2) {
                auto edge = boost::edge(v1, v2, g);
                edge_t e = edge.first;

                auto latency = g[e].get("latency");

                if (latency.has_value()) {
                    printf("latency from vd=%zu to vd=%zu: %lu\n", v1, v2, *latency);
                }

                auto bandwidth = g[e].get("bandwidth");
                if (bandwidth.has_value()) {
                    printf("bandwidth from vd=%zu to vd=%zu: %lu\n", v1, v2, *bandwidth);
                }
            }
        }
    }

    yloc::finalize();

    return EXIT_SUCCESS;
}
