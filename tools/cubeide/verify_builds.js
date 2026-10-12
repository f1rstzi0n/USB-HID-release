/* 读取实际 ELF/符号和 map，核验固定布局与 APP/Boot 职责。 */
const fs=require('fs'),path=require('path'),cp=require('child_process');
const root=path.resolve(__dirname,'../..');
const name=fs.readFileSync(path.join(root,'.project'),'utf8').match(/<name>([^<]+)<\/name>/)[1];
const boot=name.endsWith('-Boot');
const plugins='D:/Software/STMicroelectronics/STM32CubeIDE/STM32CubeIDE/plugins';
const bin=process.env.ARM_TOOLS || path.join(plugins,fs.readdirSync(plugins).find(n=>/^com\.st\.stm32cube\.ide\.mcu\.externaltools\.gnu-tools-for-stm32\..*\.win32_/.test(n)),'tools/bin');
function check(ok,msg){if(!ok)throw Error(msg)}
function sections(data){
 check(data.toString('hex',0,4)==='7f454c46' && data[4]===1 && data[5]===1,'Expected ELF32 little endian');
 const offset=data.readUInt32LE(32),size=data.readUInt16LE(46),count=data.readUInt16LE(48),index=data.readUInt16LE(50);
 const strings=data.readUInt32LE(offset+index*size+16),out=[];
 for(let i=0;i<count;i++){
  const sh=offset+i*size,n=strings+data.readUInt32LE(sh);
  out.push({name:data.toString('utf8',n,data.indexOf(0,n)),type:data.readUInt32LE(sh+4),flags:data.readUInt32LE(sh+8),
   address:data.readUInt32LE(sh+12),offset:data.readUInt32LE(sh+16),size:data.readUInt32LE(sh+20)});
 }
 return out;
}
const results=[];
for(const config of ['Debug','Release']){
 const folder=path.join(root,config),elf=path.join(folder,name+'.elf'),data=fs.readFileSync(elf),sec=sections(data);
 const get=n=>sec.find(s=>s.name===n);
 const vector=get('.isr_vector'),mailbox=get('.boot_request'),ramData=get('.data'),bss=get('.bss'),reserve=get('._user_heap_stack');
 check(vector.address===(boot?0x08000000:0x08010200),'Vector base mismatch');
 check(data.readUInt32LE(vector.offset)===0x2001fff8,'Initial MSP must exclude mailbox');
 check(mailbox.address===0x2001fff8 && mailbox.size===8 && mailbox.type===8,'Mailbox must be 8-byte NOBITS/NOLOAD');
 const imageSize=fs.statSync(path.join(folder,name+'.bin')).size;
 check(imageSize<=(boot?65536:0x6fe00),'Flash capacity exceeded');
 const r=cp.spawnSync(path.join(bin,'arm-none-eabi-nm.exe'),['-S','--size-sort','--defined-only',elf],{encoding:'utf8'});
 check(r.status===0,r.stderr);
 const symbols=r.stdout.replace(/\r\n/g,'\n');
 check(symbols.split('\n').filter(line=>/\b[Tt]\s+OTG_FS_IRQHandler$/.test(line)).length===1,'USB interrupt must have one implementation');
 check(!/HAL_PCD_IRQHandler|HAL_PCD_Init/.test(symbols),'PCD linked');
 if(boot)check(!/\b(?:[xv]Task\w*|[xv]Queue\w*|pvPortMalloc|tud_msc_\w*|w25q64_\w*|content_store_\w*|bvpv_catalog_\w*)$/.test(symbols.split('\n').filter(l=>/[TtBbDd] /.test(l)).join('\n')) && !/pvPortMalloc|xTaskCreate|msc_disk|w25q64|content_store|bvpv_service|bvpv_catalog/.test(symbols),'Boot carries APP dependencies');
 else check(!/fw_update_|HAL_FLASH_Program|HAL_FLASHEx_Erase/.test(symbols),'APP carries Boot writing logic');
 fs.writeFileSync(path.join(root,'docs/validation',config+'-symbols.txt'),symbols);
 const footprint=ramData.size+bss.size;
 const reserved=reserve?reserve.size:0;
 results.push({project:name,config,flash_bytes:imageSize,data_bytes:ramData.size,bss_bytes:bss.size,
  static_ram_bytes:footprint,linker_heap_stack_bytes:reserved,ram_after_static_and_reserve:131064-footprint-reserved,
  vector:'0x'+vector.address.toString(16),mailbox:'0x'+mailbox.address.toString(16),initial_msp:'0x2001fff8'});
}
fs.writeFileSync(path.join(root,'docs/validation/build-layout.json'),JSON.stringify(results,null,2)+'\n');
console.log(JSON.stringify(results));
