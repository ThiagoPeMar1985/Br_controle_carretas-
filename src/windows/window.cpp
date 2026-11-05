#include "window.hpp"

namespace br_carretas
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
