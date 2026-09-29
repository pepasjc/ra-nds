// sd:/_nds/ra/net.bin: what RA Sync learned the last time the WiFi came up,
// so that nds-bootstrap-ra can send unlocks while a game runs without
// scanning, DHCP or DNS.  No secrets: the WiFi key stays in the console's
// own connection settings (NVRAM), found again by the SSID.  Shared with
// nds-bootstrap-ra; plain types only.
#ifndef RA_NETPROFILE_H
#define RA_NETPROFILE_H

#define RA_NET_PROFILE_MAGIC 0x504E4152 // 'RANP'
#define RA_NET_PROFILE_VERSION 1

typedef struct RaNetProfile {
    u32 magic;
    u16 version;
    u16 size;           // sizeof(RaNetProfile)
    // The access point (WlanBssDesc fields)
    u8 bssid[6];
    u16 ssid_len;
    char ssid[32];
    u16 ieee_caps;
    u16 ieee_basic_rates;
    u16 ieee_all_rates;
    u8 auth_type;       // WlanBssAuthType
    u8 channel;
    u8 rssi;
    u8 conn_type;       // WfcConnType of the settings slot used
    // IPv4, network byte order: the DHCP lease and the RetroAchievements
    // server, as resolved then
    u32 ip;
    u32 netmask;
    u32 gateway;
    u32 dns[2];
    u32 ra_server;
} RaNetProfile;

#endif
