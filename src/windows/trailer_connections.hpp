#pragma once

#include <cstdint>

#include "trailer_shared.hpp"

namespace br_carretas
{
    enum class conn_state : uint8_t { off, parked, linked };

    // registro persistente de uma conexão nomeada: par de pontas + estado
    struct conn_record
    {
        void* left = nullptr;
        void* right = nullptr;
        conn_state st = conn_state::off;
    };

    // A0..A(max-1) = cavalo<->T(i); letras B em diante = T(i-1)<->T(i)
    constexpr uint32_t conns_total = max_trailers * 2 - 1;
    inline conn_record conns[ conns_total ];

    inline uint32_t conn_a( const uint32_t i ) { return i; }
    inline uint32_t conn_t( const uint32_t i ) { return max_trailers + i - 1; }

    // nome da conexão: "A0".."A19" na série do cavalo; letra única nas adjacentes
    inline const char* conn_name( const uint32_t idx, char out[ 4 ] )
    {
        if ( idx < max_trailers )
        {
            out[ 0 ] = 'A';
            if ( idx < 10 )
            {
                out[ 1 ] = static_cast< char >( '0' + idx );
                out[ 2 ] = '\0';
            }
            else
            {
                out[ 1 ] = '1';
                out[ 2 ] = static_cast< char >( '0' + idx - 10 );
                out[ 3 ] = '\0';
            }
        }
        else
        {
            out[ 0 ] = static_cast< char >( 'A' + ( idx - max_trailers + 1 ) );
            out[ 1 ] = '\0';
        }
        return out;
    }

    // conexão parked de uma carreta pelo slot canon: B+ se o dono é o
    // antecessor canon, senão A(i) (claim pendurado em outro ponto)
    inline conn_record& conn_slot_of( const int ci, void* owner )
    {
        static conn_record dummy;
        if ( ci < 0 ) return dummy;
        if ( ci > 0 && owner == canon[ ci - 1 ] ) return conns[ conn_t( ci ) ];
        return conns[ conn_a( ci ) ];
    }
}
