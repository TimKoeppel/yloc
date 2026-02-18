#include "interface_impl.h"
#include "bw_adapter.h"
#include <yloc/affinity.h>
#include <yloc/modules/adapter.h>

#include <yloc/yloc.h>

#include <memory>
#include <vector>
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

        const size_t L1 = static_cast<size_t>(l1) / (3.0 * sizeof(double));
        const size_t L2 = static_cast<size_t>(l2) / (3.0 * sizeof(double));
        const size_t L3 = static_cast<size_t>(l3) / (3.0 * sizeof(double));

        const double scalar = 3.0;

        // run the worker multiple times
        std::vector<size_t> run_sizes = { L1/2, L2/2, L3/2, 4*L3};
        std::vector<size_t> iterations_per_level = { 100000, 2000, 400, 10 }; // mehr Iterationen für kleine Arrays

        BandwidthResult bw_res{0.0, 0.0, 0.0, 0.0};

        for (size_t i = 0; i < run_sizes.size(); ++i) {
            const size_t len = run_sizes[i];
            const size_t iters = iterations_per_level[i];

            std::vector<double> A(len), B(len), C(len);

            std::fill(B.begin(), B.end(), 1.0);
            std::fill(C.begin(), C.end(), 2.0);

            // pin thread
            pthread_t tid = pthread_self();
            pthread_setaffinity_np(tid, sizeof(cpu_set_t), &cs1);

            // warmup
            for (size_t i = 0; i < len; ++i){
                A[i] = B[i] + scalar * C[i];
            }

            clock_gettime(CLOCK_MONOTONIC_RAW, &t0);

            for (size_t it = 0; it < iters; ++it) {
                #pragma GCC ivdep
                for (size_t i = 0; i < len; ++i) {
                    A[i] = B[i] + scalar * C[i];
                }
            }

            clock_gettime(CLOCK_MONOTONIC_RAW, &t1);

            double elapsed_s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;

            // 32 bytes per element
            double bytes = static_cast<double>(len) * static_cast<double>(iters) * 32.0;

            double bw_gb_per_s = bytes / elapsed_s / (1024.0 * 1024.0 * 1024.0);
            // Wert der richtigen Kante zuordnen
            switch (i) {
                case 0: bw_res.core_to_L1 = bw_gb_per_s; break;
                case 1: bw_res.L1_to_L2   = bw_gb_per_s; break;
                case 2: bw_res.L2_to_L3   = bw_gb_per_s; break;
                case 3: bw_res.L3_to_mem  = bw_gb_per_s; break;
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
            vertex_t l1 = vertex_t{};
            vertex_t l2 = vertex_t{};
            vertex_t l3 = vertex_t{};

        // Core -> L1 Parent-Edge
        for (auto e : yloc::edge_range(g)) {
            if (g[e].m_edgetype == edge_type::PARENT && g[target(e,g)].is_a<L1Cache>() && source(e,g) == v) {
                auto adapter = std::make_shared<BWAdapter>(bandwidth.core_to_L1);
                g[e].add_adapter(adapter);
                l1 = target(e,g);
            }
        }
        // L1 -> L2 Parent-Edge
        for (auto e : yloc::edge_range(g)) {
            if (g[e].m_edgetype == edge_type::PARENT && g[target(e,g)].is_a<L2Cache>() && source(e,g) == l1) {
                auto adapter = std::make_shared<BWAdapter>(bandwidth.L1_to_L2);
                g[e].add_adapter(adapter);
                l2 = target(e,g);
            }
        }
        // L2 -> L3 Parent-Edge
        for (auto e : yloc::edge_range(g)) {
            if (g[e].m_edgetype == edge_type::PARENT && g[target(e,g)].is_a<L3Cache>() && source(e,g) == l2) {
                auto adapter = std::make_shared<BWAdapter>(bandwidth.L2_to_L3);
                g[e].add_adapter(adapter);
                l3 = target(e,g);
            }
        }
        // L3 -> MEM Parent-Edge
        for (auto e : yloc::edge_range(g)) {
            if (g[e].m_edgetype == edge_type::PARENT && g[target(e,g)].is_a<Misc>() && source(e,g) == l3) {
                auto adapter = std::make_shared<BWAdapter>(bandwidth.L3_to_mem);
                g[e].add_adapter(adapter);
            }
        }
        }
    }

    yloc_status_t ModuleBandwidth::init_graph(Graph &g)
    {
        module_bandwidth_init();
        return YLOC_STATUS_SUCCESS;
    }