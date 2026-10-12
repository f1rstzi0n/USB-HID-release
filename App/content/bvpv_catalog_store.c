#include "bvpv_catalog_store.h"
#include "bvpv_errcode.h"
#include "fw_crypto.h"
#include "bvpv_catalog_bindings.h"
#include <string.h>

enum { P_IDLE, P_BOOT_META0, P_BOOT_META1, P_BOOT_SELECT, P_HEADER0, P_HEADER1,
    P_SIGNATURE, P_BOOT_BODY, P_PREFLIGHT_BLOCK, P_WRITE_HEADER, P_WRITE_BLOCK,
    P_ERASE_WAIT, P_PROGRAM_WAIT, P_PROGRAM_CHECK, P_VERIFY_HEADER0, P_VERIFY_HEADER1,
    P_VERIFY_BODY, P_META_ERASE, P_META_WAIT, P_META_PROGRAM, P_META_PROGRAM_WAIT,
    P_META_CHECK, P_META_MARK_WAIT };
static const uint8_t schema_sha[32] = BVPV_CATALOG_SCHEMA_SHA;
static const uint8_t target[16] = BVPV_CATALOG_TARGET;
static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t r32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static void w16(uint8_t *p, uint16_t n) { p[0]=(uint8_t)n; p[1]=(uint8_t)(n>>8); }
static void w32(uint8_t *p, uint32_t n) { p[0]=(uint8_t)n; p[1]=(uint8_t)(n>>8); p[2]=(uint8_t)(n>>16); p[3]=(uint8_t)(n>>24); }
static uint32_t address(const uint8_t *p) { return ((uint32_t)p[0]<<16)|((uint32_t)p[1]<<8)|p[2]; }
static uint32_t section_offset(const uint8_t *h, uint8_t n) { return r32(h+BVPV_CATALOG_H_SECTIONS+8u*n); }
static uint32_t section_bytes(const uint8_t *h, uint8_t n) { return r32(h+BVPV_CATALOG_H_SECTIONS+8u*n+4u); }
static uint16_t section_count(const uint8_t *h, uint8_t n) { return r16(h+(n==4u?BVPV_CATALOG_H_ERR_COUNT:BVPV_CATALOG_H_PARAM_COUNT+2u*n)); }
static void answer(bvpv_catalog_store_t *s, uint8_t status, uint16_t n)
{
    s->reply[0]=status; s->reply_len=n; s->reply_ready=true;
    if (status) s->last_error=status;
}
static void release_session(bvpv_catalog_store_t *s)
{
    if (s->signing) { fw_p256_cancel(); s->signing=false; }
    if (s->session && s->io.reserve) (void)s->io.reserve(s->io.ctx,false);
    s->session=false; s->preflight_ready=false; s->cancelled=false; s->phase=P_IDLE;
}
static void failed(bvpv_catalog_store_t *s, uint8_t status)
{
    bool boot=s->booting;
    s->last_error=status;
    if (status==BVPV_ERR_FLASH) s->fault=true;
    if (boot) { s->available=false; s->booting=false; }
    release_session(s);
    if (!boot) answer(s,status,1);
}
bool bvpv_catalog_store_ready(const bvpv_catalog_store_t *s) { return s->available && !s->fault && !s->session && !s->booting; }
static bool ascii(const uint8_t *p, uint16_t n)
{
    uint16_t i;
    for(i=0;i<n;i++) if(p[i]<0x20u || p[i]>0x7Eu) return false;
    return true;
}
static bool utf8(const uint8_t *p, uint16_t n)
{
    uint16_t i=0;
    while(i<n)
    {
        uint8_t a=p[i++], count, b;
        if(a<0x80u) { if(!a) return false; continue; }
        if(a>=0xC2u && a<=0xDFu) count=1;
        else if(a>=0xE0u && a<=0xEFu) count=2;
        else if(a>=0xF0u && a<=0xF4u) count=3;
        else return false;
        if(count>n-i) return false;
        b=p[i];
        if((a==0xE0u && b<0xA0u) || (a==0xEDu && b>=0xA0u) ||
           (a==0xF0u && b<0x90u) || (a==0xF4u && b>=0x90u)) return false;
        while(count--) if((p[i++]&0xC0u)!=0x80u) return false;
    }
    return true;
}
static bool mark_seen(bvpv_catalog_store_t *s, uint16_t index)
{
    uint8_t mask=(uint8_t)(1u<<(index&7u));
    if(s->seen[index>>3]&mask) return false;
    s->seen[index>>3]|=mask;
    return true;
}
static bool numeric_valid(uint8_t type,const uint8_t *p)
{
    if(type==BVPV_TYPE_FLOAT32) return (r32(p)&0x7F800000u)!=0x7F800000u;
    return true;
}
static bool access_subset(uint8_t proposed,uint8_t hard)
{
    if(proposed>BVPV_ACC_OWO) return false;
    if(proposed==hard || proposed==BVPV_ACC_OOO) return true;
    if(hard==BVPV_ACC_OWO || proposed==BVPV_ACC_OWO) return false;
    return (proposed==BVPV_ACC_ROO && bvpv_access_readable(hard)) ||
        (proposed==BVPV_ACC_RWO && hard==BVPV_ACC_RWA);
}
static uint64_t text_hash(const uint8_t *p,uint16_t n)
{
    uint64_t hash=UINT64_C(14695981039346656037);uint16_t i;
    for(i=0;i<n;i++) { hash^=p[i];hash*=UINT64_C(1099511628211); }
    return hash;
}
static bool unique_text(bvpv_catalog_store_t *s,const uint8_t *p,uint16_t n)
{
    uint64_t fingerprint=text_hash(p,n);uint16_t i;
    for(i=0;i<s->text_count;i++) if(s->text_hashes[i]==fingerprint) return false;
    if(s->text_count>=BVPV_CATALOG_MAX_PARAMS) return false;
    s->text_hashes[s->text_count++]=fingerprint; return true;
}
static uint8_t validate_param(bvpv_catalog_store_t *s)
{
    const uint8_t *r=s->record;
    uint16_t n=s->record_len, name, pos, ext_len, esz;
    uint8_t min[8]={0},max[8]={0}, tags=0, range_flags=0;
    bool range_changed=false;
    int32_t index;
    bvpv_param_t hard;
    if(n<13u || (name=r[9])==0u || name>63u || name>n-12u || !ascii(r+10,name)) return BVPV_ERR_LENGTH;
    if(!unique_text(s,r+10,name)) return BVPV_ERR_STATE;
    index=bvpv_catalog_find(s->runtime,address(r));
    if(index<0 || !mark_seen(s,(uint16_t)index) || !bvpv_catalog_get(s->runtime,(uint16_t)index,&hard)) return BVPV_ERR_STATE;
    esz=bvpv_param_elem_size(&hard);
    if(r[3]!=hard.type || r16(r+7)!=hard.elem_count || !access_subset(r[4],hard.access) ||
        r[5]&~BVPV_FLAG_DEFAULT_VISIBLE || r[6] || (r[5]&~hard.flags)) return BVPV_ERR_STATE;
    pos=(uint16_t)(12u+name); ext_len=r16(r+10u+name);
    if(ext_len!=n-pos) return BVPV_ERR_LENGTH;
    while(pos<n)
    {
        uint8_t tag;
        uint16_t len;
        const uint8_t *v;
        if((uint16_t)(n-pos)<3u) return BVPV_ERR_LENGTH;
        tag=r[pos]; len=r16(r+pos+1); pos=(uint16_t)(pos+3u);
        if(len>n-pos || tag<1u || tag>5u || (tags&(1u<<tag))) return BVPV_ERR_LENGTH;
        tags|=(uint8_t)(1u<<tag); v=r+pos;
        if(tag==1u || tag==2u)
        {
            if(!esz || esz>8u || hard.type>=BVPV_TYPE_TEXT8 || len!=esz || !numeric_valid(hard.type,v)) return BVPV_ERR_TYPE_MISMATCH;
            if(tag==1u) { memcpy(min,v,esz); range_flags|=BVPV_EXT_HAS_MIN; }
            else { memcpy(max,v,esz); range_flags|=BVPV_EXT_HAS_MAX; }
        }
        else if(tag==3u)
        {
            if(len!=esz || (esz && !numeric_valid(hard.type,v))) return BVPV_ERR_TYPE_MISMATCH;
        }
        else if(tag==4u)
        {
            uint16_t q=0, count=0;
            while(q<len)
            {
                uint8_t bytes; uint16_t previous=0;
                if((uint16_t)(len-q)<5u || ++count>32u) return BVPV_ERR_LENGTH;
                while(previous<q) { if(r32(v+previous)==r32(v+q)) return BVPV_ERR_STATE; previous=(uint16_t)(previous+5u+v[previous+4u]); }
                bytes=v[q+4]; q=(uint16_t)(q+5u);
                if(bytes>len-q || !utf8(v+q,bytes)) return BVPV_ERR_LENGTH;
                q=(uint16_t)(q+bytes);
            }
        }
        else if(len>255u || !utf8(v,len)) return BVPV_ERR_LENGTH;
        pos=(uint16_t)(pos+len);
    }
    if((hard.ext_flags&BVPV_EXT_HAS_MIN) && (!(range_flags&BVPV_EXT_HAS_MIN) || bvpv_value_compare(hard.type,min,hard.min)<0)) return BVPV_ERR_OUT_OF_RANGE;
    if((hard.ext_flags&BVPV_EXT_HAS_MAX) && (!(range_flags&BVPV_EXT_HAS_MAX) || bvpv_value_compare(hard.type,max,hard.max)>0)) return BVPV_ERR_OUT_OF_RANGE;
    if((range_flags&3u)==3u && bvpv_value_compare(hard.type,min,max)>0) return BVPV_ERR_OUT_OF_RANGE;
    range_changed=(range_flags!=(hard.ext_flags&3u)) ||
        ((range_flags&1u) && memcmp(min,hard.min,esz)) || ((range_flags&2u) && memcmp(max,hard.max,esz));
    if(range_changed)
    {
        bvpv_catalog_range_t *entry;
        if(s->candidate_range_count>=BVPV_CATALOG_MAX_RANGES) return BVPV_ERR_OUT_OF_RANGE;
        entry=&s->ranges[s->candidate_range_count++];
        if(s->apply_policy) { s->range_count=s->candidate_range_count; entry->index=(uint16_t)index; entry->flags=range_flags; memcpy(entry->min,min,8); memcpy(entry->max,max,8); }
    }
    if(s->apply_policy) { s->policy_access[index]=r[4]; s->policy_flags[index]=r[5]; s->param_ordinal[index]=s->ordinal; }
    return 0;
}
static bool group_present(const bvpv_catalog_store_t *s,uint16_t id)
{
    uint16_t i;if(!id)return true;
    for(i=0;i<s->text_count;i++)if((uint16_t)(s->text_hashes[i]>>48)==id)return true;
    return false;
}
static bool unique_group(bvpv_catalog_store_t *s,uint16_t id,const uint8_t *key,uint16_t n)
{
    const uint64_t mask=UINT64_C(0x0000FFFFFFFFFFFF);
    uint64_t fingerprint=text_hash(key,n)&mask;uint16_t i;
    if(!id || s->text_count>=BVPV_CATALOG_MAX_PARAMS)return false;
    for(i=0;i<s->text_count;i++)if((uint16_t)(s->text_hashes[i]>>48)==id || (s->text_hashes[i]&mask)==fingerprint)return false;
    s->text_hashes[s->text_count++]=((uint64_t)id<<48)|fingerprint;return true;
}
static uint8_t validate_tree(bvpv_catalog_store_t *s)
{
    const uint8_t *r=s->record;
    uint16_t n=s->record_len;
    if(n<6u) return BVPV_ERR_LENGTH;
    if(r[0]==BVPV_TREE_GROUP)
    {
        uint16_t id=r16(r+1), parent=r16(r+3);
        if(!r[5] || n!=6u+r[5] || !ascii(r+6,r[5]) || !group_present(s,parent) || !unique_group(s,id,r+6,r[5])) return BVPV_ERR_STATE;
        ++s->group_count;
        return 0;
    }
    if(r[0]==BVPV_TREE_PARAM && n==6u && group_present(s,r16(r+1)))
    {
        int32_t index=bvpv_catalog_find(s->runtime,address(r+3));
        if(index<0 || !mark_seen(s,(uint16_t)index)) return BVPV_ERR_STATE;
        ++s->tree_params;
        return 0;
    }
    return BVPV_ERR_STATE;
}
static uint8_t validate_ui(bvpv_catalog_store_t *s)
{
    const uint8_t *r=s->record;
    uint16_t len=s->record_len;
    bvpv_param_t p;
    int32_t index;
    if(len<6u || !r[0] || r[0]>63u || len!=5u+r[0] || !ascii(r+1,r[0])) return BVPV_ERR_LENGTH;
    index=bvpv_catalog_find(s->runtime,address(r+1u+r[0]));
    if(index<0 || !bvpv_catalog_get(s->runtime,(uint16_t)index,&p)) return BVPV_ERR_STATE;
    {
        unsigned i;
        for(i=0;i<BVPV_CATALOG_BINDING_COUNT;i++)
            if(strlen(bvpv_catalog_bindings[i].key)==r[0] && !memcmp(bvpv_catalog_bindings[i].key,r+1,r[0]) && bvpv_catalog_bindings[i].addr==p.addr) break;
        if(i==BVPV_CATALOG_BINDING_COUNT || !unique_text(s,r+1,r[0])) return BVPV_ERR_STATE;
    }
    ++s->ui_count;
    return 0;
}
static void validator_reset(bvpv_catalog_store_t *s,bool apply)
{
    s->section=0; s->ordinal=0; s->text_count=0; s->group_count=0; s->tree_params=0; s->ui_count=0;
    s->record_len=s->record_cursor=0; s->cursor=0; s->candidate_range_count=0; s->apply_policy=apply;
    if(apply) s->range_count=0;
    memset(s->seen,0,sizeof(s->seen));
    mbedtls_sha256_starts(&s->body_hash,0);
    mbedtls_sha256_starts(&s->expected_index_hash,0);
    mbedtls_sha256_starts(&s->actual_index_hash,0);
}
static uint8_t validate_error(bvpv_catalog_store_t *s)
{
    const uint8_t *r=s->record;
    if(s->record_len<3u || !r[1] || s->record_len!=2u+r[1] || !ascii(r+2,r[1])) return BVPV_ERR_LENGTH;
    return mark_seen(s,r[0])?0:BVPV_ERR_STATE;
}
static uint8_t advance_section(bvpv_catalog_store_t *s)
{
    while(s->section<5u && s->cursor==section_offset(s->header,s->section)+section_bytes(s->header,s->section))
    {
        if(s->section!=3u && (s->record_cursor || s->ordinal!=section_count(s->header,s->section))) return BVPV_ERR_LENGTH;
        if(s->section==1u && s->tree_params!=s->runtime->count) return BVPV_ERR_STATE;
        ++s->section; s->ordinal=0; s->text_count=0;
        if(s->section==1u || s->section==4u) memset(s->seen,0,sizeof(s->seen));
    }
    return 0;
}
static uint8_t validator_feed(bvpv_catalog_store_t *s,const uint8_t *p,uint16_t n)
{
    uint16_t i;
    if(mbedtls_sha256_update(&s->body_hash,p,n)) return BVPV_ERR_SIGNATURE;
    for(i=0;i<n;i++,s->cursor++)
    {
        uint8_t status=advance_section(s);
        if(status) return status;
        if(s->section==3u)
        {
            if(mbedtls_sha256_update(&s->actual_index_hash,p+i,1)) return BVPV_ERR_SIGNATURE;
            continue;
        }
        if(s->section>=5u) return BVPV_ERR_LENGTH;
        if(s->record_cursor==0u) s->record_start=s->cursor;
        if(s->record_cursor<2u)
        {
            s->record_prefix[s->record_cursor++]=p[i];
            if(s->record_cursor==2u)
            {
                s->record_len=r16(s->record_prefix);
                if(!s->record_len || s->record_len>BVPV_CATALOG_MAX_RECORD ||
                    s->record_len>section_offset(s->header,s->section)+section_bytes(s->header,s->section)-s->cursor-1u) return BVPV_ERR_LENGTH;
            }
            continue;
        }
        s->record[s->record_cursor++-2u]=p[i];
        if(s->record_cursor==s->record_len+2u)
        {
            status=s->section==0u?validate_param(s):s->section==1u?validate_tree(s):s->section==2u?validate_ui(s):validate_error(s);
            if(status) return status;
            if(s->section<2u)
            {
                uint8_t entry[4]={0}; w32(entry,s->record_start+2u);
                if(mbedtls_sha256_update(&s->expected_index_hash,entry,4)) return BVPV_ERR_SIGNATURE;
            }
            ++s->ordinal; s->record_cursor=0;
        }
    }
    return 0;
}
static uint8_t validator_finish(bvpv_catalog_store_t *s)
{
    uint8_t body[32],expected[32],actual[32],status=advance_section(s);
    if(status) return status;
    if(s->section!=5u || s->cursor!=r32(s->header+12)) return BVPV_ERR_LENGTH;
    if(mbedtls_sha256_finish(&s->body_hash,body) || mbedtls_sha256_finish(&s->expected_index_hash,expected) ||
        mbedtls_sha256_finish(&s->actual_index_hash,actual)) return BVPV_ERR_SIGNATURE;
    if(memcmp(body,s->header+BVPV_CATALOG_H_BODY_SHA,32)) return BVPV_ERR_IMAGE_DIGEST;
    return memcmp(expected,actual,32)?BVPV_ERR_LENGTH:0;
}
static uint8_t header_check(bvpv_catalog_store_t *s)
{
    const uint8_t *h=s->header;
    uint32_t off=0,total=r32(h+8),body=r32(h+12);
    uint16_t i;
    bool nul=false;
    if(memcmp(h,"ACAT",4) || r16(h+4)!=1u || r16(h+6)!=512u) return BVPV_ERR_LENGTH;
    if(total<512u || total>BVPV_FLASH_PARAM_BYTES || body!=total-512u) return BVPV_ERR_OUT_OF_RANGE;
    if(memcmp(h+16,target,16) || memcmp(h+32,s->contract_sha,32) || memcmp(h+96,schema_sha,32)) return BVPV_ERR_TARGET;
    if(r16(h+160)!=s->runtime->count || !r16(h+162)) return BVPV_ERR_STATE;
    for(i=128;i<160;i++) { if(!h[i]) nul=true; else if(nul || h[i]<0x20u || h[i]>0x7Eu) return BVPV_ERR_LENGTH; }
    if(!h[128] || !nul) return BVPV_ERR_LENGTH;
    if(r16(h+166)>256u) return BVPV_ERR_LENGTH;
    for(i=208;i<448;i++) if(h[i]) return BVPV_ERR_LENGTH;
    for(i=0;i<5;i++)
    {
        uint32_t len=section_bytes(h,(uint8_t)i);
        if(section_offset(h,(uint8_t)i)!=off || len>body-off) return BVPV_ERR_LENGTH;
        off+=len;
    }
    if(off!=body || section_bytes(h,3)!=4u*((uint32_t)r16(h+160)+r16(h+162))) return BVPV_ERR_LENGTH;
    return 0;
}
static uint8_t signature_start(bvpv_catalog_store_t *s)
{
    mbedtls_sha256_context sha;
    uint8_t digest[32];
    static const uint8_t domain[16]=BVPV_CATALOG_SIGNATURE_DOMAIN;
    uint8_t status=header_check(s);
    if(status) return status;
    mbedtls_sha256_init(&sha);
    if(mbedtls_sha256_starts(&sha,0) || mbedtls_sha256_update(&sha,domain,16) ||
        mbedtls_sha256_update(&sha,s->header,448) || mbedtls_sha256_finish(&sha,digest)) status=BVPV_ERR_SIGNATURE;
    mbedtls_sha256_free(&sha);
    if(!status) status=fw_p256_start(s->key,digest,s->header+448);
    s->signing=status==0;
    return status;
}
static void meta_encode(bvpv_catalog_store_t *s,uint8_t kind)
{
    memset(s->meta,0xFF,96); memcpy(s->meta,"ACC1",4);
    s->meta[4]=kind; memset(s->meta+5,0,3); w32(s->meta+8,s->generation+1u);
    w32(s->meta+12,kind?0:r32(s->header+8));
    if(kind) memset(s->meta+16,0,32); else memcpy(s->meta+16,s->header+64,32);
    memcpy(s->meta+48,s->contract_sha,32);
    w32(s->meta+80,bvpv_cd_crc32(s->meta,80));
    s->meta_kind=kind; s->meta_target=(uint8_t)(1u-s->meta_index); s->phase=P_META_ERASE;
}
static bool meta_valid(const uint8_t *p)
{
    unsigned i;
    if(memcmp(p,"ACC1",4) || p[4]>1u || p[5] || p[6] || p[7] || p[92]!=0xA5u ||
        r32(p+80)!=bvpv_cd_crc32(p,80)) return false;
    for(i=84;i<92;i++) if(p[i]!=0xFFu) return false;
    for(i=93;i<96;i++) if(p[i]!=0xFFu) return false;
    if(p[4]) { if(r32(p+12)) return false; for(i=16;i<48;i++) if(p[i]) return false; }
    else if(r32(p+12)<512u || r32(p+12)>BVPV_FLASH_PARAM_BYTES) return false;
    return true;
}
void bvpv_catalog_store_init(bvpv_catalog_store_t *s,const bvpv_cd_io_t *io,
    bvpv_service_ctx_t *runtime,const uint8_t key[64],const char *app)
{
    mbedtls_sha256_context sha;
    uint16_t i;
    uint8_t data[8];
    memset(s,0,sizeof(*s)); if(io) s->io=*io; s->runtime=runtime; s->app_identity=app;
    if(key) memcpy(s->key,key,64);
    mbedtls_sha256_init(&s->body_hash); mbedtls_sha256_init(&s->expected_index_hash); mbedtls_sha256_init(&s->actual_index_hash);
    if(!runtime || !runtime->count || runtime->count>BVPV_CATALOG_MAX_PARAMS) { s->fault=true; s->last_error=BVPV_ERR_STATE; return; }
    mbedtls_sha256_init(&sha); mbedtls_sha256_starts(&sha,0);
    mbedtls_sha256_update(&sha,(const uint8_t *)"BVPV-RUNTIME-1",14);
    w16(data,runtime->count); mbedtls_sha256_update(&sha,data,2);
    for(i=0;i<runtime->count;i++)
    {
        bvpv_param_t p;
        if(!bvpv_catalog_get(runtime,i,&p)) { s->fault=true; s->last_error=BVPV_ERR_STATE; break; }
        data[0]=(uint8_t)(p.addr>>16); data[1]=(uint8_t)(p.addr>>8); data[2]=(uint8_t)p.addr; data[3]=p.type;
        w16(data+4,p.elem_count); w16(data+6,bvpv_param_elem_size(&p)); mbedtls_sha256_update(&sha,data,8);
    }
    mbedtls_sha256_finish(&sha,s->contract_sha); mbedtls_sha256_free(&sha);
}
void bvpv_catalog_store_boot(bvpv_catalog_store_t *s)
{
    if(s->fault) return;
    s->available=false; s->booting=true; s->phase=P_BOOT_META0;
}
uint8_t bvpv_catalog_store_policy(void *user,uint16_t index,const bvpv_param_t *param,bvpv_effective_attr_t *a)
{
    bvpv_catalog_store_t *s=(bvpv_catalog_store_t *)user;
    uint16_t i;
    (void)param;
    if(!s->available || index>=s->runtime->count) return BVPV_ERR_STATE;
    {
        uint8_t policy=s->policy_access[index];
        bool read=bvpv_access_readable(policy) && bvpv_access_readable(a->access);
        bool write=bvpv_access_writable(policy) && bvpv_access_writable(a->access);
        bool persist=bvpv_access_eeprom_persistent(policy) && bvpv_access_eeprom_persistent(a->access);
        if(policy==BVPV_ACC_OWO && a->access==BVPV_ACC_OWO) a->access=BVPV_ACC_OWO;
        else a->access=read?(write?(persist?BVPV_ACC_RWA:BVPV_ACC_RWO):BVPV_ACC_ROO):BVPV_ACC_OOO;
    } a->flags&=s->policy_flags[index];
    for(i=0;i<s->range_count;i++) if(s->ranges[i].index==index)
    {
        const bvpv_catalog_range_t *r=&s->ranges[i];
        if(r->flags&1u) { if(!(a->ext_flags&1u) || bvpv_value_compare(param->type,r->min,a->min)>0) a->min=r->min; a->ext_flags|=1u; }
        if(r->flags&2u) { if(!(a->ext_flags&2u) || bvpv_value_compare(param->type,r->max,a->max)<0) a->max=r->max; a->ext_flags|=2u; }
        break;
    }
    return 0;
}
uint8_t bvpv_catalog_store_record(bvpv_catalog_store_t *s,uint8_t section,uint16_t index,
    uint8_t *out,uint16_t *len,uint16_t cap)
{
    uint8_t entry[4],prefix[2]; uint32_t off; uint16_t n; bvpv_cd_read_result_t result;
    if(!bvpv_catalog_store_ready(s)) return BVPV_ERR_STATE;
    if(section>1u || index>=section_count(s->active_header,section)) return BVPV_ERR_INDEX_OOB;
    off=section_offset(s->active_header,3)+4u*(index+(section==1u?section_count(s->active_header,0):0u));
    result=s->io.read(s->io.ctx,BVPV_FLASH_PARAM_BASE+512u+off,entry,4);
    if(result!=BVPV_CD_READ_OK) return result==BVPV_CD_READ_BUSY?BVPV_ERR_BUSY:BVPV_ERR_FLASH;
    off=r32(entry);
    if(off<section_offset(s->active_header,section)+2u ||
        off>section_offset(s->active_header,section)+section_bytes(s->active_header,section) ||
        off>r32(s->active_header+12)) return BVPV_ERR_LENGTH;
    result=s->io.read(s->io.ctx,BVPV_FLASH_PARAM_BASE+512u+off-2u,prefix,2);
    if(result!=BVPV_CD_READ_OK) return result==BVPV_CD_READ_BUSY?BVPV_ERR_BUSY:BVPV_ERR_FLASH;
    n=r16(prefix);
    if(!n || n>cap || n>section_offset(s->active_header,section)+section_bytes(s->active_header,section)-off) return BVPV_ERR_LENGTH;
    result=s->io.read(s->io.ctx,BVPV_FLASH_PARAM_BASE+512u+off,out,n);
    if(result!=BVPV_CD_READ_OK) return result==BVPV_CD_READ_BUSY?BVPV_ERR_BUSY:BVPV_ERR_FLASH;
    *len=n; return 0;
}
static uint8_t effective_record(bvpv_catalog_store_t *s,uint16_t index,uint8_t *out,uint16_t *len,uint16_t cap)
{
    uint8_t status=bvpv_catalog_store_record(s,0,index,out,len,cap);
    bvpv_effective_attr_t a; bvpv_param_t p; int32_t runtime_index;
    uint16_t pos,base,esz,n=*len;
    if(status) return status;
    runtime_index=bvpv_catalog_find(s->runtime,address(out));
    if(runtime_index<0 || !bvpv_catalog_get(s->runtime,(uint16_t)runtime_index,&p)) return BVPV_ERR_STATE;
    status=bvpv_catalog_effective_attr(s->runtime,(uint16_t)runtime_index,&a);
    if(status) return status;
    out[4]=a.access; out[5]=a.flags; out[6]=a.state; base=(uint16_t)(12u+out[9]); pos=base; esz=bvpv_param_elem_size(&p);
    while(pos<n)
    {
        uint8_t tag=out[pos]; uint16_t bytes=r16(out+pos+1);
        if(tag==1u || tag==2u)
        {
            uint8_t flag=tag==1u?1u:2u;
            if(!(a.ext_flags&flag)) { memmove(out+pos,out+pos+3u+bytes,n-pos-3u-bytes); n=(uint16_t)(n-3u-bytes); continue; }
            if(bytes!=esz || bytes>8u) return BVPV_ERR_TYPE_MISMATCH;
            memcpy(out+pos+3u,tag==1u?a.min:a.max,bytes);
        }
        pos=(uint16_t)(pos+3u+bytes);
    }
    w16(out+base-2u,(uint16_t)(n-base)); *len=n; return 0;
}
static uint8_t static_page(bvpv_catalog_store_t *s,uint8_t svc,const uint8_t *p,uint16_t n,
    uint8_t *out,uint16_t *out_len,uint16_t cap)
{
    uint16_t start,max,index,used=0,count,head,next;
    uint8_t status;
    if((svc==0x12u && n!=8u) || (svc!=0x12u && n!=4u) || !p) return BVPV_ERR_LENGTH;
    start=r16(p); max=r16(p+2); head=svc==0x12u?6u:2u;
    if(cap<head) return BVPV_ERR_LENGTH;
    if(svc==0x12u && r32(p+4) && r32(p+4)!=s->runtime->revision) return BVPV_ERR_STATE;
    count=section_count(s->active_header,svc==0x02u?1u:0u); index=start;
    if(start>count) return BVPV_ERR_INDEX_OOB;
    while(index<count)
    {
        uint16_t bytes=0;
        status=svc==0x02u?bvpv_catalog_store_record(s,1,index,out+head+used,&bytes,(uint16_t)(cap-head-used)):
            effective_record(s,index,out+head+used,&bytes,(uint16_t)(cap-head-used));
        if(status==BVPV_ERR_LENGTH && index!=start) break;
        if(status) return status;
        if(bytes>max-used) { if(index==start) return BVPV_ERR_LENGTH; break; }
        used=(uint16_t)(used+bytes); ++index;
    }
    next=index==count?0xFFFFu:index;
    if(svc==0x12u) { w32(out,s->runtime->revision); w16(out+4,next); }
    else w16(out,next);
    *out_len=(uint16_t)(head+used); return 0;
}
static uint8_t sequential_page(bvpv_catalog_store_t *s,uint8_t section,const uint8_t *p,uint16_t n,uint8_t *out,uint16_t *out_len,uint16_t cap)
{
    uint32_t pos=section_offset(s->active_header,section); uint16_t start,max,index=0,used=0,count=section_count(s->active_header,section);
    if(!p || n!=4u || cap<2u) return BVPV_ERR_LENGTH;
    start=r16(p); max=r16(p+2); if(start>count) return BVPV_ERR_INDEX_OOB;
    while(index<count)
    {
        uint8_t prefix[2]; uint16_t bytes; bvpv_cd_read_result_t r;
        r=s->io.read(s->io.ctx,BVPV_FLASH_PARAM_BASE+512u+pos,prefix,2);
        if(r!=BVPV_CD_READ_OK) return r==BVPV_CD_READ_BUSY?BVPV_ERR_BUSY:BVPV_ERR_FLASH;
        bytes=r16(prefix);
        if(!bytes || bytes>section_offset(s->active_header,section)+section_bytes(s->active_header,section)-pos-2u) return BVPV_ERR_LENGTH;
        if(index>=start)
        {
            if(bytes>max-used || bytes>cap-2u-used) { if(index==start) return BVPV_ERR_LENGTH; break; }
            r=s->io.read(s->io.ctx,BVPV_FLASH_PARAM_BASE+512u+pos+2u,out+2u+used,bytes);
            if(r!=BVPV_CD_READ_OK) return r==BVPV_CD_READ_BUSY?BVPV_ERR_BUSY:BVPV_ERR_FLASH;
            used=(uint16_t)(used+bytes);
        }
        pos+=2u+bytes; ++index;
    }
    w16(out,index==count?0xFFFFu:index); *out_len=(uint16_t)(2u+used); return 0;
}
bool bvpv_catalog_store_take_reply(bvpv_catalog_store_t *s,uint8_t *out,uint16_t *len,uint16_t cap)
{
    if(!s->reply_ready) return false;
    if(cap<s->reply_len) { if(cap) out[0]=BVPV_ERR_LENGTH; *len=cap?1u:0u; }
    else { memcpy(out,s->reply,s->reply_len); *len=s->reply_len; }
    s->reply_ready=false; return true;
}
void bvpv_catalog_store_cancel(bvpv_catalog_store_t *s)
{
    if(s->session) s->cancelled=true;
}
static void info(bvpv_catalog_store_t *s,uint8_t *out,uint16_t *n,uint16_t cap)
{
    uint8_t *p=out+1; size_t app=s->app_identity?strlen(s->app_identity):0;
    if(app>63u) app=63u;
    if(cap<134u+app) { out[0]=BVPV_ERR_LENGTH; *n=1; return; }
    memset(out,0,134u+app);
    p[0]=1; p[1]=s->fault?5u:s->preflight_ready?2u:s->session?(s->preflight?1u:s->phase==P_VERIFY_BODY||s->phase==P_SIGNATURE?4u:3u):s->booting?4u:0u;
    p[2]=s->available?1u:0u; p[3]=s->last_error;
    w32(p+4,BVPV_FLASH_PARAM_BYTES); w32(p+8,s->available?r32(s->active_header+8):0);
    w32(p+12,s->generation); w16(p+16,s->runtime?s->runtime->count:0);
    w16(p+18,s->available?r16(s->active_header+162):0); w16(p+20,s->available?r16(s->active_header+164):0);
    w32(p+24,s->runtime?s->runtime->revision:0); w32(p+28,s->session?s->session_id:0); w32(p+32,s->next_offset);
    if(s->available) memcpy(p+36,s->active_header+64,32);
    memcpy(p+68,s->contract_sha,32); memcpy(p+100,schema_sha,32); p[132]=(uint8_t)app;
    if(app) memcpy(p+133,s->app_identity,app);
    *n=(uint16_t)(134u+app);
}
bool bvpv_catalog_store_request(bvpv_catalog_store_t *s,uint8_t svc,const uint8_t *p,uint16_t n,
    uint8_t *out,uint16_t *out_len,uint16_t cap,uint32_t now)
{
    uint8_t status=0;
    /* 新事务不沿用断线后未取走的上一条应答。 */
    s->reply_ready=false;
    if(!out || !out_len || !cap) return true;
    *out_len=1; out[0]=0;
    if(n && !p) { out[0]=BVPV_ERR_LENGTH; return true; }
    if(svc==0x30u)
    {
        if(!n) { info(s,out,out_len,cap); return true; }
        if(n!=6u) status=BVPV_ERR_LENGTH;
        else if(!bvpv_catalog_store_ready(s)) status=BVPV_ERR_STATE;
        else
        {
            uint32_t off=r32(p),bytes=r16(p+4),total=r32(s->active_header+8); bvpv_cd_read_result_t result;
            if(!bytes || bytes>3500u || off>=total || bytes>total-off || cap<37u+bytes) status=BVPV_ERR_LENGTH;
            else
            {
                result=s->io.read(s->io.ctx,BVPV_FLASH_PARAM_BASE+off,out+37,bytes);
                if(result!=BVPV_CD_READ_OK) status=result==BVPV_CD_READ_BUSY?BVPV_ERR_BUSY:BVPV_ERR_FLASH;
                else { memcpy(out+1,s->active_header+64,32); w32(out+33,off); *out_len=(uint16_t)(37u+bytes); }
            }
        }
        out[0]=status; return true;
    }
    if(svc==0x33u)
    {
        if(s->fault) status=BVPV_ERR_FLASH;
        else if(n!=520u || p[0]>1u || p[1] || p[2] || p[3]) status=BVPV_ERR_LENGTH;
        else if(s->booting || (s->session && !s->preflight_ready)) status=BVPV_ERR_BUSY;
        else if(p[0]==0u)
        {
            if(r32(p+4)) status=BVPV_ERR_STATE;
            else if(!s->io.reserve || !s->io.reserve(s->io.ctx,true)) status=BVPV_ERR_BUSY;
            else
            {
                memcpy(s->header,p+8,512); s->session=true; s->preflight=true; s->preflight_ready=false;
                s->cancelled=false; s->committed=false; s->last_error=0; s->next_offset=0; s->last_activity=now;
                if(++s->session_id==0) ++s->session_id;
                status=signature_start(s);
                if(!status) { s->phase=P_SIGNATURE; s->signature_stage=0; return false; }
                release_session(s);
            }
        }
        else if(!s->preflight_ready || r32(p+4)!=s->session_id || memcmp(p+8,s->header,512)) status=BVPV_ERR_STATE;
        else
        {
            s->preflight=false; s->preflight_ready=false; s->next_offset=0; s->last_activity=now;
            meta_encode(s,1); return false;
        }
        out[0]=status; s->last_error=status; return true;
    }
    if(svc==0x34u || svc==0x35u || svc==0x36u)
    {
        if(n<4u || !p || r32(p)!=s->session_id || !s->session) status=BVPV_ERR_STATE;
        else if(svc==0x36u)
        {
            if(n!=4u) status=BVPV_ERR_LENGTH;
            else { s->cancelled=true; if(s->phase==P_IDLE) release_session(s); }
        }
        else if(s->phase!=P_IDLE || s->preflight_ready) status=BVPV_ERR_BUSY;
        else if(svc==0x34u)
        {
            uint32_t bytes=n>=8u?n-8u:0;
            if(n<9u || bytes>3500u || r32(p+4)!=s->next_offset || s->next_offset>=r32(s->header+12) || bytes>r32(s->header+12)-s->next_offset) status=BVPV_ERR_LENGTH;
            else { memcpy(s->block,p+8,bytes); s->block_start=s->next_offset; s->block_len=(uint16_t)bytes; s->block_cursor=0;
                s->last_activity=now; s->phase=s->preflight?P_PREFLIGHT_BLOCK:P_WRITE_BLOCK; return false; }
        }
        else if(n!=4u || s->next_offset!=r32(s->header+12)) status=BVPV_ERR_STATE;
        else if(s->preflight)
        {
            status=validator_finish(s);
            if(!status) { s->preflight_ready=true; s->last_activity=now; }
            else release_session(s);
        }
        else { s->cursor=0; s->phase=P_VERIFY_HEADER0; return false; }
        out[0]=status; if(status) s->last_error=status; return true;
    }
    if(!bvpv_catalog_store_ready(s)) { out[0]=BVPV_ERR_STATE; return true; }
    if(svc==0x02u || svc==0x12u) status=static_page(s,svc,p,n,out+1,out_len,(uint16_t)(cap-1u));
    else if(svc==0x32u || svc==0x09u) status=sequential_page(s,svc==0x32u?2u:4u,p,n,out+1,out_len,(uint16_t)(cap-1u));
    else if(svc==0x04u)
    {
        uint16_t i,bytes=0;
        if(n!=4u) status=BVPV_ERR_LENGTH;
        else
        {
            status=BVPV_ERR_NOT_FOUND;
            {
                int32_t runtime_index=bvpv_catalog_find(s->runtime,address(p));
                if(runtime_index>=0)
                {
                    i=s->param_ordinal[runtime_index];
                    status=effective_record(s,i,s->record,&bytes,sizeof(s->record));
                    if(!status)
                    {
                        uint16_t base=(uint16_t)(12u+s->record[9]); bytes=(uint16_t)(bytes-base);
                        if(bytes>cap-1u) status=BVPV_ERR_LENGTH;
                        else { memcpy(out+1,s->record+base,bytes); *out_len=bytes; }
                    }
                }
            }
        }
    }
    else status=BVPV_ERR_NOT_SUPPORTED;
    if(!status) ++*out_len; else *out_len=1;
    out[0]=status;
    if(status==BVPV_ERR_FLASH) { s->available=false; s->last_error=status; }
    return true;
}
static bool waiting(uint8_t phase)
{
    return phase==P_ERASE_WAIT || phase==P_PROGRAM_WAIT || phase==P_META_WAIT ||
        phase==P_META_PROGRAM_WAIT || phase==P_META_MARK_WAIT;
}
static bvpv_cd_read_result_t read_chunk(bvpv_catalog_store_t *s,uint32_t a,uint8_t *p,uint16_t n)
{
    if(!s->io.read || n>256u) return BVPV_CD_READ_ERROR;
    return s->io.read(s->io.ctx,a,p,n);
}
static void write_step(bvpv_catalog_store_t *s)
{
    const uint8_t *data=s->header_programming?s->header:s->block;
    uint16_t total=s->header_programming?512u:s->block_len;
    uint32_t a=BVPV_FLASH_PARAM_BASE+(s->header_programming?0u:512u+s->block_start)+s->block_cursor;
    uint16_t bytes=(uint16_t)(total-s->block_cursor),left=(uint16_t)(256u-a%256u);
    if(bytes>left) bytes=left;
    if(a%4096u==0u)
    {
        if(!s->io.erase_start(s->io.ctx,a)) failed(s,BVPV_ERR_FLASH);
        else s->phase=P_ERASE_WAIT;
        return;
    }
    s->io_count=bytes;
    if(!s->io.program_start(s->io.ctx,a,data+s->block_cursor,bytes)) failed(s,BVPV_ERR_FLASH);
    else s->phase=P_PROGRAM_WAIT;
}
void bvpv_catalog_store_task(bvpv_catalog_store_t *s,uint32_t now)
{
    uint8_t error=0;
    bvpv_cd_read_result_t result;
    uint32_t a;
    if(s->session && (uint32_t)(now-s->last_activity)>=30000u && s->phase==P_IDLE) s->cancelled=true;
    if(waiting(s->phase))
    {
        int p=s->io.poll?s->io.poll(s->io.ctx):-1;
        if(p>0) return;
        if(p<0) { failed(s,BVPV_ERR_FLASH); return; }
    }
    /* 标志已发出就必须回读；断线不能把有效提交重新变成失败。 */
    if(s->phase==P_META_MARK_WAIT)
    {
        result=read_chunk(s,BVPV_CATALOG_META_BASE(s->meta_target),s->scratch,96);
        if(result==BVPV_CD_READ_BUSY) return;
        if(result!=BVPV_CD_READ_OK || !meta_valid(s->scratch) || memcmp(s->scratch,s->meta,96)) { failed(s,BVPV_ERR_FLASH); return; }
        s->meta_index=s->meta_target; s->generation=r32(s->meta+8);
        if(s->meta_kind)
        {
            s->available=false;
            if(s->cancelled) { release_session(s); answer(s,BVPV_ERR_STATE,1); return; }
            s->header_programming=true; s->block_cursor=0; s->phase=P_WRITE_HEADER;
        }
        else
        {
            memcpy(s->active_header,s->header,512); s->available=true; s->committed=true; ++s->runtime->revision;
            if(!s->runtime->revision) ++s->runtime->revision;
            release_session(s); answer(s,0,1);
        }
        return;
    }
    if(s->cancelled && s->session) { release_session(s); answer(s,BVPV_ERR_STATE,1); return; }
    switch(s->phase)
    {
    case P_IDLE: return;
    case P_BOOT_META0:
    case P_BOOT_META1:
    {
        uint8_t i=s->phase==P_BOOT_META0?0u:1u;
        result=read_chunk(s,BVPV_CATALOG_META_BASE(i),s->boot_meta[i],96);
        if(result==BVPV_CD_READ_BUSY) return;
        if(result!=BVPV_CD_READ_OK) { failed(s,BVPV_ERR_FLASH); return; }
        s->phase=i?P_BOOT_SELECT:P_BOOT_META1;
        return;
    }
    case P_BOOT_SELECT:
    {
        bool v0=meta_valid(s->boot_meta[0]),v1=meta_valid(s->boot_meta[1]);
        uint8_t *m;
        if(!v0 && !v1)
        {
            unsigned i;const uint8_t *raw=(const uint8_t *)s->boot_meta;
            for(i=0;i<sizeof(s->boot_meta);i++)if(raw[i]!=0xFFu)break;
            if(i<sizeof(s->boot_meta))s->last_error=BVPV_ERR_CRC;
            s->booting=false;s->phase=P_IDLE;return;
        }
        s->meta_index=v1 && (!v0 || bvpv_cd_sequence_newer(r32(s->boot_meta[1]+8),r32(s->boot_meta[0]+8)))?1u:0u;
        m=s->boot_meta[s->meta_index]; s->generation=r32(m+8);
        if(v0 && v1 && !bvpv_cd_sequence_newer(r32(s->boot_meta[0]+8),r32(s->boot_meta[1]+8)) &&
           !bvpv_cd_sequence_newer(r32(s->boot_meta[1]+8),r32(s->boot_meta[0]+8)) && memcmp(s->boot_meta[0],s->boot_meta[1],96)) { failed(s,BVPV_ERR_CRC); return; }
        if(m[4]) { s->booting=false; s->phase=P_IDLE; return; }
        if(memcmp(m+48,s->contract_sha,32)) { failed(s,BVPV_ERR_TARGET); return; }
        s->phase=P_HEADER0; return;
    }
    case P_HEADER0:
    case P_HEADER1:
    {
        uint16_t off=s->phase==P_HEADER0?0u:256u;
        result=read_chunk(s,BVPV_FLASH_PARAM_BASE+off,s->header+off,256);
        if(result==BVPV_CD_READ_BUSY) return;
        if(result!=BVPV_CD_READ_OK) { failed(s,BVPV_ERR_FLASH); return; }
        if(!off) { s->phase=P_HEADER1; return; }
        if(r32(s->header+8)!=r32(s->boot_meta[s->meta_index]+12) || memcmp(s->header+64,s->boot_meta[s->meta_index]+16,32)) { failed(s,BVPV_ERR_IMAGE_DIGEST); return; }
        error=signature_start(s);
        if(error) { failed(s,error); return; }
        s->signature_stage=1; s->phase=P_SIGNATURE; return;
    }
    case P_SIGNATURE:
    {
        int verify=fw_p256_poll();
        if(verify>0) return;
        s->signing=false;
        if(verify<0) { failed(s,BVPV_ERR_SIGNATURE); return; }
        validator_reset(s,s->signature_stage!=0u);
        if(s->signature_stage==1u) s->phase=P_BOOT_BODY;
        else if(s->signature_stage==2u) s->phase=P_VERIFY_BODY;
        else { s->phase=P_IDLE; w32(s->reply+1,s->session_id); answer(s,0,5); }
        return;
    }
    case P_BOOT_BODY:
    case P_VERIFY_BODY:
    {
        uint16_t bytes=(uint16_t)((r32(s->header+12)-s->cursor)>256u?256u:r32(s->header+12)-s->cursor);
        if(bytes)
        {
            result=read_chunk(s,BVPV_FLASH_PARAM_BASE+512u+s->cursor,s->scratch,bytes);
            if(result==BVPV_CD_READ_BUSY) return;
            if(result!=BVPV_CD_READ_OK) { failed(s,BVPV_ERR_FLASH); return; }
            error=validator_feed(s,s->scratch,bytes);
            if(error) { failed(s,error); return; }
            if(s->cursor<r32(s->header+12)) return;
        }
        error=validator_finish(s);
        if(error) { failed(s,error); return; }
        if(s->booting) { memcpy(s->active_header,s->header,512); s->booting=false; s->available=true; s->phase=P_IDLE; }
        else meta_encode(s,0);
        return;
    }
    case P_PREFLIGHT_BLOCK:
    {
        uint16_t bytes=(uint16_t)(s->block_len-s->block_cursor);
        if(bytes>256u) bytes=256u;
        error=validator_feed(s,s->block+s->block_cursor,bytes);
        if(error) { failed(s,error); return; }
        s->block_cursor=(uint16_t)(s->block_cursor+bytes);
        if(s->block_cursor==s->block_len) { s->next_offset+=s->block_len; s->phase=P_IDLE; s->last_activity=now; w32(s->reply+1,s->next_offset); answer(s,0,5); }
        return;
    }
    case P_WRITE_HEADER:
        s->header_programming=true; write_step(s); return;
    case P_WRITE_BLOCK:
        s->header_programming=false; write_step(s); return;
    case P_ERASE_WAIT:
    {
        const uint8_t *data=s->header_programming?s->header:s->block;
        uint16_t total=s->header_programming?512u:s->block_len;
        a=BVPV_FLASH_PARAM_BASE+(s->header_programming?0u:512u+s->block_start)+s->block_cursor;
        s->io_count=(uint16_t)(total-s->block_cursor); if(s->io_count>256u) s->io_count=256u;
        if(!s->io.program_start(s->io.ctx,a,data+s->block_cursor,s->io_count)) failed(s,BVPV_ERR_FLASH);
        else s->phase=P_PROGRAM_WAIT;
        return;
    }
    case P_PROGRAM_WAIT: s->phase=P_PROGRAM_CHECK; return;
    case P_PROGRAM_CHECK:
    {
        const uint8_t *data=s->header_programming?s->header:s->block;
        uint16_t total=s->header_programming?512u:s->block_len;
        a=BVPV_FLASH_PARAM_BASE+(s->header_programming?0u:512u+s->block_start)+s->block_cursor;
        result=read_chunk(s,a,s->scratch,s->io_count);
        if(result==BVPV_CD_READ_BUSY) return;
        if(result!=BVPV_CD_READ_OK || memcmp(s->scratch,data+s->block_cursor,s->io_count)) { failed(s,BVPV_ERR_FLASH); return; }
        s->block_cursor=(uint16_t)(s->block_cursor+s->io_count);
        if(s->block_cursor==total)
        {
            s->phase=P_IDLE; s->last_activity=now;
            if(s->header_programming) w32(s->reply+1,s->session_id);
            else { s->next_offset=s->block_start+s->block_len; w32(s->reply+1,s->next_offset); }
            answer(s,0,5);
        }
        else s->phase=s->header_programming?P_WRITE_HEADER:P_WRITE_BLOCK;
        return;
    }
    case P_VERIFY_HEADER0:
    case P_VERIFY_HEADER1:
    {
        uint16_t off=s->phase==P_VERIFY_HEADER0?0u:256u;
        result=read_chunk(s,BVPV_FLASH_PARAM_BASE+off,s->scratch,256);
        if(result==BVPV_CD_READ_BUSY) return;
        if(result!=BVPV_CD_READ_OK || memcmp(s->scratch,s->header+off,256)) { failed(s,BVPV_ERR_FLASH); return; }
        if(!off) { s->phase=P_VERIFY_HEADER1; return; }
        error=signature_start(s);
        if(error) { failed(s,error); return; }
        s->signature_stage=2; s->phase=P_SIGNATURE; return;
    }
    case P_META_ERASE:
        if(!s->io.erase_start(s->io.ctx,BVPV_CATALOG_META_BASE(s->meta_target))) failed(s,BVPV_ERR_FLASH);
        else s->phase=P_META_WAIT;
        return;
    case P_META_WAIT: s->phase=P_META_PROGRAM; return;
    case P_META_PROGRAM:
        if(!s->io.program_start(s->io.ctx,BVPV_CATALOG_META_BASE(s->meta_target),s->meta,92)) failed(s,BVPV_ERR_FLASH);
        else s->phase=P_META_PROGRAM_WAIT;
        return;
    case P_META_PROGRAM_WAIT: s->phase=P_META_CHECK; return;
    case P_META_CHECK:
        result=read_chunk(s,BVPV_CATALOG_META_BASE(s->meta_target),s->scratch,96);
        if(result==BVPV_CD_READ_BUSY) return;
        if(result!=BVPV_CD_READ_OK || memcmp(s->scratch,s->meta,96)) { failed(s,BVPV_ERR_FLASH); return; }
        s->meta[92]=0xA5u;
        if(!s->io.program_start(s->io.ctx,BVPV_CATALOG_META_BASE(s->meta_target)+92u,s->meta+92,1)) failed(s,BVPV_ERR_FLASH);
        else s->phase=P_META_MARK_WAIT;
        return;
    default: failed(s,BVPV_ERR_STATE); return;
    }
}
