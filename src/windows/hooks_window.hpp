#pragma once
#include "window.hpp"

namespace ts_extra_utilities
{
    class CHooksWindow : public CWindow
    {
    public:
        bool init() override { return true; }
        void render() override;
    };
}
