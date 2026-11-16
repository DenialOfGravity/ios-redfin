# glibc 2.43+ declares C11 once_flag/call_once in <stdlib.h>, clashing with Mesa 25.0's src/c11/threads.h (host build only).
# From hakios-xnu-kernel tools/air/mk-host.sh (MIT). Usage: python3 -I glibc-call-once.py MESA_SRC
import sys
root = sys.argv[1]
p = root + '/src/c11/threads.h'
s = open(p).read()
old = "typedef pthread_once_t  once_flag;\n#  define ONCE_FLAG_INIT PTHREAD_ONCE_INIT\n"
new = ("#  if defined(__GLIBC__) && __GLIBC_USE(ISOC23) && !defined(__cplusplus)\n"
       "#    define C11_LIBC_HAS_CALL_ONCE 1\n"
       "#  else\n"
       "typedef pthread_once_t  once_flag;\n"
       "#    define ONCE_FLAG_INIT PTHREAD_ONCE_INIT\n"
       "#  endif\n")
if old in s:
    open(p, 'w').write(s.replace(old, new))
p = root + '/src/c11/impl/threads_posix.c'
s = open(p).read()
old = "call_once(once_flag *flag, void (*func)(void))\n{\n    pthread_once(flag, func);\n}\n"
if old in s and 'C11_LIBC_HAS_CALL_ONCE' not in s:
    i = s.index(old)
    j = s.rfind('\n\n', 0, i)
    s = (s[:j] + "\n\n#ifndef C11_LIBC_HAS_CALL_ONCE" + s[j:i + len(old)] +
         "#endif\n" + s[i + len(old):])
    open(p, 'w').write(s)
