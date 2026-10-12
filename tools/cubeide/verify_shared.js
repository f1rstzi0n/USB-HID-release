/* 校验本工程冻结的共享源码；可显式传入另一工程目录做副本比较。 */
const fs=require('fs'),path=require('path'),crypto=require('crypto');
const root=path.resolve(__dirname,'../..'),peer=process.argv[2]&&path.resolve(process.argv[2]);
const manifest=JSON.parse(fs.readFileSync(path.join(root,'docs/shared-source-manifest.json'),'utf8'));
const hash=file=>crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
function check(ok,message){if(!ok)throw Error(message)}
for(const [rel,expected] of Object.entries(manifest.files)){
 check(hash(path.join(root,rel))===expected,'共享文件已变化：'+rel);
 if(peer)check(hash(path.join(peer,rel))===expected,'另一工程版本不同：'+rel);
}
const header=fs.readFileSync(path.join(root,'App/firmware/fw_public_key.h'),'utf8');
const key=Buffer.from([...header.matchAll(/0x([0-9a-f]{2})/gi)].map(m=>parseInt(m[1],16)));
check(key.length===64 && key.equals(fs.readFileSync(path.join(root,'release-public/public-key.bin'))),'公钥头与发布工具公钥不同');
check(hash(path.join(root,'release-public/public-key.bin'))===manifest.public_key_sha256,'公钥版本改变');
console.log('PASS '+Object.keys(manifest.files).length+' 个共享文件、公钥、固件契约'+(peer?'，两工程副本一致':''));
