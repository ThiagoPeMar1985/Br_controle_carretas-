// Garage flow - a trailer returned to the garage is destroyed, and when it
// is called back with the truck in the garage it respawns and couples
// natively at the garage dock, checkbox on or off. The plugin's only job
// here is to drop the marks a destroyed piece left behind: without that, a
// recalled trailer inherits a stale parked pose or a stale detach mark.

#include "trailer_shared.hpp"
#include "trailer_coupling_internal.hpp"

#include "core.hpp"

namespace br_carretas
{
    void garage_update()
    {
        for ( uint32_t i = 0; i < max_trailers; ++i )
        {
            void* t = canon[ i ];
            if ( t != nullptr && is_trailer_obj( t ) ) continue;
            // slot's trailer is dead (garage took it or the world dropped
            // it) - the marks die with it; a recalled trailer is a fresh
            // native piece that attaches at the garage dock
            if ( !detached_idx[ i ] && !parked_xform_valid[ i ] &&
                 !anchor_valid[ i ] && !carry_hold[ i ] ) continue;
            detached_idx[ i ] = false;
            parked_xform_valid[ i ] = false;
            anchor_valid[ i ] = false;
            carry_hold[ i ] = false;
            carry_park[ i ] = 0;
            CCore::g_instance->info( "garage: dead canon slot {} marks dropped", i );
        }
        // pointer-based marks on dead objects die too - a recycled address
        // is no longer a trailer-class object
        for ( void*& d : plugin_detached )
            if ( d != nullptr && !is_trailer_obj( d ) ) d = nullptr;
        for ( void*& d : detached_trailers )
            if ( d != nullptr && !is_trailer_obj( d ) ) d = nullptr;
    }
}
