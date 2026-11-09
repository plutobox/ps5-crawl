/* C library functions libc++ needs that the native title runtime lacks and
   ps5-opengl's cts_runtime_shims.c (linked alongside) does not define. Both
   use that file's single "C" locale rune table. */

#include <runetype.h>
#include <stddef.h>
#include <xlocale.h>

_RuneLocale *__runes_for_locale(locale_t locale, int *limit);
unsigned long ___runetype_l(__ct_rune_t value, locale_t locale);

unsigned long ___runetype(__ct_rune_t value)
{
    return ___runetype_l(value, NULL);
}

const _RuneLocale *__getCurrentRuneLocale(void)
{
    return __runes_for_locale(NULL, NULL);
}
