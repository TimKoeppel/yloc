#pragma once

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

/////////////////////////////////////////////
// MODULE BANDWIDTH START
/////////////////////////////////////////////

namespace yloc
{
    class BWAdapter : public Adapter
    {
    public:
        BWAdapter(uint64_t bandwidth) : m_bandwidth{bandwidth} {};

        std::string to_string() const override
        {
            std::stringstream ss;
            /* TODO: string representation of bandwidth */
            return ss.str();
        }

        std::optional<uint64_t> bandwidth() const override
        {
            return m_bandwidth;
        }

    private:
        uint64_t m_bandwidth;   // measured bandwidth in bits/s
    };


}