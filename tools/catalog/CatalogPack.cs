/* 独立 ACAT 发布器：使用既有 Windows CNG 命名钥，只允许导出公钥。
 * 生产路径不创建密钥；self-test 创建的无名临时钥不持久化。 */
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Web.Script.Serialization;

internal static class CatalogPack
{
    private const string Schema = "BVPV-CATALOG/1;PARAM=v4;TREE=v4;UI=role-address-order;INDEX=offset32;ERR=code-text;LE";
    private static readonly byte[] Domain = Encoding.ASCII.GetBytes("BVPV-CATALOG-V1\0");
    private sealed class Source
    {
        internal byte[] Body, Runtime;
        internal int Params, Trees, Ui, Errors;
        internal int[] Offsets, Lengths;
    }
    private static int Main(string[] args)
    {
        try
        {
            if (args.Length > 0 && args[0] == "source") return CatalogSource.Run(args.Skip(1).ToArray());
            bool test = args.Length > 0 && args[0] == "--self-test";
            if (test) args = args.Skip(1).ToArray();
            Dictionary<string, string> options = Options(args);
            Source source = options.ContainsKey("--input") ? Read(Required(options, "--input")) : test ? Tiny() : null;
            if (source == null) throw new ArgumentException("Missing --input.");
            if (test)
            {
                using (CngKey key = CngKey.Create(CngAlgorithm.ECDsaP256))
                {
                    string version = options.ContainsKey("--version") ? Required(options, "--version") : "SELFTEST", target = options.ContainsKey("--target") ? Required(options, "--target") : "BV500-F446";
                    byte[] package = Package(source, version, key, target), publicKey = PublicKey(key);
                    if (!Verify(package, publicKey)) throw new Exception("Signature roundtrip failed.");
                    foreach (int offset in new[] { 0,16,32,64,96,128,160,166,168,200,207,208,447,448,511,512,package.Length-1 })
                    {
                        byte[] changed = (byte[])package.Clone(); changed[offset] ^= 1;
                        if (Verify(changed, publicKey)) throw new Exception("Tamper accepted at " + offset + ".");
                    }
                    using (CngKey wrong = CngKey.Create(CngAlgorithm.ECDsaP256))
                        if (Verify(package, PublicKey(wrong))) throw new Exception("Wrong key accepted.");
                    byte[] regenerated = Package(source, version, key, target);
                    if (!ReuseExisting(package, regenerated, publicKey).SequenceEqual(package)) throw new Exception("Release reuse changed the existing package.");
                    byte[] invalid = (byte[])package.Clone(); invalid[448] ^= 1;
                    byte[] different = Package(source, version == "REUSE-TEST" ? "REUSE-TEST-2" : "REUSE-TEST", key, target);
                    foreach (byte[] previous in new[] { invalid, different })
                    {
                        bool rejected = false;
                        try { ReuseExisting(previous, regenerated, publicKey); } catch (InvalidDataException) { rejected = true; }
                        if (!rejected) throw new Exception("Release reuse accepted an invalid or changed package.");
                    }
                    using (ECDsaCng signer = new ECDsaCng(key))
                        if (signer.VerifyHash(Hash(package.Take(448).ToArray()), package.Skip(448).Take(64).ToArray()))
                            throw new Exception("Signature unexpectedly omitted the catalog domain.");
                    if (options.ContainsKey("--output"))
                    {
                        string output = Required(options, "--output");
                        if (File.Exists(output) || File.Exists(output + ".public.bin")) throw new IOException("Test output already exists.");
                        WriteNew(output, package); WriteNew(output + ".public.bin", publicKey);
                    }
                }
                Console.WriteLine("PASS ACAT CNG: body/index/contract validation, domain signature, tamper rejection and unchanged release reuse.");
            }
            else
            {
                string output = Required(options, "--output");
                if (!output.EndsWith(".acat", StringComparison.OrdinalIgnoreCase)) throw new ArgumentException("Release output must end in .acat.");
                byte[] package;
                using (CngKey key = CngKey.Open(Required(options, "--key")))
                {
                    byte[] publicKey = PublicKey(key);
                    byte[] trusted = ReadBounded(Required(options, "--public-key"), 64);
                    if (trusted.Length != 64 || !publicKey.SequenceEqual(trusted)) throw new CryptographicException("Release key does not match the trusted public key.");
                    package = Package(source, Required(options, "--version"), key, options.ContainsKey("--target") ? Required(options, "--target") : "BV500-F446");
                    if (options.ContainsKey("--reuse-existing"))
                    {
                        byte[] previous = ReadBounded(Required(options, "--reuse-existing"), 1048576);
                        package = ReuseExisting(previous, package, publicKey);
                    }
                }
                WriteNew(output, package);
                Console.WriteLine("Generated signed catalog: " + package.Length + " bytes.");
            }
            return 0;
        }
        catch (Exception exception) { Console.Error.WriteLine(exception.Message); return 1; }
    }
    private static Dictionary<string, string> Options(string[] args)
    {
        string[] valid = { "--input", "--version", "--output", "--key", "--public-key", "--reuse-existing", "--target" };
        Dictionary<string,string> options = new Dictionary<string,string>(StringComparer.Ordinal);
        if (args.Length % 2 != 0) throw new ArgumentException("Use --input BODY --version VERSION --key CNG_NAME --public-key FILE --output FILE.acat, or --self-test [--input BODY] [--output TEST.acat].");
        for (int i=0;i<args.Length;i+=2)
        { if (!valid.Contains(args[i]) || options.ContainsKey(args[i])) throw new ArgumentException("Unknown or duplicate option."); options.Add(args[i],args[i+1]); }
        return options;
    }
    private static string Required(Dictionary<string,string> options, string name)
    { string value; if (!options.TryGetValue(name,out value) || string.IsNullOrWhiteSpace(value)) throw new ArgumentException("Missing " + name + "."); return value; }
    private static byte[] ReadBounded(string path, int limit)
    {
        using (FileStream stream = File.OpenRead(path))
        {
            if (stream.Length <= 0 || stream.Length > limit) throw new InvalidDataException("Input exceeds its capacity.");
            byte[] data = new byte[(int)stream.Length]; int read=0, next;
            while (read<data.Length && (next=stream.Read(data,read,data.Length-read))>0) read+=next;
            if (read!=data.Length || stream.ReadByte()!=-1) throw new EndOfStreamException("Input changed while reading.");
            return data;
        }
    }
    private static byte[] ReuseExisting(byte[] previous, byte[] package, byte[] publicKey)
    {
        if (previous.Length != package.Length || !Verify(previous, publicKey) || !previous.Take(448).SequenceEqual(package.Take(448)) || !previous.Skip(512).SequenceEqual(package.Skip(512)))
            throw new InvalidDataException("Existing release has different content or an invalid signature; choose a new catalog version.");
        return previous;
    }
    private static Source Read(string path)
    {
        byte[] description = ReadBounded(path + ".json",4096);
        Dictionary<string,object> meta = new JavaScriptSerializer().Deserialize<Dictionary<string,object>>(Encoding.UTF8.GetString(description));
        string[] keys = {"parameterCount","treeCount","uiCount","errorCount","parameterOffset","parameterBytes","treeOffset","treeBytes","uiOffset","uiBytes","indexOffset","indexBytes","errorOffset","errorBytes"};
        if (meta.Count!=keys.Length || keys.Any(key=>!meta.ContainsKey(key))) throw new InvalidDataException("Unexpected body description fields.");
        Func<string,int> get = key => { if (!(meta[key] is int) || (int)meta[key]<0) throw new InvalidDataException("Invalid body description number."); return (int)meta[key]; };
        Source source = new Source {Body=ReadBounded(path,1048576-512),Runtime=ReadBounded(path+".runtime",16+2300*8),
            Params=get("parameterCount"),Trees=get("treeCount"),Ui=get("uiCount"),Errors=get("errorCount"),
            Offsets=new[]{get("parameterOffset"),get("treeOffset"),get("uiOffset"),get("indexOffset"),get("errorOffset")},
            Lengths=new[]{get("parameterBytes"),get("treeBytes"),get("uiBytes"),get("indexBytes"),get("errorBytes")}};
        Validate(source); return source;
    }
    private static int U16(byte[] b,int p) { return b[p] | b[p+1]<<8; }
    private static uint U32(byte[] b,int p) { return (uint)(b[p]|b[p+1]<<8|b[p+2]<<16|b[p+3]<<24); }
    private static void W16(byte[] b,int p,int v) { b[p]=(byte)v;b[p+1]=(byte)(v>>8); }
    private static void W32(byte[] b,int p,int v) { for(int i=0;i<4;i++)b[p+i]=(byte)(v>>(i*8)); }
    private static int Address(byte[] b,int p) { return b[p]<<16|b[p+1]<<8|b[p+2]; }
    private static void Ascii(byte[] b,int p,int size)
    { if(size<1 || p<0 || p>b.Length-size || Enumerable.Range(p,size).Any(i=>b[i]<32||b[i]>126))throw new InvalidDataException("Invalid ASCII field."); }
    private static int FixedSize(int type)
    { int[] sizes={0,1,2,4,8,4,4,4,4,2,8,16,32,0}; if(type>=sizes.Length)throw new InvalidDataException("Unknown parameter type.");return sizes[type]; }
    private static int CompareNumber(int type,byte[] a,byte[] b)
    {
        if(type==6)return BitConverter.ToSingle(a,0).CompareTo(BitConverter.ToSingle(b,0));
        if(type==5||type==7)return unchecked((int)U32(a,0)).CompareTo(unchecked((int)U32(b,0)));
        ulong first=0,second=0;for(int i=a.Length-1;i>=0;i--){first=first<<8|a[i];second=second<<8|b[i];}
        return first.CompareTo(second);
    }
    private static void Validate(Source s)
    {
        if(s.Params<1||s.Params>2300||s.Trees<0||s.Trees>=65535||s.Ui<0||s.Ui>64||s.Errors<1||s.Errors>256)throw new InvalidDataException("Catalog counts exceed capacity.");
        long end=0;for(int i=0;i<5;i++){if(s.Offsets[i]!=end||s.Lengths[i]<0)throw new InvalidDataException("Body sections are not contiguous.");end+=s.Lengths[i];}
        if(end!=s.Body.Length||s.Body.Length>1048576-512||s.Lengths[3]!=(s.Params+s.Trees)*4)throw new InvalidDataException("Body/index size mismatch.");
        int[] counts={s.Params,s.Trees,s.Ui};int indexed=0;
        if(s.Runtime.Length!=16+s.Params*8 || !s.Runtime.Take(14).SequenceEqual(Encoding.ASCII.GetBytes("BVPV-RUNTIME-1")) || U16(s.Runtime,14)!=s.Params)throw new InvalidDataException("Invalid runtime sidecar header or size.");
        Dictionary<int,int> compositeSizes=new Dictionary<int,int>();
        for(int i=0;i<s.Params;i++)
        {
            int p=16+i*8;
            if(s.Runtime[p+3]==13)
            {
                int size=U16(s.Runtime,p+6),address=Address(s.Runtime,p);
                if(size<1||size>64||compositeSizes.ContainsKey(address))throw new InvalidDataException("Invalid composite runtime size.");
                compositeSizes.Add(address,size);
            }
        }
        SortedDictionary<int,byte[]> runtime = new SortedDictionary<int,byte[]>();
        HashSet<int> nodes=new HashSet<int>{0},addresses=new HashSet<int>(),treeAddresses=new HashSet<int>();
        HashSet<string> roles=new HashSet<string>(),names=new HashSet<string>(),groupKeys=new HashSet<string>();
        for(int section=0;section<3;section++)
        {
            int p=s.Offsets[section],limit=p+s.Lengths[section];
            for(int record=0;record<counts[section];record++)
            {
                if(p>limit-2)throw new InvalidDataException("Missing record prefix.");
                int length=U16(s.Body,p),start=p+2,finish=start+length;
                if(length<1||length>3500||finish>limit)throw new InvalidDataException("Record crosses section boundary.");
                if(section<2&&U32(s.Body,s.Offsets[3]+4*indexed++)!=(uint)start)throw new InvalidDataException("Index does not point at the record.");
                if(section==0)
                {
                    if(length<12)throw new InvalidDataException("Short parameter record.");
                    int addr=Address(s.Body,start),type=s.Body[start+3],count=U16(s.Body,start+7),name=s.Body[start+9];
                    if(!addresses.Add(addr)||s.Body[start+4]>4||s.Body[start+5]>1||s.Body[start+6]!=0||count<1||count>256||start+12+name>finish)throw new InvalidDataException("Invalid parameter core.");
                    Ascii(s.Body,start+10,name);int ext=start+10+name;
                    if(!names.Add(Encoding.ASCII.GetString(s.Body,start+10,name)))throw new InvalidDataException("Duplicate parameter name.");
                    if(ext+2+U16(s.Body,ext)!=finish)throw new InvalidDataException("Extension length mismatch.");
                    int size=FixedSize(type),q=ext+2;HashSet<int> tags=new HashSet<int>();byte[] lower=null,upper=null;
                    while(q<finish)
                    {
                        if(q>finish-3)throw new InvalidDataException("Truncated TLV.");
                        int tag=s.Body[q],n=U16(s.Body,q+1);q+=3;
                        if(n>finish-q||tag<1||tag>5||!tags.Add(tag))throw new InvalidDataException("Invalid TLV.");
                        if(tag<=3)
                        {
                            if(type==13&&tag==3){if(n<1||n>64)throw new InvalidDataException("Composite default size is invalid.");size=n;}
                            else if(n!=size||(type==0&&tag!=3)||(tag<3&&(type>=10)))throw new InvalidDataException("Typed TLV size mismatch at " + addr.ToString("X6") + " tag=" + tag + " type=" + type + " bytes=" + n + ", expected=" + size + ".");
                            if(type==6 && (U32(s.Body,q)&0x7F800000u)==0x7F800000u)throw new InvalidDataException("Nonfinite floating point bound/default.");
                            if(tag==1)lower=s.Body.Skip(q).Take(n).ToArray();
                            if(tag==2)upper=s.Body.Skip(q).Take(n).ToArray();
                        }
                        if(tag==4)
                        {
                            int item=q,items=0;while(item<q+n){if(item>q+n-5)throw new InvalidDataException("Short enum.");int text=s.Body[item+4];item+=5;if(text>q+n-item)throw new InvalidDataException("Enum text exceeds TLV.");item+=text;items++;}
                            if(items<1||items>32)throw new InvalidDataException("Enum capacity exceeded.");
                        }
                        if(tag==5&&(n<1||n>255))throw new InvalidDataException("Unit capacity exceeded.");
                        q+=n;
                    }
                    if(lower!=null&&upper!=null&&CompareNumber(type,lower,upper)>0)throw new InvalidDataException("Minimum exceeds maximum.");
                    if(type==13&&size==0&&!compositeSizes.TryGetValue(addr,out size))throw new InvalidDataException("Composite contract needs an explicit runtime size.");
                    byte[] contract=new byte[8];Buffer.BlockCopy(s.Body,start,contract,0,4);W16(contract,4,count);W16(contract,6,size);runtime.Add(addr,contract);
                }
                else if(section==1)
                {
                    int kind=s.Body[start];
                    if(kind==1){if(length<7||!nodes.Contains(U16(s.Body,start+3))||!nodes.Add(U16(s.Body,start+1))||length!=6+s.Body[start+5])throw new InvalidDataException("Invalid tree group.");Ascii(s.Body,start+6,s.Body[start+5]);if(!groupKeys.Add(Encoding.ASCII.GetString(s.Body,start+6,s.Body[start+5])))throw new InvalidDataException("Duplicate tree key.");}
                    else if(kind==2){if(length!=6||!nodes.Contains(U16(s.Body,start+1))||!addresses.Contains(Address(s.Body,start+3))||!treeAddresses.Add(Address(s.Body,start+3)))throw new InvalidDataException("Invalid tree parameter.");}
                    else throw new InvalidDataException("Unknown tree kind.");
                }
                else
                {
                    int role=s.Body[start];if(length!=role+5)throw new InvalidDataException("Invalid UI record.");Ascii(s.Body,start+1,role);
                    if(!roles.Add(Encoding.ASCII.GetString(s.Body,start+1,role))||!addresses.Contains(Address(s.Body,start+1+role)))throw new InvalidDataException("Duplicate or unbound UI role.");
                }
                p=finish;
            }
            if(p!=limit)throw new InvalidDataException("Unexpected trailing record bytes.");
        }
        if(s.Trees>0&&!addresses.SetEquals(treeAddresses))throw new InvalidDataException("Tree does not cover the parameter set.");
        int error=s.Offsets[4],errorEnd=error+s.Lengths[4];HashSet<int> codes=new HashSet<int>();
        for(int i=0;i<s.Errors;i++)
        {
            if(error>errorEnd-4)throw new InvalidDataException("Short error record.");
            int size=U16(s.Body,error),text=s.Body[error+3];
            if(size!=text+2||error+2+size>errorEnd||!codes.Add(s.Body[error+2]))throw new InvalidDataException("Invalid or duplicate error code.");
            Ascii(s.Body,error+4,text);error+=size+2;
        }
        if(error!=errorEnd)throw new InvalidDataException("Error section length mismatch.");
        List<byte> canonical=new List<byte>(Encoding.ASCII.GetBytes("BVPV-RUNTIME-1"));canonical.Add((byte)s.Params);canonical.Add((byte)(s.Params>>8));foreach(byte[] item in runtime.Values)canonical.AddRange(item);
        if(!canonical.SequenceEqual(s.Runtime))throw new InvalidDataException("Runtime sidecar differs from the body contract.");
    }
    private static byte[] Hash(byte[] bytes) { using(SHA256 hash=SHA256.Create())return hash.ComputeHash(bytes); }
    private static byte[] Transcript(byte[] package) { return Domain.Concat(package.Take(448)).ToArray(); }
    private static byte[] PublicKey(CngKey key)
    {
        byte[] blob=key.Export(CngKeyBlobFormat.EccPublicBlob);
        if(key.Algorithm!=CngAlgorithm.ECDsaP256||blob.Length!=72||U32(blob,0)!=0x31534345u)throw new CryptographicException("Signing key must be ECDSA P-256.");
        return blob.Skip(8).ToArray();
    }
    private static byte[] Package(Source source,string version,CngKey key,string target)
    {
        Validate(source);PublicKey(key);
        if(string.IsNullOrEmpty(version)||version.Length>31||version.Any(c=>c<32||c>126))throw new InvalidDataException("Version must be 1-31 printable ASCII characters.");
        if(string.IsNullOrEmpty(target)||target.Length>16||target.Any(c=>c<32||c>126))throw new InvalidDataException("Target must be 1-16 printable ASCII characters.");
        byte[] package=new byte[512+source.Body.Length];Encoding.ASCII.GetBytes("ACAT").CopyTo(package,0);W16(package,4,1);W16(package,6,512);W32(package,8,package.Length);W32(package,12,source.Body.Length);
        Encoding.ASCII.GetBytes(target).CopyTo(package,16);Hash(source.Runtime).CopyTo(package,32);Hash(source.Body).CopyTo(package,64);Hash(Encoding.ASCII.GetBytes(Schema)).CopyTo(package,96);Encoding.ASCII.GetBytes(version).CopyTo(package,128);
        W16(package,160,source.Params);W16(package,162,source.Trees);W16(package,164,source.Ui);W16(package,166,source.Errors);
        for(int i=0;i<5;i++){W32(package,168+i*8,source.Offsets[i]);W32(package,172+i*8,source.Lengths[i]);}
        source.Body.CopyTo(package,512);
        using(ECDsaCng signer=new ECDsaCng(key)){byte[] signature=signer.SignHash(Hash(Transcript(package)));if(signature.Length!=64)throw new CryptographicException("Unexpected P-256 signature format.");signature.CopyTo(package,448);}
        if(!Verify(package,PublicKey(key)))throw new CryptographicException("Generated catalog failed signature verification.");
        return package;
    }
    private static bool Verify(byte[] package,byte[] publicKey)
    {
        if(package.Length<513||publicKey.Length!=64||!Hash(package.Skip(512).ToArray()).SequenceEqual(package.Skip(64).Take(32)))return false;
        byte[] blob=new byte[72];W32(blob,0,unchecked((int)0x31534345));W32(blob,4,32);publicKey.CopyTo(blob,8);
        using(CngKey key=CngKey.Import(blob,CngKeyBlobFormat.EccPublicBlob))using(ECDsaCng verifier=new ECDsaCng(key))return verifier.VerifyHash(Hash(Transcript(package)),package.Skip(448).Take(64).ToArray());
    }
    private static void WriteNew(string path,byte[] bytes)
    { using(FileStream stream=new FileStream(path,FileMode.CreateNew,FileAccess.Write))stream.Write(bytes,0,bytes.Length); }
    private static Source Tiny()
    {
        byte[] param={1,1,1,1,1,1,0,1,0,1,65,0,0};byte[] body=new byte[2+param.Length+4+6];W16(body,0,param.Length);param.CopyTo(body,2);W32(body,15,2);W16(body,19,4);body[22]=2;body[23]=(byte)'O';body[24]=(byte)'K';
        byte[] canonical=Encoding.ASCII.GetBytes("BVPV-RUNTIME-1").Concat(new byte[]{1,0,1,1,1,1,1,0,1,0}).ToArray();
        return new Source{Body=body,Runtime=canonical,Params=1,Trees=0,Ui=0,Errors=1,Offsets=new[]{0,15,15,15,19},Lengths=new[]{15,0,0,4,6}};
    }
}
