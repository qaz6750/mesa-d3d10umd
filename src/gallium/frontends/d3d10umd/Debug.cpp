#include "Debug.h"

#include <stdarg.h>
#include <stdio.h>

extern "C" void QcdxWddmLog(const char *text);


#if MESA_DEBUG

unsigned st_debug = 0;

static const
struct debug_named_value st_debug_flags[] = {
   {"oldtexops", ST_DEBUG_OLD_TEX_OPS, "oldtexops"},
   {"tgsi", ST_DEBUG_TGSI, "tgsi"},
   DEBUG_NAMED_VALUE_END
};
void
st_debug_parse(void)
{
   st_debug = debug_get_flags_option("ST_DEBUG", st_debug_flags, st_debug);
}

#endif


void
DebugPrintf(const char *format, ...)
{
    char buf[4096];

    va_list ap;
    va_start(ap, format);
    vsnprintf(buf, sizeof buf, format, ap);
    va_end(ap);

    OutputDebugStringA(buf);
    QcdxWddmLog(buf);   /* qcdxkm */
}


void
AssertFail(const char *expr,
           const char *file,
           unsigned line,
           const char *function)
{
   DebugPrintf("%s:%u:%s: Assertion `%s' failed.\n", file, line, function, expr);
#if defined(__GNUC__)
   __asm("int3");
#elif defined(_MSC_VER)
   __debugbreak();
#else
   DebugBreak();
#endif
}
