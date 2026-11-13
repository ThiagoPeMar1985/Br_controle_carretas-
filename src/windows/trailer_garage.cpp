#include "trailer_shared.hpp"
#include "trailer_coupling_internal.hpp"

#include "core.hpp"

namespace br_carretas
{
    // 166 - por tick: limpa marcas de carretas destruídas (garagem/recall)
    void garage_update()
    {
        for ( uint32_t i = 0; i < max_trailers; ++i )
        {
            void* t = canon[ i ];
            if ( t != nullptr && is_trailer_obj( t ) ) continue;

            if ( !detached_idx[ i ] && !parked_xform_valid[ i ] &&
                 !anchor_valid[ i ] && !carry_hold[ i ] ) continue;
            detached_idx[ i ] = false;
            parked_xform_valid[ i ] = false;
            anchor_valid[ i ] = false;
            carry_hold[ i ] = false;
            carry_park[ i ] = 0;
            CCore::g_instance->info( "garage: dead canon slot {} marks dropped", i );
        }

        for ( void*& d : plugin_detached )
            if ( d != nullptr && !is_trailer_obj( d ) ) d = nullptr;
        for ( void*& d : detached_trailers )
            if ( d != nullptr && !is_trailer_obj( d ) ) d = nullptr;
    }
    namespace
    {
        const bool fn_idx_reg = []()
        {
            fn_index_add( fn_addr_of( &garage_update ), 166, "garage_update" );
            return true;
        }();
    }
}
