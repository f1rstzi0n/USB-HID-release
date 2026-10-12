/* APP 实际只读验证模块：有效、签名/镜像篡改和非法向量。 */
#include "fw_image.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint8_t flash[0x80000],package[2307],key[64],uid[12]={1,2,3};
static unsigned progress;
void fw_validation_progress(void) { ++progress; }
static void load(const char *name,uint8_t *out,size_t size)
{
    char file_path[1024];
    FILE *file;
    assert(snprintf(file_path,sizeof(file_path),"%s/%s",FW_FIXTURE_DIR,name)>0);
    file=fopen(file_path,"rb");assert(file);
    assert(fread(out,1,size,file)==size && fgetc(file)==EOF);fclose(file);
}
static bool read_io(void *ctx,uint32_t address,uint8_t *out,uint32_t size)
{
    (void)ctx;
    assert(address>=FW_HEADER_BASE && size<=FW_FLASH_END-address);
    memcpy(out,flash+address-0x08000000u,size);return true;
}
static void install(const char *fixture)
{
    uint32_t commit=FW_COMMIT_VALUE;
    load(fixture,package,sizeof(package));memset(flash,255,sizeof(flash));
    memcpy(flash+FW_HEADER_BASE-0x08000000u,package,256);
    memcpy(flash+FW_APP_BASE-0x08000000u,package+256,sizeof(package)-256);
    memcpy(flash+FW_COMMIT_ADDRESS-0x08000000u,&commit,4);
}
int main(void)
{
    fw_image_t image;
    uint8_t info[FW_INFO_SIZE];
    const fw_flash_io_t io={.read=read_io};
    load("public-key.bin",key,sizeof(key));install("valid.afw");
    fw_image_init(&image,&io,key,uid);assert(image.valid_application && progress>8);
    fw_image_info(&image,FW_MODE_APP,info);
    assert(info[1]==FW_MODE_APP && info[3]==1 && !memcmp(info+5,uid,12));
    flash[FW_APP_BASE-0x08000000u+100]^=1;
    fw_image_init(&image,&io,key,uid);assert(!image.valid_application && image.last_error==FW_ERR_DIGEST);
    install("valid.afw");flash[FW_HEADER_BASE-0x08000000u+192]^=1;
    fw_image_init(&image,&io,key,uid);assert(!image.valid_application && image.last_error==FW_ERR_SIGNATURE);
    install("bad-vectors.afw");fw_image_init(&image,&io,key,uid);
    assert(!image.valid_application && image.last_error==3);
    flash[FW_COMMIT_ADDRESS-0x08000000u]=255;fw_image_init(&image,&io,key,uid);
    assert(!image.valid_application);
    puts("APP readonly signed image validation passed");return 0;
}
