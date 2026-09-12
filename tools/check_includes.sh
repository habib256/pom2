#!/usr/bin/env bash
# POM2 — "declare what you use" scan over first-party sources.
#
# WHY. libc++ (AppleClang) declares many std:: facilities through OTHER
# headers; libstdc++ (the Linux CI leg) does not. A file that uses
# `std::abort` with no <cstdlib>, or `std::fill` with no <algorithm>, then
# builds green on a developer's Mac and fails the Linux job — three CI cycles
# were spent on exactly that between 2026-09-11 and 2026-09-12 (c8ac4fc,
# 6fcfa47, and the coverage job that never reached its measurement).
#
#     tools/check_includes.sh            # scan src/ and tests/
#     tools/check_includes.sh --self-test
#
# Exit 0 = clean, 1 = at least one file uses a facility whose header it does
# not include and cannot reach.
#
# WHAT IT DOES NOT FLAG. A facility reached through a PROJECT header (Memory.h
# pulling <vector>, say) is "fragile, not broken": it compiles on every
# library today, and flagging ~590 such pairs would be noise nobody acts on.
# Only the unreachable ones fail the scan.
#
# TWO FALSE-POSITIVE CLASSES ARE HANDLED, and both cost a wrong answer while
# this was being written:
#   * size_t / uintN_t are declared by MANY headers, not one. A file getting
#     size_t from <cstdint> is correct, so PROVIDERS maps such facilities to
#     the full set that satisfies them. Mapping size_t to <cstddef> alone
#     reported 35 files that were all fine.
#   * A COMMENT naming a facility is not a use. DiskImage.h documents
#     "std::thread 512 KB" and uses no thread; the matcher therefore strips
#     line and block comments, string-literal aware so http:// survives.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 2
python3 - "$@" <<'PY'
import glob, os, re, sys

NEED = {
 'cstdlib':  r'std::(abort|exit|getenv|malloc|calloc|realloc|free|qsort|strtol|strtoul|strtod|atoi|atof|system)\b',
 'cstring':  r'std::(memcpy|memset|memmove|memcmp|strlen|strcmp|strncmp|strcpy|strncpy|strchr|strstr)\b',
 'cstdio':   r'std::(printf|fprintf|snprintf|sprintf|puts|fputs|fopen|fclose|fread|fwrite|perror)\b',
 'cstdint':  r'\b(u?int(8|16|32|64)_t|uintptr_t|intptr_t)\b',
 'cmath':    r'std::(sqrt|fabs|floor|ceil|round|sin|cos|tan|pow|log|exp|hypot|isnan|isinf)\b',
 'algorithm':r'std::(sort|stable_sort|find|find_if|min_element|max_element|fill|fill_n|copy|copy_n|count|count_if|clamp|any_of|all_of|none_of|remove_if|lower_bound|upper_bound|reverse|unique|transform|equal|min|max)\b',
 'numeric':  r'std::(accumulate|iota|inner_product|partial_sum)\b',
 'vector':   r'std::vector\b',    'string':   r'std::(string|to_string|stoi|stof|stod)\b',
 'memory':   r'std::(unique_ptr|shared_ptr|make_unique|make_shared|weak_ptr)\b',
 'thread':   r'std::thread\b',    'atomic':   r'std::atomic\b',
 'mutex':    r'std::(mutex|lock_guard|unique_lock|scoped_lock|recursive_mutex)\b',
 'chrono':   r'std::chrono::',    'functional': r'std::function\b',
 'array':    r'std::array\b',     'optional': r'std::optional\b',
 'map':      r'std::(map|multimap)\b', 'set': r'std::(set|multiset)\b',
 'unordered_map': r'std::unordered_map\b',
 'filesystem':r'std::filesystem::',
 'fstream':  r'std::(ifstream|ofstream|fstream)\b',
 'sstream':  r'std::(ostringstream|istringstream|stringstream)\b',
 'iostream': r'std::(cout|cerr|cin|endl)\b',
}
NEED = {h: re.compile(p) for h, p in NEED.items()}

# Facilities several headers may legally supply (see header comment).
SHARED = {'cstddef','stddef.h','cstdint','stdint.h','cstdio','stdio.h','cstring','string.h',
          'cstdlib','stdlib.h','ctime','time.h','vector','string','array','memory','map','set'}
PROVIDERS = {'cstdint': SHARED | {'cinttypes','arpa/inet.h','netinet/in.h','windows.h'}}
def providers(h):
    if h in PROVIDERS: return PROVIDERS[h]
    return {h, h.replace('c', '', 1) + '.h'} if h.startswith('c') else {h}

def strip_comments(t):
    out=[];i=0;n=len(t)
    while i<n:
        c=t[i]
        if c in '"\'':
            q=c;out.append(c);i+=1
            while i<n:
                if t[i]=='\\': out.append(' ');i+=2;continue
                if t[i]==q: out.append(q);i+=1;break
                out.append(t[i]);i+=1
            continue
        if c=='/' and i+1<n and t[i+1]=='/':
            while i<n and t[i]!='\n': i+=1
            continue
        if c=='/' and i+1<n and t[i+1]=='*':
            i+=2
            while i+1<n and not (t[i]=='*' and t[i+1]=='/'): i+=1
            i+=2;continue
        out.append(c);i+=1
    return ''.join(out)

SYS_INC=re.compile(r'^\s*#\s*include\s*<([^>]+)>',re.M)
PRJ_INC=re.compile(r'^\s*#\s*include\s*"([^"]+)"',re.M)
def read(p):
    try: return open(p,encoding='utf-8',errors='replace').read()
    except OSError: return ''

if '--self-test' in sys.argv:
    ok=True
    def expect(name, cond):
        global ok
        print(f"  {'ok  ' if cond else 'FAIL'}  {name}")
        ok = ok and cond
    expect("a comment naming std::thread is not a use",
           not NEED['thread'].search(strip_comments("// std::thread 512 KB\nint x;")))
    expect("a real use survives comment stripping",
           bool(NEED['thread'].search(strip_comments("std::thread t;"))))
    expect("a URL in a string is not a comment",
           'http://x' in strip_comments('const char* u = "http://x";'))
    expect("size_t accepts <cstdint> as a provider", 'cstdint' in providers('cstdint'))
    expect("<algorithm> has exactly one provider", providers('algorithm') == {'algorithm'})
    print("check_includes --self-test:", "all checks passed" if ok else "FAILURES")
    sys.exit(0 if ok else 1)

hdr={}
for d in ('src','tests'):
    for p in glob.glob(f'{d}/**/*.h',recursive=True): hdr.setdefault(os.path.basename(p),p)
def reachable(path,seen=None):
    if seen is None: seen=set()
    if path in seen: return set()
    seen.add(path)
    txt=read(path); out=set(SYS_INC.findall(txt))
    for q in PRJ_INC.findall(txt):
        b=os.path.basename(q)
        if b in hdr: out|=reachable(hdr[b],seen)
    return out

bad=0; checked=0
for d in ('src','tests'):
    for p in sorted(glob.glob(f'{d}/**/*.cpp',recursive=True))+sorted(glob.glob(f'{d}/**/*.h',recursive=True)):
        txt=read(p); checked+=1
        body=strip_comments(re.sub(r'^\s*#\s*include.*$','',txt,flags=re.M))
        direct=set(SYS_INC.findall(txt)); trans=reachable(p)
        for h,pat in NEED.items():
            if not pat.search(body): continue
            alt=providers(h)
            if direct & alt or trans & alt: continue
            bad+=1
            print(f"MISSING INCLUDE  {p}: uses a facility from <{h}>, which it neither includes nor reaches")
if bad:
    print(f"\n{bad} missing include(s) over {checked} file(s).")
    print("Add the header to that file: libstdc++ will not lend what libc++ does.")
    sys.exit(1)
print(f"clean — {checked} first-party file(s), every used facility declared")
PY
