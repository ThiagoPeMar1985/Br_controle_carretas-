#pragma once
#include <cstdint>
#include <Windows.h>

#include "memory_scan.hpp"

namespace br_carretas::memory
{
    inline uint64_t get_address_for_pattern( const char* pattern, const uint64_t offset = 0 )
    {
        thread_local static bool initialized = false;
        thread_local static uintptr_t game_base = 0;
        thread_local static uint32_t img_size = 0;

        if ( !initialized )
        {
            game_base = reinterpret_cast< uint64_t >( GetModuleHandle( nullptr ) );
            if ( game_base == NULL ) return NULL;
            const auto header = reinterpret_cast< const IMAGE_DOS_HEADER* >( game_base );
            const auto nt_header = reinterpret_cast< const IMAGE_NT_HEADERS64* >(
                reinterpret_cast< const uint8_t* >( header ) + header->e_lfanew );
            img_size = nt_header->OptionalHeader.SizeOfImage;
            initialized = true;
        }

        const auto scan_result = pattern::scan(
            pattern,
            game_base,
            img_size );

        if ( scan_result == NULL ) return NULL;

        return scan_result + offset;
    }

    inline uint64_t get_address_from_offset( const uint64_t offset )
    {
        return reinterpret_cast< uint64_t >( GetModuleHandle( nullptr ) ) + offset - 0x140000000;
    }

    // fingerprint check for a fixed image offset: the bytes at the address
    // must still match the signature captured on the exe the offset was
    // derived from - this is what lets the plugin run on any game build
    // whose code did not move, and refuse one whose did
    inline bool offset_bytes_match( const uint64_t offset, const uint8_t* sig, const uint32_t len )
    {
        const uint64_t addr = get_address_from_offset( offset );
        if ( addr == 0 ) return false;
        MEMORY_BASIC_INFORMATION mbi{};
        if ( VirtualQuery( reinterpret_cast< const void* >( addr ), &mbi, sizeof( mbi ) ) == 0 ||
             mbi.State != MEM_COMMIT || ( mbi.Protect & ( PAGE_NOACCESS | PAGE_GUARD ) ) != 0 )
        {
            return false;
        }
        return len == 0 || memcmp( reinterpret_cast< const void* >( addr ), sig, len ) == 0;
    }

    template < class T >
    T* get_function_from_offset( const uint64_t offset )
    {
        return reinterpret_cast< T* >( reinterpret_cast< uint64_t >( GetModuleHandle( nullptr ) ) + offset - 0x140000000 );
    }

    template < class T >
    T* get_function_from_pattern( const char* pattern, const uint64_t offset = 0 )
    {
        const auto address = get_address_for_pattern( pattern, 0 );

        if ( address == 0 )
        {
            return nullptr;
        }

        return reinterpret_cast< T* >( address + offset );
    }

    template < class T >
    T* get_vtable_function( const uint64_t vtable_address, const uint64_t offset )
    {
        return reinterpret_cast< T* >( *reinterpret_cast< uint64_t* >( vtable_address + offset * 0x08 ) );
    }

    inline uint64_t as_offset( const uint64_t address )
    {
        return address - reinterpret_cast< uint64_t >( GetModuleHandle( nullptr ) );
    }
}
