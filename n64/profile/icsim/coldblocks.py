"""coldblocks.py SRC COV FIRST LAST MINLINES: brace blocks in SRC lines
[FIRST, LAST] whose code never executed (COV from srccov.py) and that hold at
least MINLINES lines with code; outermost only. Prints 'open_line close_line
code_lines first_text'."""
import sys
src = open(sys.argv[1]).read().split("\n"); first, last, minl = int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
cov = {}
for l in open(sys.argv[2]).read().split("\n")[1:]:
    p = l.split()
    if len(p) == 3: cov[int(p[0])] = int(p[1])
def strip(l):
    out, i, q = [], 0, None
    while i < len(l):
        c = l[i]
        if q:
            if c == "\\": i += 2; continue
            if c == q: q = None
        elif c in "\"'": q = c
        elif l.startswith("//", i): break
        else: out.append(c)
        i += 1
    return "".join(out)
# brace matching over the range (block comments ignored crudely: lines starting with * or /*)
stack, blocks = [], []
incomment = False
for n in range(first, last + 1):
    l = src[n - 1]
    s = l.strip()
    if incomment:
        if "*/" in s: incomment = False
        continue
    if s.startswith("/*") and "*/" not in s: incomment = True; continue
    t = strip(l)
    for c in t:
        if c == "{": stack.append(n)
        elif c == "}" and stack:
            o = stack.pop(); blocks.append((o, n))
cands = []
for o, c in blocks:
    lines = [k for k in range(o + 1, c) if k in cov]
    if len(lines) >= minl and all(cov[k] == 0 for k in lines) and (o not in cov or cov[o] == 0 or True):
        cands.append((o, c, len(lines)))
outer = [b for b in cands if not any(a[0] < b[0] and b[1] < a[1] for a in cands)]
for o, c, k in sorted(outer):
    print(o, c, k, src[o - 1].strip()[:80])
