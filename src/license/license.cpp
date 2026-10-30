#include "license.hpp"
#include "settings/settings.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace
{
    // header of the blob returned by GetSystemFirmwareTable('RSMB', 0)
    #pragma pack(push, 1)
    struct RawSMBIOSData
    {
        BYTE used20;
        BYTE major;
        BYTE minor;
        BYTE dmi_revision;
        DWORD length;
        BYTE data[1];
    };
    #pragma pack(pop)

    // Type 1 (System Information) carries the board UUID at formatted offset
    // 0x08 - 16 raw bytes. Byte order is irrelevant: the hash only needs to
    // be stable on the same machine, not a canonical UUID string
    bool motherboard_uuid( BYTE out[16] )
    {
        constexpr DWORD RSMB = 0x52534D42; // 'RSMB'
        const UINT need = GetSystemFirmwareTable( RSMB, 0, nullptr, 0 );
        if ( need == 0 ) return false;
        std::vector<BYTE> buf( need );
        if ( GetSystemFirmwareTable( RSMB, 0, buf.data(), need ) == 0 ) return false;

        const auto* raw = reinterpret_cast< const RawSMBIOSData* >( buf.data() );
        const BYTE* p = raw->data;
        const BYTE* const end = p + raw->length;
        while ( p + 2 <= end )
        {
            const BYTE type = p[0];
            const BYTE len = p[1];
            if ( type == 1 && len >= 0x18 && p + len <= end )
            {
                memcpy( out, p + 0x08, 16 );
                // boards without a real UUID report all-zero or all-FF
                for ( int i = 0; i < 16; ++i )
                {
                    if ( out[ i ] != 0 && out[ i ] != 0xFF ) return true;
                }
                return false;
            }
            if ( type == 127 || len < 2 ) break; // 127 = end-of-table
            // skip the formatted section then the string set (double NUL)
            const BYTE* s = p + len;
            while ( s + 1 < end && !( s[ 0 ] == 0 && s[ 1 ] == 0 ) ) ++s;
            p = s + 2;
        }
        return false;
    }

    bool machine_guid( std::string& out )
    {
        char buf[64]{};
        DWORD size = sizeof( buf );
        if ( RegGetValueA( HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography",
             "MachineGuid", RRF_RT_REG_SZ, nullptr, buf, &size ) == ERROR_SUCCESS && buf[0] )
        {
            out = buf;
            return true;
        }
        return false;
    }

    std::string sha256_id( const void* data, const size_t size )
    {
        BCRYPT_ALG_HANDLE alg = nullptr;
        if ( BCryptOpenAlgorithmProvider( &alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0 ) != 0 )
            return {};
        BYTE hash[32]{};
        const NTSTATUS st = BCryptHash( alg, nullptr, 0,
            static_cast< PUCHAR >( const_cast< void* >( data ) ),
            static_cast< ULONG >( size ), hash, sizeof( hash ) );
        BCryptCloseAlgorithmProvider( alg, 0 );
        if ( st != 0 ) return {};

        // 8-byte code, four groups of two bytes - readable over a screenshot
        char id[20];
        snprintf( id, sizeof( id ),
            "%02X%02X-%02X%02X-%02X%02X-%02X%02X",
            hash[0], hash[1], hash[2], hash[3],
            hash[4], hash[5], hash[6], hash[7] );
        return id;
    }

    std::string compute_hardware_id()
    {
        // product salt keeps this hash domain separate from any other tool -
        // and from the ATS build: the same PC shows a different code there
        std::string seed = "brcdt|";
        BYTE uuid[16];
        if ( motherboard_uuid( uuid ) )
        {
            seed.append( reinterpret_cast< const char* >( uuid ), sizeof( uuid ) );
        }
        else
        {
            std::string guid;
            if ( !machine_guid( guid ) ) return {};
            seed += guid;
        }
        return sha256_id( seed.data(), seed.size() );
    }
}

std::string license::hardware_id()
{
    static const std::string cached = compute_hardware_id();
    return cached;
}

//========================================================================
// License check - ECDSA P-256 over "BRCDT1|<hwid>|<unix-seconds>".
//
// The site signs that payload with the PRIVATE key (kept only on the
// server); the plugin carries only the PUBLIC key, so a key pulled out of
// this dll can never mint licenses. Key pair, once (openssl):
//   openssl ecparam -name prime256v1 -genkey -noout -out BR-CDT-private.pem
//   openssl ec -in BR-CDT-private.pem -pubout -outform DER | tail -c 64 | xxd -i
// The last command prints the 64 bytes (X||Y) to paste into k_public_key
// below. Until then the blob stays zeroed and check() reports
// "unconfigured" - dev builds run licensed; REPLACE BEFORE PUBLISHING.
//========================================================================
namespace
{
    const char* const k_purchase_url = ""; // TODO: store page URL when it exists

    // BCRYPT_ECCKEY_BLOB (ECDSA_P256 public, cbKey=32) followed by X||Y -
    // generated by re/make_license.py keygen (private key in license_keys/,
    // git-ignored - re-run `pubkey` to reprint this block)
    unsigned char k_public_key[ 8 + 64 ] = {
        0x45, 0x43, 0x53, 0x31, 0x20, 0x00, 0x00, 0x00, // magic ECS1, cbKey 32
        0x70, 0x37, 0xC9, 0xFC, 0x81, 0xC7, 0x4C, 0xF2,
        0xC4, 0xDD, 0x7A, 0xB6, 0xBB, 0x6C, 0x0E, 0x31,
        0x54, 0xE1, 0x5A, 0x03, 0xD6, 0x26, 0x32, 0xEB,
        0xBE, 0xFD, 0xCF, 0x15, 0x34, 0x3F, 0xA5, 0x5F,
        0xF0, 0x25, 0x6E, 0x87, 0xDB, 0x77, 0xC2, 0xE7,
        0x55, 0x95, 0xEA, 0xB7, 0x99, 0xF8, 0xE7, 0x49,
        0x08, 0xE5, 0x69, 0xE3, 0xE7, 0x27, 0x84, 0x1C,
        0x6A, 0xAF, 0x6A, 0xEE, 0x19, 0x74, 0x48, 0xE2,
    };

    bool pubkey_configured()
    {
        for ( size_t i = 8; i < sizeof( k_public_key ); ++i )
            if ( k_public_key[ i ] != 0 ) return true;
        return false;
    }

    int b64url_val( const char c )
    {
        if ( c >= 'A' && c <= 'Z' ) return c - 'A';
        if ( c >= 'a' && c <= 'z' ) return c - 'a' + 26;
        if ( c >= '0' && c <= '9' ) return c - '0' + 52;
        if ( c == '-' ) return 62;
        if ( c == '_' ) return 63;
        return -1;
    }

    // strict decode; returns false on any non-alphabet char or bad length
    bool b64url_decode( const std::string& s, BYTE* out, const size_t out_len )
    {
        std::vector<BYTE> buf( ( s.size() * 6 ) / 8 + 4 );
        size_t n = 0;
        int acc = 0, bits = 0;
        for ( const char c : s )
        {
            const int v = b64url_val( c );
            if ( v < 0 ) return false;
            acc = ( acc << 6 ) | v;
            bits += 6;
            if ( bits >= 8 )
            {
                bits -= 8;
                if ( n >= buf.size() ) return false;
                buf[ n++ ] = static_cast< BYTE >( ( acc >> bits ) & 0xFF );
            }
        }
        if ( n != out_len ) return false;
        memcpy( out, buf.data(), n );
        return true;
    }

    std::string lower( std::string s )
    {
        for ( char& c : s ) c = static_cast< char >( std::tolower( c & 0xFF ) );
        return s;
    }

    // digits -> epoch seconds (UTC); -1 when malformed. The expiry is an
    // exact timestamp so a license lives N*24h from the moment it is minted
    __time64_t epoch_secs( const std::string& s )
    {
        if ( s.empty() || s.size() > 11 ) return -1;
        for ( const char c : s ) if ( !isdigit( c & 0xFF ) ) return -1;
        return _atoi64( s.c_str() );
    }

    bool ecdsa_verify( const void* payload, const ULONG payload_len,
        const BYTE* sig, const ULONG sig_len )
    {
        // BCryptHash needs a HASH provider handle - the ECDSA provider can't
        // be reused for the digest, so open SHA256 separately
        BCRYPT_ALG_HANDLE hash_alg = nullptr;
        if ( BCryptOpenAlgorithmProvider( &hash_alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0 ) != 0 )
            return false;
        BYTE hash[32]{};
        const bool hashed = BCryptHash( hash_alg, nullptr, 0,
            static_cast< PUCHAR >( const_cast< void* >( payload ) ),
            payload_len, hash, sizeof( hash ) ) == 0;
        BCryptCloseAlgorithmProvider( hash_alg, 0 );
        if ( !hashed ) return false;

        BCRYPT_ALG_HANDLE alg = nullptr;
        if ( BCryptOpenAlgorithmProvider( &alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0 ) != 0 )
            return false;
        BCRYPT_KEY_HANDLE key = nullptr;
        bool ok = BCryptImportKeyPair( alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key,
                    k_public_key, sizeof( k_public_key ), 0 ) == 0;
        if ( ok )
        {
            ok = BCryptVerifySignature( key, nullptr, hash, sizeof( hash ),
                const_cast< BYTE* >( sig ), sig_len, 0 ) == 0;
            BCryptDestroyKey( key );
        }
        BCryptCloseAlgorithmProvider( alg, 0 );
        return ok;
    }
}

license::info license::check( const char* key_text )
{
    info out;
    if ( !pubkey_configured() ) { out.st = status::unconfigured; return out; }
    if ( key_text == nullptr || key_text[0] == 0 ) { out.st = status::missing; return out; }

    // "BRCDT1.<unix-seconds>.<b64url(sig r||s 64B)>"
    const std::string k = key_text;
    if ( k.rfind( "BRCDT1.", 0 ) != 0 ) { out.st = status::invalid; return out; }
    const size_t d2 = k.find( '.', 7 );
    if ( d2 == std::string::npos || d2 + 1 >= k.size() ) { out.st = status::invalid; return out; }
    const std::string expiry = k.substr( 7, d2 - 7 );
    const std::string sig_text = k.substr( d2 + 1 );
    if ( sig_text.find( '.' ) != std::string::npos ) { out.st = status::invalid; return out; }

    BYTE sig[64]{};
    const __time64_t expiry_at = epoch_secs( expiry );
    if ( expiry_at < 0 || !b64url_decode( sig_text, sig, sizeof( sig ) ) )
    {
        out.st = status::invalid;
        return out;
    }

    const std::string payload = "BRCDT1|" + lower( hardware_id() ) + "|" + expiry;
    if ( !ecdsa_verify( payload.data(), static_cast< ULONG >( payload.size() ), sig, sizeof( sig ) ) )
    {
        out.st = status::invalid;
        return out;
    }

    // genuine key: expiry still running? (clock rollback also counts as out)
    const __time64_t now = _time64( nullptr );
    if ( now < ts_extra_utilities::settings::g_settings.license_seen )
    {
        out.st = status::expired;
        return out;
    }
    out.days_left = static_cast< int >( ( expiry_at - now + 86399 ) / 86400 );
    if ( out.days_left <= 0 ) { out.st = status::expired; return out; }

    // remember the furthest moment seen - a clock set backwards can no longer
    // stretch a license it has already outlived
    if ( now > ts_extra_utilities::settings::g_settings.license_seen )
    {
        ts_extra_utilities::settings::g_settings.license_seen = static_cast< int32_t >( now );
        ts_extra_utilities::settings::save();
    }
    out.st = status::valid;
    return out;
}

//========================================================================
// cached verdict - hooks on the game thread read the atomic, the UI path
// refreshes it at most every ~2 s so a pasted key activates without restart
//========================================================================
namespace
{
    std::atomic< int > g_active{ 0 }; // locked until the first real check passes
    std::atomic< ULONGLONG > g_checked_at{ 0 };
    std::atomic< int > g_last_st{ static_cast< int >( license::status::missing ) };
    std::atomic< int > g_last_days{ 0 };
}

bool license::active()
{
    return g_active.load( std::memory_order_relaxed ) != 0;
}

license::info license::last()
{
    return { static_cast< status >( g_last_st.load( std::memory_order_relaxed ) ),
        g_last_days.load( std::memory_order_relaxed ) };
}

void license::refresh( const bool force )
{
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG last = g_checked_at.load( std::memory_order_relaxed );
    if ( !force && now - last < 2000 ) return;
    g_checked_at.store( now, std::memory_order_relaxed );

    const info r = check( ts_extra_utilities::settings::g_settings.license_key );
    g_active.store( r.st == status::valid || r.st == status::unconfigured ? 1 : 0,
        std::memory_order_relaxed );
    g_last_st.store( static_cast< int >( r.st ), std::memory_order_relaxed );
    g_last_days.store( r.days_left, std::memory_order_relaxed );
}

const char* const license::purchase_url = k_purchase_url;
