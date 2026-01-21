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
// MODULE C2C START
/////////////////////////////////////////////

namespace yloc
{
    class C2CAdapter : public Adapter
    {
    public:
        C2CAdapter(uint64_t latency) : m_latency{latency} {};

        std::string to_string() const override 
        {
            return "C2C Latency: " + std::to_string(m_latency) + " ns";
        }

        std::optional<uint64_t> latency() const override
        {
            return m_latency;
        }

    private:
        uint64_t m_latency;   // measured latency
    };


}