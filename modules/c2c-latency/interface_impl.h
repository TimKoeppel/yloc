#pragma once

#include <yloc/modules/module.h>

namespace yloc
{
    class ModuleC2C : public Module
    {
    public:
        ModuleC2C() { m_init_order = Module::init_order::SECOND; }
        
        yloc_status_t init_graph(Graph &graph) override;
    };
}