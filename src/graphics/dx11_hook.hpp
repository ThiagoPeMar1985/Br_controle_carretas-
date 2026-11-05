#pragma once

namespace br_carretas
{
    class CDirectX11Hook
    {
    public:
        ~CDirectX11Hook();

        bool hook_present();
        bool unhook_present();
    };
}
