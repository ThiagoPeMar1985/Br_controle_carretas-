#pragma once
#include <string>

namespace license
{
    // Stable machine hash ("XX-XX-XX-XX-XX-XX-XX-XX") derived from the
    // motherboard/SMBIOS UUID (MachineGuid fallback for boards without one).
    // This is the code the buyer sends to the site so the issued license can
    // be bound to that PC.
    std::string hardware_id();

    enum class status
    {
        unconfigured, // no public key embedded yet - dev builds, treated as licensed
        missing,      // no key entered
        invalid,      // key fails the signature/format check
        expired,      // signature fine but the 365-day term ran out
        valid,
    };

    struct info
    {
        status st = status::missing;
        int days_left = 0;
    };

    // Verifies the pasted key against this machine's hardware id:
    // "BRCDT1.<unix-seconds>.<sig>" where sig is the base64url ECDSA-P256
    // signature (raw r||s, 64 bytes) over "BRCDT1|<hwid>|<unix-seconds>".
    // Crypto runs here - call refresh(), which throttles it.
    info check( const char* key_text );

    // cached verdict - cheap, safe for the game-thread hooks
    bool active();
    // last full verdict for display (written by refresh)
    info last();
    // recompute the cached verdict (internally throttled to ~2 s); call once
    // per frame from the UI/update path. force=true skips the throttle -
    // used by the Activate button so the key validates on the click
    void refresh( bool force = false );

    // store page shown by the Buy button - still empty until the site exists;
    // set the URL here (e.g. "https://store.example/br-controle-de-carretas")
    // and the button starts opening it in the browser
    extern const char* const purchase_url;
}
