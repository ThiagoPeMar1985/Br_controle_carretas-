#pragma once

namespace br_carretas
{
    class CDirectInput8Hook
    {
    public:
        ~CDirectInput8Hook();

        bool hook();
        bool unhook();
    };
}
