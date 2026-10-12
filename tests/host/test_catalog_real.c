/* Native release-package check: actual compact runtime + catalog validator. No hardware. */
#include "bvpv_catalog_store.h"
#include "bvpv_catalog_data.h"
#include "bvpv_errcode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"FAIL line %d: %s (error=%u section=%u ordinal=%u offset=%lu)\n",__LINE__,#x,store.last_error,store.section,store.ordinal,(unsigned long)store.cursor);exit(1); } } while(0)
static bvpv_catalog_store_t store;
static uint8_t package[0x100000],key[64],metadata[96],input[3508],output[4096];
static uint32_t package_len,ticks,task_bytes;static unsigned reads,reserves;static bool held;
static void put32(uint8_t *p,uint32_t n) { unsigned i;for(i=0;i<4;i++)p[i]=(uint8_t)(n>>(8*i)); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static bvpv_cd_read_result_t read_io(void *user,uint32_t a,uint8_t *out,uint32_t n)
{
    (void)user;++reads;task_bytes+=n;CHECK(n<=256 && task_bytes<=256);
    if(a>=BVPV_FLASH_PARAM_BASE && a<BVPV_FLASH_PARAM_BASE+package_len && n<=BVPV_FLASH_PARAM_BASE+package_len-a)memcpy(out,package+a-BVPV_FLASH_PARAM_BASE,n);
    else if(a==BVPV_CATALOG_META_BASE(0) && n==96)memcpy(out,metadata,n);
    else if(a==BVPV_CATALOG_META_BASE(1) && n==96)memset(out,255,n);
    else CHECK(0);
    return BVPV_CD_READ_OK;
}
static bool erase_io(void *user,uint32_t a) { (void)user;(void)a;CHECK(0);return false; }
static bool program_io(void *user,uint32_t a,const uint8_t *p,uint16_t n) { (void)user;(void)a;(void)p;(void)n;CHECK(0);return false; }
static int poll_io(void *user) { (void)user;return 0; }
static bool reserve_io(void *user,bool lock) { (void)user;held=lock;reserves++;return true; }
static const bvpv_cd_io_t io={0,read_io,erase_io,program_io,poll_io,reserve_io};
static size_t load(const char *name,uint8_t *p,size_t n) { size_t count;FILE *f=fopen(name,"rb");CHECK(f);count=fread(p,1,n,f);CHECK(count && !ferror(f) && fgetc(f)==EOF);fclose(f);return count; }
static void task(void) { task_bytes=0;bvpv_catalog_store_task(&store,++ticks); }
static uint16_t command(uint8_t svc,const uint8_t *p,uint16_t n)
{
    unsigned budget=100000;uint16_t bytes=0;
    if(!bvpv_catalog_store_request(&store,svc,p,n,output,&bytes,sizeof(output),ticks))
        while(!bvpv_catalog_store_take_reply(&store,output,&bytes,sizeof(output)) && budget--)task();
    CHECK(budget && bytes && output[0]==0);return bytes;
}
int main(int argc,char **argv)
{
    uint8_t begin[520]={0};uint32_t session,off;unsigned before; bvpv_service_ctx_t runtime_ctx;
    CHECK(argc==3);package_len=(uint32_t)load(argv[1],package,sizeof(package));CHECK(load(argv[2],key,sizeof(key))==64);
    runtime_ctx=*bvpv_catalog_data_ctx();bvpv_catalog_store_init(&store,&io,&runtime_ctx,key,bvpv_catalog_data_app_identity());
    runtime_ctx.policy_provider=bvpv_catalog_store_policy;runtime_ctx.policy_provider_user=&store;
    memcpy(begin+8,package,512);CHECK(command(0x33,begin,sizeof(begin))==5);session=get32(output+1);
    for(off=0;off<package_len-512u;)
    {
        uint16_t bytes=(uint16_t)(package_len-512u-off>3500u?3500u:package_len-512u-off);
        put32(input,session);put32(input+4,off);memcpy(input+8,package+512u+off,bytes);CHECK(command(0x34,input,(uint16_t)(8u+bytes))==5);off+=bytes;CHECK(get32(output+1)==off);
    }
    put32(input,session);CHECK(command(0x35,input,4)==1 && store.preflight_ready && reads==0);CHECK(command(0x36,input,4)==1 && !held);
    memset(metadata,255,sizeof(metadata));memcpy(metadata,"ACC1",4);metadata[4]=0;memset(metadata+5,0,3);put32(metadata+8,1);put32(metadata+12,package_len);
    memcpy(metadata+16,package+64,32);memcpy(metadata+48,package+32,32);put32(metadata+80,bvpv_cd_crc32(metadata,80));metadata[92]=0xA5;
    bvpv_catalog_store_init(&store,&io,&runtime_ctx,key,bvpv_catalog_data_app_identity());before=reserves;bvpv_catalog_store_boot(&store);
    for(off=0;store.booting && off<100000;off++)task();
    CHECK(off<100000 && bvpv_catalog_store_ready(&store) && !held && before==reserves && store.range_count==0);
    printf("real catalog: %lu bytes, %u params, %u trees; signed zero-I/O preflight + bounded mount PASS (%lu tasks)\n",(unsigned long)package_len,runtime_ctx.count,(unsigned)(package[162]|((uint16_t)package[163]<<8)),(unsigned long)off);
    return 0;
}