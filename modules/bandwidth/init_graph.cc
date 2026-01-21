#include "interface_impl.h"
#include "bw_adapter.h"
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
#include <unistd.h>

using namespace yloc;
using namespace std::string_literals;

    struct BandwidthResult {
        double core_to_L1;
        double L1_to_L2;
        double L2_to_L3;
        double L3_to_mem;
    };

    struct timespec t0, t1;

    BandwidthResult measure_bandwidth(yloc::Graph &g, vertex_t v1)
    {
        auto mask1 = g[v1].get<AffinityMask>("cpu_affinity_mask").value();
        cpu_set_t cs1 = mask1;

        // Triad benchmark: A = B + scalar * C
        long l1 = sysconf(_SC_LEVEL1_DCACHE_SIZE);
        long l2 = sysconf(_SC_LEVEL2_CACHE_SIZE);
        long l3 = sysconf(_SC_LEVEL3_CACHE_SIZE);

        // sensible fallbacks if sysconf is not available / returns -1
        const long fallback_l1 = 64 * 1024;       // 64KB
        const long fallback_l2 = 256 * 1024;      // 256KB
        const long fallback_l3 = 8 * 1024 * 1024; // 8MB

        if (l1 <= 0) l1 = fallback_l1;
        if (l2 <= 0) l2 = fallback_l2;
        if (l3 <= 0) l3 = fallback_l3;

        const size_t L1 = static_cast<size_t>(l1) / sizeof(double);
        const size_t L2 = static_cast<size_t>(l2) / sizeof(double);
        const size_t L3 = static_cast<size_t>(l3) / sizeof(double);
        const size_t large_cache = 1024 * 1024 * 1024 / sizeof(double); // 1GB

        const double scalar = 3.0;

        std::atomic<uint64_t> bw_acc_bytes_per_s{0};

        // run the worker multiple times
        std::vector<size_t> run_sizes = { L1, L2, L3, large_cache};
        std::vector<size_t> iterations_per_level = { 100000, 1000, 100, 10 }; // mehr Iterationen für kleine Arrays

        BandwidthResult bw_res{0.0, 0.0, 0.0, 0.0};

        for (size_t i = 0; i < run_sizes.size(); ++i) {
            bw_acc_bytes_per_s.store(0, std::memory_order_relaxed);
            size_t run_len = run_sizes[i];
            if (run_len == 0) continue;

            // allocate per-run so A/B/C sizes match the current working set
            std::vector<double> A(run_len);
            std::vector<double> B(run_len);
            std::vector<double> C(run_len);
            std::fill(B.begin(), B.end(), 1.0);
            std::fill(C.begin(), C.end(), 2.0);

            auto triad_worker = [&](size_t offset, size_t len, cpu_set_t cs) {
                // pin
                pthread_t tid = pthread_self();
                pthread_setaffinity_np(tid, sizeof(cpu_set_t), &cs);

                // warmup
                for (size_t i = offset; i < offset + len; ++i) {
                    A[i] = B[i] + scalar * C[i];
                }

                // timed iterations
                clock_gettime(CLOCK_MONOTONIC_RAW, &t0);
                for (size_t it = 0; it < iterations_per_level[i]; ++it) {
                    for (size_t i = offset; i < offset + len; ++i) {
                        A[i] = B[i] + scalar * C[i];
                    }
                }
                clock_gettime(CLOCK_MONOTONIC_RAW, &t1);

                double elapsed_s = static_cast<double>(t1.tv_sec - t0.tv_sec) + static_cast<double>(t1.tv_nsec - t0.tv_nsec) * 1e-9;

                // bytes per element: read B (8), read C (8), write A (8) = 24 bytes
                uint64_t bytes = static_cast<uint64_t>(len) * 24ull * static_cast<uint64_t>(iterations_per_level[i]);
                // compute bytes/s for this worker and accumulate into integer atomic
                uint64_t bw = static_cast<uint64_t>(static_cast<double>(bytes) / elapsed_s);
                bw_acc_bytes_per_s.fetch_add(bw, std::memory_order_seq_cst);
            };

            std::thread w(triad_worker, 0ul, run_len, cs1);
            w.join();
            double bw_mib_per_s = static_cast<double>(bw_acc_bytes_per_s.load()) / (1024.0*1024.0);

            // Wert der richtigen Kante zuordnen
            switch(i) {
                case 0: bw_res.core_to_L1 = bw_mib_per_s; break;
                case 1: bw_res.L1_to_L2   = bw_mib_per_s; break;
                case 2: bw_res.L2_to_L3   = bw_mib_per_s; break;
                case 3: bw_res.L3_to_mem  = bw_mib_per_s; break;
            }
        }
        return bw_res;
    }

    void module_bandwidth_init()
    {
        yloc::Graph &g = yloc::root_graph();

        std::function<bool(vertex_t)> predicate = [&](vertex_t v) { return g[v].is_a<PhysicalCore>(); };
        auto cores = boost::make_filtered_graph(g, boost::keep_all{}, predicate);

        // measurement code
        for (auto v : yloc::vertex_range(cores)) {
            /* measure bandwidth for v */
            auto bandwidth = measure_bandwidth(g, v);

        // Core -> L1 Parent-Edge
        for (auto e : yloc::edge_range(g)) {
            if (g[e].m_edgetype == edge_type::PARENT && g[target(e,g)].is_a<L1Cache>()) {
                auto adapter = std::make_shared<BWAdapter>(bandwidth.core_to_L1);
                g[e].add_adapter(adapter);
            }
        }
        // L1 -> L2 Parent-Edge
        for (auto e : yloc::edge_range(g)) {
            if (g[e].m_edgetype == edge_type::PARENT && g[target(e,g)].is_a<L2Cache>()) {
                auto adapter = std::make_shared<BWAdapter>(bandwidth.L1_to_L2);
                g[e].add_adapter(adapter);
            }
        }
        // L2 -> L3 Parent-Edge
        for (auto e : yloc::edge_range(g)) {
            if (g[e].m_edgetype == edge_type::PARENT && g[target(e,g)].is_a<L3Cache>()) {
                auto adapter = std::make_shared<BWAdapter>(bandwidth.L2_to_L3);
                g[e].add_adapter(adapter);
            }
        }
        // L3 -> MEM Parent-Edge
        for (auto e : yloc::edge_range(g)) {
            if (g[e].m_edgetype == edge_type::PARENT && g[target(e,g)].is_a<Memory>()) {
                auto adapter = std::make_shared<BWAdapter>(bandwidth.L3_to_mem);
                g[e].add_adapter(adapter);
            }
        }
            /* add adapter for bandwidth */
            /* set measurement value of bandwidth in vertex */
            //auto adapter = std::make_shared<BWAdapter>(bandwidth);
            //g[v].add_adapter(adapter);
        }
    }

    yloc_status_t ModuleBandwidth::init_graph(Graph &g)
    {
        module_bandwidth_init();
        return YLOC_STATUS_SUCCESS;
    }