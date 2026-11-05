#pragma once
#include "window.hpp"

namespace br_carretas
{
    class CHooksWindow : public CWindow
    {
    public:
        bool init() override { return true; }
        void render() override;
    };
}
