#include "bvpv_catalog_store.h"
#include "bvpv_errcode.h"
#include "fw_crypto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
#ifndef CATALOG_FIXTURE_DIR
#define CATALOG_FIXTURE_DIR "tests/fixtures/catalog"
#endif
static uint8_t flash[0x800000], baseline[0x800000], valid[8192], candidate[8192], key[64];
static uint8_t response[4096], request[3510], states[2], value0[2]={42,0}, value1[1]={7};
static bvpv_catalog_store_t store;
static const bvpv_runtime_param_t runtime[]={
    {0x010101u,value0,{0},{100},1,2,BVPV_TYPE_UINT16,BVPV_ACC_RWA,1,3},
    {0x010102u,value1,{0},{0},1,1,BVPV_TYPE_UINT8,BVPV_ACC_ROO,1,0}
};
static bvpv_service_ctx_t ctx;
static uint32_t ticks, task_bytes;
static unsigned operations, cut_at, cut_bytes, busy, reads, write_operations;
static uint8_t catalog_owner_token,cd_owner_token;
static void *owner;
static bool dead, reserved, deny_reserve, fail_read, in_task;
static size_t valid_len;
static uint16_t response_len;
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static void put16(uint8_t *p,uint16_t n) { p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8); }
static void put32(uint8_t *p,uint32_t n) { p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);p[2]=(uint8_t)(n>>16);p[3]=(uint8_t)(n>>24); }
static bool allowed(void *user,uint32_t a,uint32_t n)
{
    if(user==&cd_owner_token) return (a<BVPV_CD_IMAGE_AREA_BYTES && n<=BVPV_CD_IMAGE_AREA_BYTES-a) ||
        (a>=BVPV_CD_META_BASE && a<BVPV_CD_META_BASE+8192u && n<=BVPV_CD_META_BASE+8192u-a);
    return (a>=BVPV_FLASH_PARAM_BASE && a<BVPV_FLASH_PARAM_BASE+BVPV_FLASH_PARAM_BYTES && n<=BVPV_FLASH_PARAM_BASE+BVPV_FLASH_PARAM_BYTES-a) ||
        (a>=BVPV_CATALOG_META_BASE(0) && a<BVPV_CATALOG_META_BASE(1)+4096u && n<=BVPV_CATALOG_META_BASE(1)+4096u-a);
}
static bvpv_cd_read_result_t read_io(void *user,uint32_t a,uint8_t *p,uint32_t n)
{
    (void)user; CHECK(!busy && allowed(user,a,n)); ++reads;
    if(in_task) { task_bytes+=n; CHECK(n<=256u && task_bytes<=256u); }
    if(dead || fail_read) return BVPV_CD_READ_ERROR;
    memcpy(p,flash+a,n); return BVPV_CD_READ_OK;
}
static bool erase_io(void *user,uint32_t a)
{
    unsigned bytes=4096;
    (void)user; CHECK(reserved && !busy && a%4096u==0u && allowed(user,a,4096)); ++operations;
    if(dead) return false;
    if(operations==cut_at) { bytes=cut_bytes<bytes?cut_bytes:bytes;dead=true; }
    memset(flash+a,255,bytes);busy=dead?0:2;return !dead;
}
static bool program_io(void *user,uint32_t a,const uint8_t *p,uint16_t n)
{
    unsigned i;
    (void)user; CHECK(reserved && !busy && n && n<=256u && a%256u+n<=256u && allowed(user,a,n)); ++operations;
    if(dead) return false;
    if(operations==cut_at) { if(cut_bytes<n)n=(uint16_t)cut_bytes;dead=true; }
    for(i=0;i<n;i++) { flash[a+i]&=p[i]; }
    busy=dead?0:2;return !dead;
}
static int poll_io(void *user) { (void)user;if(busy){--busy;return 1;}return dead?-1:0; }
static bool reserve_io(void *user,bool lock)
{
    if(lock) { if(deny_reserve || (owner && owner!=user)) return false;owner=user; }
    else { CHECK(!owner || owner==user);owner=NULL; }
    reserved=owner!=NULL;return true;
}
static const bvpv_cd_io_t io={&catalog_owner_token,read_io,erase_io,program_io,poll_io,reserve_io};
static void task(void) { in_task=true;task_bytes=0;bvpv_catalog_store_task(&store,++ticks);in_task=false; }
static void reset(void)
{
    fw_p256_cancel();ticks=0;operations=0;cut_at=0;busy=0;reads=0;dead=false;reserved=false;owner=NULL;deny_reserve=false;fail_read=false;in_task=false;
    memset(&ctx,0,sizeof(ctx));ctx.runtime=runtime;ctx.count=2;ctx.states=states;ctx.revision=1;
    states[0]=BVPV_STATE_VISIBLE|BVPV_STATE_PENDING;states[1]=BVPV_STATE_VISIBLE;
    bvpv_catalog_store_init(&store,&io,&ctx,key,"TEST.APP.1");ctx.policy_provider=bvpv_catalog_store_policy;ctx.policy_provider_user=&store;
}
static void boot(void)
{
    unsigned budget=100000;
    reset();bvpv_catalog_store_boot(&store);CHECK(store.booting && reads==0 && operations==0);
    while(store.booting && budget--) { task(); }
    CHECK(budget && !reserved && operations==0);
}
static size_t fixture(const char *name,uint8_t *p,size_t capacity)
{
    char path[512];size_t n;FILE *f;
    CHECK(snprintf(path,sizeof(path),"%s/%s",CATALOG_FIXTURE_DIR,name)>0);f=fopen(path,"rb");CHECK(f);
    n=fread(p,1,capacity,f);CHECK(n>0 && !ferror(f) && fgetc(f)==EOF);fclose(f);return n;
}
static uint8_t send(uint8_t svc,const uint8_t *p,uint16_t n)
{
    unsigned budget=100000;
    if(!bvpv_catalog_store_request(&store,svc,p,n,response,&response_len,sizeof(response),ticks))
    {
        while(!bvpv_catalog_store_take_reply(&store,response,&response_len,sizeof(response)) && budget--)task();
        CHECK(budget);
    }
    CHECK(response_len);return response[0];
}
static uint8_t begin(const uint8_t *package,uint8_t phase,uint32_t session)
{
    uint8_t b[520]={0};b[0]=phase;put32(b+4,session);memcpy(b+8,package,512);return send(0x33,b,sizeof(b));
}
static uint8_t block(const uint8_t *package,uint32_t session,uint32_t off,uint16_t count)
{
    put32(request,session);put32(request+4,off);memcpy(request+8,package+512u+off,count);return send(0x34,request,(uint16_t)(8u+count));
}
static uint8_t finish(uint32_t session) { put32(request,session);return send(0x35,request,4); }
static uint8_t preflight(const uint8_t *package,size_t n,uint32_t *session)
{
    uint32_t off=0;uint8_t status=begin(package,0,0);if(status)return status;
    CHECK(response_len==5);*session=get32(response+1);CHECK(*session && store.session && reserved);
    while(off<n-512u)
    {
        uint16_t count=(uint16_t)(n-512u-off);if(count>991u)count=991u;
        status=block(package,*session,off,count);if(status)return status;off+=count;CHECK(get32(response+1)==off);
    }
    return finish(*session);
}
static uint8_t install(const uint8_t *package,size_t n,uint32_t session)
{
    uint32_t off=0;uint8_t status=begin(package,1,session);if(status)return status;
    CHECK(!store.available && store.generation%2u==1u && response_len==5);
    while(off<n-512u)
    {
        uint16_t count=(uint16_t)(n-512u-off);if(count>991u)count=991u;
        status=block(package,session,off,count);if(status)return status;off+=count;CHECK(get32(response+1)==off);
    }
    return finish(session);
}
static void abort_session(uint32_t session) { put32(request,session);CHECK(send(0x36,request,4)==0);CHECK(!reserved && !store.session); }
static void business_policy(void *user,uint16_t index,const bvpv_param_t *param,bvpv_effective_attr_t *attr)
{
    static const uint8_t min[8]={30},max[8]={70};
    (void)user;(void)param;
    if(index==0) { attr->access=BVPV_ACC_ROO;attr->min=min;attr->max=max; }
}
static void success_and_mount(void)
{
    uint32_t session;unsigned before;bvpv_effective_attr_t attr;uint16_t n;
    memset(flash,255,sizeof(flash));boot();CHECK(!bvpv_catalog_store_ready(&store));
    CHECK(preflight(valid,valid_len,&session)==0 && store.preflight_ready && operations==0);
    CHECK(install(valid,valid_len,session)==0);write_operations=operations;
    CHECK(write_operations>20u && store.committed && bvpv_catalog_store_ready(&store) && store.generation==2);
    CHECK(!memcmp(flash+BVPV_FLASH_PARAM_BASE,valid,valid_len));CHECK(value0[0]==42 && (states[0]&BVPV_STATE_PENDING));
    before=reads;CHECK(bvpv_catalog_effective_attr(&ctx,0,&attr)==0);CHECK(attr.access==BVPV_ACC_RWO && attr.min[0]==20 && attr.max[0]==80);CHECK(reads==before);
    ctx.attr_provider=business_policy;CHECK(bvpv_catalog_effective_attr(&ctx,0,&attr)==0 && attr.access==BVPV_ACC_ROO && attr.min[0]==30 && attr.max[0]==70 && reads==before);
    ctx.attr_provider=NULL;
    CHECK(bvpv_catalog_store_record(&store,0,1,response,&n,sizeof(response))==0);CHECK(response[2]==2 && n>2000);
    put16(request,0);put16(request+2,3500);CHECK(send(0x09,request,4)==0 && response[1]==255 && response[2]==255 && response[3]==0 && response[4]==2);
    put32(request,0);put16(request+4,512);CHECK(send(0x30,request,6)==0 && response_len==549 && !memcmp(response+37,valid,512));
    memcpy(baseline,flash,sizeof(flash));boot();CHECK(bvpv_catalog_store_ready(&store) && store.generation==2 && store.range_count==1);
    CHECK(bvpv_catalog_effective_attr(&ctx,0,&attr)==0 && attr.min[0]==20 && attr.max[0]==80);
}
static void preflight_rejection(void)
{
    static const char *bad[]={"duplicate-param.acat","duplicate-name.acat","duplicate-tree.acat","bad-index.acat","widen-range.acat","missing-range.acat","widen-access.acat","bad-signature.acat","bad-digest.acat","duplicate-error.acat","bad-default.acat","duplicate-enum.acat"};
    unsigned i;uint32_t session=0;size_t n;bvpv_effective_attr_t attr;
    for(i=0;i<sizeof(bad)/sizeof(bad[0]);i++)
    {
        memcpy(flash,baseline,sizeof(flash));boot();n=fixture(bad[i],candidate,sizeof(candidate));CHECK(preflight(candidate,n,&session)!=0);
        CHECK(operations==0 && !reserved && !store.session && store.available);CHECK(!memcmp(flash,baseline,sizeof(flash)));
        CHECK(bvpv_catalog_effective_attr(&ctx,0,&attr)==0 && attr.min[0]==20 && attr.max[0]==80 && store.range_count==1);
    }
    memcpy(flash,baseline,sizeof(flash));boot();CHECK(preflight(valid,valid_len,&session)==0);memcpy(candidate,valid,valid_len);candidate[128]^=1;
    CHECK(begin(candidate,1,session)==BVPV_ERR_STATE && operations==0 && store.preflight_ready);abort_session(session);CHECK(store.available);
    memcpy(candidate,valid,valid_len);put32(candidate+8,BVPV_FLASH_PARAM_BYTES+1u);CHECK(begin(candidate,0,0)==BVPV_ERR_OUT_OF_RANGE && operations==0);
    deny_reserve=true;CHECK(begin(valid,0,0)==BVPV_ERR_BUSY && operations==0);deny_reserve=false;
    memset(store.key,0,64);CHECK(begin(valid,0,0)!=0 && operations==0);memcpy(store.key,key,64);
    store.fault=true;CHECK(begin(valid,0,0)==BVPV_ERR_FLASH && operations==0 && !reserved);
}
static void progress_abort_corruption(void)
{
    uint32_t session;uint8_t b[520]={0};unsigned budget=100000;
    memcpy(flash,baseline,sizeof(flash));boot();memcpy(b+8,valid,512);
    CHECK(!bvpv_catalog_store_request(&store,0x33,b,sizeof(b),response,&response_len,sizeof(response),ticks));
    CHECK(send(0x30,NULL,0)==0 && response[2]==1 && get32(response+29)==store.session_id && operations==0);
    while(!bvpv_catalog_store_take_reply(&store,response,&response_len,sizeof(response)) && budget--) { task(); }
    CHECK(budget && response[0]==0);
    session=get32(response+1);abort_session(session);CHECK(store.available && operations==0);
    CHECK(preflight(valid,valid_len,&session)==0);CHECK(begin(valid,1,session)==0 && !store.available);abort_session(session);
    boot();CHECK(!bvpv_catalog_store_ready(&store) && store.generation==3);
    memcpy(flash,baseline,sizeof(flash));boot();CHECK(preflight(valid,valid_len,&session)==0);CHECK(begin(valid,1,session)==0);
    { uint32_t off=0;while(off<valid_len-512u){uint16_t count=(uint16_t)(valid_len-512u-off);if(count>991u)count=991;CHECK(block(valid,session,off,count)==0);off+=count;} }
    flash[BVPV_FLASH_PARAM_BASE+600u]^=1;CHECK(finish(session)!=0 && !store.available && !reserved);boot();CHECK(!bvpv_catalog_store_ready(&store) && store.generation==3);
    memcpy(flash,baseline,sizeof(flash));flash[BVPV_FLASH_PARAM_BASE+600u]^=1;boot();CHECK(!bvpv_catalog_store_ready(&store) && store.last_error!=0);
    memcpy(flash,baseline,sizeof(flash));memset(flash+BVPV_CATALOG_META_BASE(0),0,96);memset(flash+BVPV_CATALOG_META_BASE(1),0,96);boot();CHECK(!store.available && !store.fault && store.last_error==BVPV_ERR_CRC && operations==0);
    memcpy(flash,baseline,sizeof(flash));reset();fail_read=true;bvpv_catalog_store_boot(&store);while(store.booting)task();CHECK(store.fault && !store.available && operations==0);
}
static void disconnect_reply_isolation(void)
{
    unsigned mode;
    for(mode=0;mode<3;mode++)
    {
        uint8_t b[520]={0};uint32_t old_session,session;unsigned budget=100000;
        memcpy(flash,baseline,sizeof(flash));boot();memcpy(b+8,valid,512);
        CHECK(!bvpv_catalog_store_request(&store,0x33,b,sizeof(b),response,&response_len,sizeof(response),ticks));
        old_session=store.session_id;
        /* Mode 2 loses a completed BEGIN reply; other modes disconnect before it. */
        if(mode==2) { while(!store.reply_ready && budget--)task();CHECK(budget && store.reply[0]==0); }
        bvpv_catalog_store_cancel(&store);
        if(mode==2)
        {
            CHECK(bvpv_catalog_store_request(&store,0x33,b,sizeof(b),response,&response_len,sizeof(response),ticks));
            CHECK(response[0]==BVPV_ERR_BUSY && !store.reply_ready);
        }
        while(store.session && budget--)task();
        CHECK(budget && !reserved && store.reply_ready && store.reply[0]==BVPV_ERR_STATE);
        /* The disconnected service no longer polls this completion. */
        if(mode)
        {
            CHECK(send(0x30,NULL,0)==0 && response[2]==0 && get32(response+29)==0);
            CHECK(!bvpv_catalog_store_take_reply(&store,response,&response_len,sizeof(response)));
        }
        CHECK(preflight(valid,valid_len,&session)==0 && session!=old_session && operations==0);
        CHECK(install(valid,valid_len,session)==0 && store.committed && store.generation==4u && bvpv_catalog_store_ready(&store));
        CHECK(!bvpv_catalog_store_take_reply(&store,response,&response_len,sizeof(response)));
    }
}
static void power_cuts(void)
{
    unsigned operation,partial;uint32_t session;
    for(operation=1;operation<=write_operations;operation++)for(partial=0;partial<3;partial++)
    {
        memcpy(flash,baseline,sizeof(flash));boot();CHECK(preflight(valid,valid_len,&session)==0);cut_at=operation;cut_bytes=partial==0?0u:partial==1?17u:4096u;
        CHECK(install(valid,valid_len,session)!=0 && dead && operations==operation);boot();
        if(bvpv_catalog_store_ready(&store)) CHECK((store.generation==2u || store.generation==4u) && !memcmp(flash+BVPV_FLASH_PARAM_BASE,valid,valid_len));
        else CHECK(store.generation==3u);
        CHECK(!memcmp(flash,baseline,BVPV_FLASH_PARAM_BASE));
        CHECK(!memcmp(flash+BVPV_FLASH_MCU_BASE,baseline+BVPV_FLASH_MCU_BASE,BVPV_CATALOG_META_BASE(0)-BVPV_FLASH_MCU_BASE));
        CHECK(!memcmp(flash+BVPV_CATALOG_META_BASE(1)+4096u,baseline+BVPV_CATALOG_META_BASE(1)+4096u,sizeof(flash)-BVPV_CATALOG_META_BASE(1)-4096u));
    }
}
static void shared_owner(void)
{
    bvpv_cd_store_t cd;bvpv_cd_io_t cd_io=io;uint8_t reply[25],p[8];uint16_t n;uint32_t session;unsigned count,before;
    memcpy(flash,baseline,sizeof(flash));boot();cd_io.ctx=&cd_owner_token;bvpv_cd_store_init(&cd,&cd_io);CHECK(!bvpv_cd_store_boot(&cd));
    while(cd.booting) { bvpv_cd_store_task(&cd,++ticks); }
    CHECK(!owner && !reserved);
    CHECK(preflight(valid,valid_len,&session)==0 && owner==&catalog_owner_token);before=operations;
    put32(p,2048);put32(p+4,0);bvpv_cd_store_request(&cd,0x0A,p,8,ticks);CHECK(bvpv_cd_store_take_reply(&cd,reply,&n) && reply[0]==BVPV_ERR_BUSY && owner==&catalog_owner_token && operations==before);
    abort_session(session);bvpv_cd_store_request(&cd,0x0A,p,8,ticks);
    for(count=0;count<1000u;count++) { if(bvpv_cd_store_take_reply(&cd,reply,&n))break;bvpv_cd_store_task(&cd,++ticks); }
    CHECK(count<1000u && reply[0]==0 && owner==&cd_owner_token);before=operations;
    CHECK(begin(valid,0,0)==BVPV_ERR_BUSY && owner==&cd_owner_token && operations==before);
    bvpv_cd_store_cancel(&cd);for(count=0;count<1000u && cd.session;count++)bvpv_cd_store_task(&cd,++ticks);
    CHECK(!owner && !reserved && !cd.session);boot();CHECK(bvpv_catalog_store_ready(&store) && store.generation==2);
}
static void range_cap(void)
{
    static bvpv_runtime_param_t expanded[65];static uint8_t values[65][2],expanded_states[65];uint8_t public_key[64];uint32_t session;unsigned i;size_t n;
    reset();for(i=0;i<65;i++) { memset(expanded+i,0,sizeof(expanded[i]));expanded[i].addr=0x020101u+i;expanded[i].value=values[i];expanded[i].type=BVPV_TYPE_UINT16;expanded[i].elem_count=1;expanded[i].elem_size=2;expanded[i].access=BVPV_ACC_RWA;expanded[i].flags=1;expanded[i].ext_flags=3;expanded[i].max[0]=100; }
    ctx.runtime=expanded;ctx.count=65;ctx.states=expanded_states;CHECK(fixture("range-overrides.public.bin",public_key,sizeof(public_key))==64);
    bvpv_catalog_store_init(&store,&io,&ctx,public_key,"TEST.RANGES.1");n=fixture("range-overrides-64.acat",candidate,sizeof(candidate));
    CHECK(preflight(candidate,n,&session)==0 && store.candidate_range_count==64u && operations==0);abort_session(session);
    n=fixture("range-overrides-65.acat",candidate,sizeof(candidate));CHECK(preflight(candidate,n,&session)==BVPV_ERR_OUT_OF_RANGE && operations==0 && !store.session && !reserved);
}
int main(void)
{
    CHECK(fixture("public-key.bin",key,sizeof(key))==64);valid_len=fixture("valid.acat",valid,sizeof(valid));
    success_and_mount();preflight_rejection();progress_abort_corruption();disconnect_reply_isolation();power_cuts();shared_owner();range_cap();
    printf("catalog_store: signed preflight, policy, bounded boot, partitions and %u power-cut cases PASS (store %uB)\n",write_operations*3u,(unsigned)sizeof(store));return 0;
}