#pragma once

#include <cstdint>
#include <intrin.h>

namespace br_carretas
{
    enum hk_slot : int
    {
        HK_CAM, HK_COMBO, HK_HOSE, HK_LAMPM, HK_LAMPP, HK_CABL,
        HK_BSET, HK_FSET, HK_BCH, HK_FCH, HK_CSLV, HK_SAPP, HK_CRASH,
        HK_LOCKW, HK_REND, HK_UPD, HK_COUNT
    };

    // cada slot em sua propria linha de cache: sim e render escrevem juntos
    struct alignas( 64 ) hk_slot_stats { uint64_t calls = 0; uint64_t tsc = 0; };
    inline hk_slot_stats hk_stats[ HK_COUNT ] = {};
    inline const char* hk_names[ HK_COUNT ] =
        { "cam", "combo", "hose", "lampm", "lampp", "cabl",
          "bset", "fset", "bch", "fch", "cslv", "sapp", "crash",
          "lockw", "rend", "upd" };

    struct hk_timer
    {
        int s;
        uint64_t t0;
        explicit hk_timer( const int slot ) : s( slot ), t0( __rdtsc() ) {}
        ~hk_timer()
        {
            hk_stats[ s ].tsc += __rdtsc() - t0;
            ++hk_stats[ s ].calls;
        }
        hk_timer( const hk_timer& ) = delete;
        hk_timer& operator=( const hk_timer& ) = delete;
    };
}
