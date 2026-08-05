/**
 * M18: Android implementation of the UxPlay dnssd interface.
 *
 * Builds the same _raop._tcp / _airplay._tcp TXT records as lib/dnssd.c
 * (which raop_handlers also reads back for /info responses), but performs no
 * registration itself: the Java side registers the services through Android's
 * NsdManager using the entries exposed by the vkr_dnssd_* accessors.
 *
 * Derived from UxPlay lib/dnssd.c (LGPL 2.1, C) 2011-2012 Juho Vähä-Herttua,
 * modifications (C) fduncanh 2022).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dnssd.h"
#include "dnssdint.h"
#include "global.h"
#include "utils.h"

#define MAX_TXT_BYTES 1024
#define MAX_TXT_ENTRIES 24
#define MAX_ENTRY 192
#define MAX_SERVNAME 256

typedef struct {
    uint8_t wire[MAX_TXT_BYTES]; /* length-prefixed key=value concatenation */
    int wireLen;
    char entries[MAX_TXT_ENTRIES][MAX_ENTRY]; /* "key=value" for NsdManager */
    int count;
} txt_record_t;

struct dnssd_s {
    char *name;
    int name_len;
    char *hw_addr;
    int hw_addr_len;
    char *pk;
    uint32_t features1;
    uint32_t features2;
    unsigned char require_pw;

    txt_record_t raop_txt;
    txt_record_t airplay_txt;
    char raop_servname[MAX_SERVNAME];
    unsigned short raop_port;
    unsigned short airplay_port;
    int raop_registered;
    int airplay_registered;
};

static void txt_add(txt_record_t *txt, const char *key, const char *value) {
    size_t kv = strlen(key) + 1 + strlen(value);
    if (txt->count >= MAX_TXT_ENTRIES || kv > 255 ||
        txt->wireLen + 1 + (int)kv > MAX_TXT_BYTES)
        return;
    txt->wire[txt->wireLen++] = (uint8_t)kv;
    memcpy(txt->wire + txt->wireLen, key, strlen(key));
    txt->wireLen += (int)strlen(key);
    txt->wire[txt->wireLen++] = '=';
    memcpy(txt->wire + txt->wireLen, value, strlen(value));
    txt->wireLen += (int)strlen(value);
    snprintf(txt->entries[txt->count], MAX_ENTRY, "%s=%s", key, value);
    txt->count++;
}

dnssd_t *
dnssd_init(const char *name, int name_len, const char *hw_addr, int hw_addr_len, int *error,
           int require_pw) {
    dnssd_t *dnssd;
    char *end;
    unsigned long features;

    if (error) *error = DNSSD_ERROR_NOERROR;
    dnssd = calloc(1, sizeof(dnssd_t));
    if (!dnssd) {
        if (error) *error = DNSSD_ERROR_OUTOFMEM;
        return NULL;
    }
    dnssd->require_pw = (unsigned char)require_pw;

    features = strtoul(FEATURES_1, &end, 16);
    dnssd->features1 = (uint32_t)features;
    features = strtoul(FEATURES_2, &end, 16);
    dnssd->features2 = (uint32_t)features;

    dnssd->name_len = name_len;
    dnssd->name = calloc(1, name_len + 1);
    dnssd->hw_addr_len = hw_addr_len;
    dnssd->hw_addr = calloc(1, hw_addr_len);
    if (!dnssd->name || !dnssd->hw_addr) {
        free(dnssd->name);
        free(dnssd->hw_addr);
        free(dnssd);
        if (error) *error = DNSSD_ERROR_OUTOFMEM;
        return NULL;
    }
    memcpy(dnssd->name, name, name_len);
    memcpy(dnssd->hw_addr, hw_addr, hw_addr_len);
    return dnssd;
}

void dnssd_destroy(dnssd_t *dnssd) {
    if (dnssd) {
        free(dnssd->name);
        free(dnssd->hw_addr);
        free(dnssd);
    }
}

int dnssd_register_raop(dnssd_t *dnssd, unsigned short port) {
    char features[22];
    snprintf(features, sizeof(features), "0x%X,0x%X", dnssd->features1, dnssd->features2);

    txt_record_t *t = &dnssd->raop_txt;
    memset(t, 0, sizeof(*t));
    txt_add(t, "ch", RAOP_CH);
    txt_add(t, "cn", RAOP_CN);
    txt_add(t, "da", RAOP_DA);
    txt_add(t, "et", RAOP_ET);
    txt_add(t, "vv", RAOP_VV);
    txt_add(t, "ft", features);
    txt_add(t, "am", GLOBAL_MODEL);
    txt_add(t, "md", RAOP_MD);
    txt_add(t, "rhd", RAOP_RHD);
    txt_add(t, "pw", dnssd->require_pw ? "true" : "false");
    txt_add(t, "sr", RAOP_SR);
    txt_add(t, "ss", RAOP_SS);
    txt_add(t, "sv", RAOP_SV);
    txt_add(t, "tp", RAOP_TP);
    txt_add(t, "txtvers", RAOP_TXTVERS);
    txt_add(t, "sf", RAOP_SF);
    txt_add(t, "vs", RAOP_VS);
    txt_add(t, "vn", RAOP_VN);
    txt_add(t, "pk", dnssd->pk ? dnssd->pk : "");

    if (utils_hwaddr_raop(dnssd->raop_servname, sizeof(dnssd->raop_servname), dnssd->hw_addr,
                          dnssd->hw_addr_len) < 0)
        return -1;
    strncat(dnssd->raop_servname, "@",
            sizeof(dnssd->raop_servname) - strlen(dnssd->raop_servname) - 1);
    strncat(dnssd->raop_servname, dnssd->name,
            sizeof(dnssd->raop_servname) - strlen(dnssd->raop_servname) - 1);
    dnssd->raop_port = port;
    dnssd->raop_registered = 1;
    return 0; /* Java performs the actual NSD registration */
}

int dnssd_register_airplay(dnssd_t *dnssd, unsigned short port) {
    char device_id[3 * MAX_HWADDR_LEN];
    char features[22];
    snprintf(features, sizeof(features), "0x%X,0x%X", dnssd->features1, dnssd->features2);
    if (utils_hwaddr_airplay(device_id, sizeof(device_id), dnssd->hw_addr, dnssd->hw_addr_len) <
        0)
        return -1;

    txt_record_t *t = &dnssd->airplay_txt;
    memset(t, 0, sizeof(*t));
    txt_add(t, "deviceid", device_id);
    txt_add(t, "features", features);
    txt_add(t, "flags", AIRPLAY_FLAGS);
    txt_add(t, "model", GLOBAL_MODEL);
    txt_add(t, "pk", dnssd->pk ? dnssd->pk : "");
    txt_add(t, "pw", dnssd->require_pw ? "true" : "false");
    txt_add(t, "pi", AIRPLAY_PI);
    txt_add(t, "srcvers", AIRPLAY_SRCVERS);
    txt_add(t, "vv", AIRPLAY_VV);

    dnssd->airplay_port = port;
    dnssd->airplay_registered = 1;
    return 0;
}

void dnssd_unregister_raop(dnssd_t *dnssd) { dnssd->raop_registered = 0; }
void dnssd_unregister_airplay(dnssd_t *dnssd) { dnssd->airplay_registered = 0; }

const char *dnssd_get_airplay_txt(dnssd_t *dnssd, int *length) {
    *length = dnssd->airplay_txt.wireLen;
    return (const char *)dnssd->airplay_txt.wire;
}

const char *dnssd_get_name(dnssd_t *dnssd, int *length) {
    *length = dnssd->name_len;
    return dnssd->name;
}

const char *dnssd_get_hw_addr(dnssd_t *dnssd, int *length) {
    *length = dnssd->hw_addr_len;
    return dnssd->hw_addr;
}

uint64_t dnssd_get_airplay_features(dnssd_t *dnssd) {
    uint64_t features = ((uint64_t)dnssd->features2) << 32;
    features += (uint64_t)dnssd->features1;
    return features;
}

void dnssd_set_pk(dnssd_t *dnssd, char *pk_str) { dnssd->pk = pk_str; }

void dnssd_set_airplay_features(dnssd_t *dnssd, int bit, int val) {
    uint32_t mask;
    uint32_t *features;
    if (bit < 0 || bit > 63 || val < 0 || val > 1) return;
    if (bit >= 32) {
        mask = 0x1u << (bit - 32);
        features = &(dnssd->features2);
    } else {
        mask = 0x1u << bit;
        features = &(dnssd->features1);
    }
    if (val) *features |= mask;
    else *features &= ~mask;
}

/* ---- accessors for the Java/NSD side -------------------------------------- */

const char *vkr_dnssd_service_name(dnssd_t *dnssd, int which) {
    return which == 0 ? dnssd->raop_servname : dnssd->name;
}
unsigned short vkr_dnssd_service_port(dnssd_t *dnssd, int which) {
    return which == 0 ? dnssd->raop_port : dnssd->airplay_port;
}
int vkr_dnssd_txt_count(dnssd_t *dnssd, int which) {
    return which == 0 ? dnssd->raop_txt.count : dnssd->airplay_txt.count;
}
const char *vkr_dnssd_txt_entry(dnssd_t *dnssd, int which, int idx) {
    txt_record_t *t = which == 0 ? &dnssd->raop_txt : &dnssd->airplay_txt;
    return idx >= 0 && idx < t->count ? t->entries[idx] : "";
}
