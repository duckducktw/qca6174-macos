import sys
from capstone import *
from capstone.x86 import *
d = open("/home/user/.hermes/cache/scratch/a2.bin","rb").read()
base = 0x8afa
md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = True
ins = list(md.disasm(d, base))
idx = {i.address: k for k, i in enumerate(ins)}
sites = [i.address for i in ins if "fffff0c1" in i.op_str]
for s in sites:
    k = idx[s]
    print("=== -3903 at", format(s, "x"))
    for j in range(max(0, k-6), k+2):
        i = ins[j]
        print("   ", format(i.address, "x"), i.mnemonic.ljust(7), i.op_str)