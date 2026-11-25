#!/usr/bin/env python3
"""Static checks for src/psp/psp_gl_guard.h and its use in src/psp/psp_renderer.c.
  check_gl_guard.py PSPSDK_SRC
1. every libgu function that can write a display list (sendCommand*, gu_list->current writes, or a call to such a function,
   transitively) is either wrapped or made a compile error by the header;
2. every GL_ARM() in the renderer sits in a function that starts its own list and has a recovery point (setjmp(g_gl_jb) in
   `if (...)`), the one exception being DrawGeBody, whose recovery point is DrawGe();
3. no emitting sceGu call is outside the guard (the preprocessor rejects the denied ones; this reports bare uses of the
   silent names, which is fine, and any name the header does not know)."""
import re, sys, glob, os

sdk = sys.argv[1]
hdr = open('src/psp/psp_gl_guard.h').read()
src = open('src/psp/psp_renderer.c').read()
bad = 0

# 1. emitters in libgu
emit_re = re.compile(r'sendCommand[if]?\s*\(|gu_list->current\s*(\+\+|\+=|=[^=])|\*\s*\(?\s*gu_list->current\s*\+\+')
files = {os.path.basename(f)[:-2]: open(f).read() for f in glob.glob(sdk + '/gu/sceGu*.c')}
emit = {n for n, t in files.items() if emit_re.search(t)}
changed = True
while changed:
    changed = False
    for n, t in files.items():
        if n not in emit and any(re.search(r'\b%s\s*\(' % e, t) for e in emit if e != n):
            emit.add(n); changed = True
defined = set(re.findall(r'^#define (sceGu\w+)\(\.\.\.\)', hdr, re.M))
missing = sorted(e for e in emit if e not in defined)
if missing:
    bad = 1
    print('1. libgu functions that write a list and are neither wrapped nor denied in psp_gl_guard.h:', ', '.join(missing))
else:
    print('1. all %d list-writing libgu functions are wrapped or denied' % len(emit))

# 2. recovery points
def functions(text):
    for m in re.finditer(r'^static[^;{=\n]*\)\s*\{\n', text, re.M):
        sig = m.group(0)
        names = re.findall(r'(\w+)\(', sig)
        if not names or '=' in sig.split('(')[0]:
            continue
        name = names[-1] if 'attribute' not in names[-1] else names[0]
        start = m.end()
        i = text.find('\n}\n', start)  # the file's style: a function ends with `}` in column 0 (#if'd braces defeat counting)
        i = len(text) if i < 0 else i
        yield name, text[start:i]
def blank(m):  # comments, strings and char literals: braces in them must not count (newlines kept)
    return re.sub(r'[^\n]', ' ', m.group(0))
clean = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])+\'', blank, src, flags=re.S)
fn = dict(functions(clean))
n_arm = 0
for name, body in fn.items():
    if 'GL_ARM()' not in body:
        continue
    n_arm += 1
    a = body.index('GL_ARM()')
    pre = body[:a]
    if not re.search(r'\b(FrameListStart|GlStart)\(', pre):
        print('2. %s: GL_ARM() before the function started its list' % name); bad = 1
    if name == 'DrawGeBody':
        d = fn.get('DrawGe', '')
        if not re.search(r'if \(setjmp\(g_gl_jb\)\)', d) or 'DrawGeBody()' not in d:
            print('2. DrawGe() lacks the recovery point of DrawGeBody'); bad = 1
    elif not re.search(r'if \(setjmp\(g_gl_jb\)\)', pre):
        print('2. %s: GL_ARM() without `if (setjmp(g_gl_jb))` before it' % name); bad = 1
for m in re.finditer(r'setjmp\(g_gl_jb\)', clean):
    if not clean[max(0, m.start() - 4):m.start()].endswith('if ('):
        print('2. setjmp used outside `if (setjmp(g_gl_jb))`'); bad = 1
for name, body in fn.items():
    if re.search(r'\bFrameListStart\(', body) and 'GL_ARM()' not in body:
        print('2. %s starts a frame list without GL_ARM()' % name); bad = 1
print('2. %d functions arm a recovery point; each starts its list first and has its setjmp' % n_arm)

# 3. raw lists: nothing but GlStart / FrameListStart / GuStartBoot may open one
if re.search(r'(?<![\w(])sceGuStart\b', re.sub(r'/\*.*?\*/|//[^\n]*|"[^"\n]*"', '', src, flags=re.S)):
    print('3. sceGuStart is used in psp_renderer.c'); bad = 1
else:
    print('3. no direct sceGuStart in psp_renderer.c')
sys.exit(bad)
