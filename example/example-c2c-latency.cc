#include <boost/graph/filtered_graph.hpp>
#include <boost/graph/graph_utility.hpp> // print_graph
#include <boost/graph/graphviz.hpp>      // write_graphviz
#include <boost/property_map/property_map.hpp>

#include <yloc/yloc.h>

#include <memory>
#include <vector>

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

        // mask1, mask2 convertible to cpu_set_t

        // start thread1 -> sched_setaffinity(mask1)
        // start thread2 -> sched_setaffinity(mask2)

        // run c2c code between thread1, thread2
        uint64_t latency; /* = TODO */

        return latency;
    }

    uint64_t measure_c2c_bandwidth(yloc::Graph &g, vertex_t v1, vertex_t v2)
    {
        uint64_t bandwidth; /* = TODO */

        return bandwidth;
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

                    // dummy values
                    latency = v1 + v2;
                    bandwidth = v1 * v2;

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
