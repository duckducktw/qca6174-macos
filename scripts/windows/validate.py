#!/usr/bin/env python3
"""Structural and ad-hoc hash verification; not a replacement for a macOS boot test."""
from pathlib import Path
import struct,json,hashlib,plistlib,argparse,re
parser=argparse.ArgumentParser(description="Validate a signed Sequoia native kext offline.")
parser.add_argument('bundle',type=Path)
parser.add_argument('--kernel-collection',type=Path)
args=parser.parse_args()
bundle=args.bundle.resolve();out=bundle.parent
path=bundle/'Contents/MacOS/Ath10kMac';data=path.read_bytes()
magic,cpu,subtype,kind,ncmds,szcmds,flags,reserved=struct.unpack_from('<8I',data)
assert magic==0xfeedfacf and cpu==0x1000007 and kind==11,(hex(magic),hex(cpu),kind)
assert 32+szcmds<=len(data)
segments=[];sections=[];cmds=[];pos=32;table=None;signature=None;uuid=None;dysym=None
for _ in range(ncmds):
    cmd,size=struct.unpack_from('<II',data,pos);assert size>=8 and pos+size<=32+szcmds
    cmds.append(hex(cmd))
    if cmd==0x19:
        name,vm,vmsize,off,length,maxp,initp,nsects,sflags=struct.unpack_from('<16sQQQQIIII',data,pos+8)
        assert off+length<=len(data);segments.append((vm,off,length,name.rstrip(b'\0').decode()))
        for i in range(nsects):
            p=pos+72+i*80;sn,sg,addr,ss,so,al,ro,nr,sf=struct.unpack_from('<16s16sQQIIIII',data,p)
            assert ro+nr*8<=len(data)
            sections.append(dict(name=sn.rstrip(b'\0').decode(),address=addr,size=ss,offset=so,relocation_offset=ro,relocations=nr,flags=sf))
    elif cmd==2:table=struct.unpack_from('<IIII',data,pos+8)
    elif cmd==0xb:dysym=struct.unpack_from('<18I',data,pos+8)
    elif cmd==0x1d:signature=struct.unpack_from('<II',data,pos+8)
    elif cmd==0x1b:uuid=data[pos+8:pos+24].hex()
    assert cmd not in [0xc,0x80000018,0x8000001c],f'Unexpected userspace dylib/rpath load command {hex(cmd)}'
    pos+=size
so,count,strings,length=table;assert so+16*count<=len(data) and strings+length<=len(data)
syms={};undefined=[];weak=[]
for i in range(count):
    ix,t,section,desc,value=struct.unpack_from('<IBBHQ',data,so+i*16)
    if not ix or t&0xe0:continue
    name=data[strings+ix:data.index(b'\0',strings+ix)].decode(errors='replace')
    if (t&0xe)==0 and t&1 and not value:
        undefined.append(name)
        if desc&0x40:weak.append(name)
    elif (t&0xe)==0xe:syms[name]=value
assert not weak,weak
bad=[n for n in undefined if any(x in n for x in ['chkstk','stack_chk','___cxa_atexit','___gxx_personality'])]
assert not bad,bad
def kernel_symbols(path):
    kc=path.read_bytes();entries=[];pos=32
    assert struct.unpack_from('<I',kc,12)[0]==12, 'Expected MH_FILESET'
    for _ in range(struct.unpack_from('<I',kc,16)[0]):
        cmd,size=struct.unpack_from('<II',kc,pos)
        if cmd==0x80000035:entries.append(struct.unpack_from('<Q',kc,pos+16)[0])
        pos+=size
    names=set()
    for entry in entries:
        pos=entry+32
        for _ in range(struct.unpack_from('<I',kc,entry+16)[0]):
            cmd,size=struct.unpack_from('<II',kc,pos)
            if cmd==2:
                so,count,strings,length=struct.unpack_from('<IIII',kc,pos+8)
                for i in range(count):
                    ix,t,section,desc,value=struct.unpack_from('<IBBHQ',kc,so+i*16)
                    if ix and value and (t&0xe)==0xe:
                        names.add(kc[strings+ix:kc.index(b'\0',strings+ix)].decode(errors='replace'))
            pos+=size
    return names

if args.kernel_collection:
    missing=sorted(set(undefined)-kernel_symbols(args.kernel_collection))
    assert not missing,missing
for s in sections:
    for j in range(s['relocations']):
        addr,info=struct.unpack_from('<iI',data,s['relocation_offset']+j*8)
        if info&(1<<27):assert (info&0xffffff)<count
assert dysym is not None
external_relocations,local_relocations=dysym[15],dysym[17]
assert external_relocations>0
for ro,nr,external in [(dysym[14],external_relocations,True),(dysym[16],local_relocations,False)]:
    assert ro+nr*8<=len(data)
    for j in range(nr):
        address,info=struct.unpack_from('<iI',data,ro+j*8)
        assert address>=0 and any(v<=address<v+s for v,o,s,n in segments)
        assert bool(info&(1<<27))==external
        if external:assert (info&0xffffff)<count
assert signature is not None
sigoff,sigsize=signature;assert sigoff+sigsize<=len(data)
smagic,slength,scount=struct.unpack_from('>III',data,sigoff);assert smagic==0xfade0cc0
directories=[];blobs={}
for i in range(scount):
    stype,blob=struct.unpack_from('>II',data,sigoff+12+i*8)
    boff=sigoff+blob;bmagic,blength=struct.unpack_from('>II',data,boff)
    assert blob+blength<=slength
    blobs[stype]=data[boff:boff+blength]
    if stype==0 or 0x1000<=stype<0x1005:directories.append(boff)
assert directories
verified=[]
for cd in directories:
    cmagic,clength,version,cflags,hoff,ident,nspecial,ncode,limit=struct.unpack_from('>9I',data,cd)
    hashsize,hashtype,platform,page=struct.unpack_from('4B',data,cd+36)
    assert cmagic==0xfade0c02 and cflags&2 and hashtype in [1,2]
    digest=hashlib.sha1 if hashtype==1 else hashlib.sha256
    assert hashsize==digest().digest_size and limit==sigoff
    assert ncode==(limit+(1<<page)-1)//(1<<page)
    for i in range(ncode):
        expect=data[cd+hoff+i*hashsize:cd+hoff+(i+1)*hashsize]
        assert digest(data[i*(1<<page):min((i+1)*(1<<page),limit)]).digest()==expect
    special={1:(bundle/'Contents/Info.plist').read_bytes(),2:blobs[2],3:(bundle/'Contents/_CodeSignature/CodeResources').read_bytes()}
    for slot,content in special.items():
        assert nspecial>=slot
        assert data[cd+hoff-slot*hashsize:cd+hoff-(slot-1)*hashsize]==digest(content).digest()
    verified.append(dict(algorithm='sha1' if hashtype==1 else 'sha256',pages=ncode))
assert any(x['algorithm']=='sha256' for x in verified)
assert not any(data[sigoff+slength:sigoff+sigsize])
info=plistlib.loads((bundle/'Contents/Info.plist').read_bytes());p=info['IOKitPersonalities']['Lunnen-QCA9377']
q=info['IOKitPersonalities'].get('LiteOn-QCA6174')
assert q and q['IOPCIMatch']=='0x003e168c' and q['IOPCISecondaryMatch']=='0x080711ad'
assert q['Ath10kDiagnosticStage']==p['Ath10kDiagnosticStage']
assert p['Ath10kDiagnosticStage'] in range(4) and not p['Ath10kProbeOnly']
assert info['OSBundleLibraries']['com.apple.iokit.IO80211Family']
getters={}
for name in ['__ZNK15Ath10kPCIDevice11getWorkLoopEv','__ZNK15Ath10kPCIDevice12getWorkQueueEv']:
    addr=syms[name];off=next(o+addr-v for v,o,s,n in segments if v<=addr<v+s)
    # These two leaf getters load their field with mov rax,[rdi+disp32].
    match=re.search(rb'\x48\x8b\x87(.{4})',data[off:off+24],re.S)
    assert match, 'Unexpected getter encoding: '+name
    getters[name]={'field':struct.unpack('<i',match[1])[0]}
assert len({v['field'] for v in getters.values()})==2
report=dict(result='PASS_OFFLINE_NOT_BOOT_TESTED',architecture='x86_64',filetype='MH_KEXT_BUNDLE',uuid=uuid,bytes=len(data),sha256=hashlib.sha256(data).hexdigest(),load_commands=cmds,defined_symbols=len(syms),undefined_symbols=len(undefined),undefined_available_in_kernel_collection=True if args.kernel_collection else None,weak_imports=weak,stack_runtime_imports=bad,section_relocations=sum(s['relocations'] for s in sections),external_relocations=external_relocations,local_relocations=local_relocations,adhoc_code_directories_verified=verified,signed_info_and_resources_verified=True,diagnostic_personality_checked=True,stage=p['Ath10kDiagnosticStage'],getters=getters,efi_changed=False)
(out/'validation.json').write_text(json.dumps(report,indent=2));(out/'undefined-symbols.txt').write_text('\n'.join(sorted(undefined)))
print(json.dumps(report,indent=2))
