#include "window.hpp"

namespace ts_extra_utilities
{
    void CWindow::show()
    {
        this->open_ = true;
    }

    void CWindow::hide()
    {
        this->open_ = false;
    }
}
