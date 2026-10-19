import re,sys
src=open(sys.argv[1],encoding='utf-8-sig').read()
out=[]
for line in src.splitlines():
    s=line.strip()
    if s.startswith('//'): continue
    m=re.match(r'^[\w\s\*]*?__stdcall\s+((?:e[A-Z]\w*)|(?:strm2_\d+))\s*\((.*)\)\s*;\s*//\s*(\d{3})',s)
    if not m: continue
    name,params,ordn=m.groups()
    if name.endswith('_lib'): continue
    # count top-level commas
    depth=0;n=0;cur=''
    parts=[]
    for ch in params:
        if ch in '([': depth+=1
        elif ch in ')]': depth-=1
        if ch==',' and depth==0: parts.append(cur);cur=''
        else: cur+=ch
    parts.append(cur)
    parts=[p.strip() for p in parts if p.strip() and p.strip()!='void']
    out.append((int(ordn),name,4*len(parts)))
out.sort()
seen=set()
print('LIBRARY Strm2.dll\nEXPORTS')
for o,n,b in out:
    if o in seen: sys.stderr.write(f'dup ordinal {o} {n}\n'); continue
    seen.add(o); print(f'    {n}@{b} @{o} NONAME')
sys.stderr.write(f'{len(seen)} funcs\n')
