/* libc_table.c -- libm and pure libc functions newlib provides with the same
 * ABI as bionic, mapped straight through for mods. libm: every bionic libm
 * function newlib has (generated from the NDK libm.so stub and newlib's libm.a).
 * The libc set is a reviewed list: no FILE*, struct or flag differences.
 *
 * Only the addresses matter, so every function is declared with a placeholder
 * type and no headers are included.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdint.h>
#include "so_util.h"

extern void __signbitf(void);
extern void acosh(void);
extern void acoshf(void);
extern void acoshl(void);
extern void acosl(void);
extern void asin(void);
extern void asinhl(void);
extern void asinl(void);
extern void atan(void);
extern void atan2l(void);
extern void atanh(void);
extern void atanhf(void);
extern void atanhl(void);
extern void atanl(void);
extern void cabs(void);
extern void cabsf(void);
extern void cabsl(void);
extern void cacos(void);
extern void cacosf(void);
extern void cacosh(void);
extern void cacoshf(void);
extern void carg(void);
extern void cargf(void);
extern void casin(void);
extern void casinf(void);
extern void casinh(void);
extern void casinhf(void);
extern void catan(void);
extern void catanf(void);
extern void catanh(void);
extern void catanhf(void);
extern void cbrt(void);
extern void cbrtf(void);
extern void cbrtl(void);
extern void ccos(void);
extern void ccosf(void);
extern void ccosh(void);
extern void ccoshf(void);
extern void ceil(void);
extern void ceilf(void);
extern void ceill(void);
extern void cexp(void);
extern void cexpf(void);
extern void cimag(void);
extern void cimagf(void);
extern void cimagl(void);
extern void conj(void);
extern void conjf(void);
extern void conjl(void);
extern void copysign(void);
extern void copysignf(void);
extern void copysignl(void);
extern void cosh(void);
extern void coshf(void);
extern void coshl(void);
extern void cosl(void);
extern void cproj(void);
extern void cprojf(void);
extern void creal(void);
extern void crealf(void);
extern void creall(void);
extern void csin(void);
extern void csinf(void);
extern void csinh(void);
extern void csinhf(void);
extern void csqrt(void);
extern void csqrtf(void);
extern void ctan(void);
extern void ctanf(void);
extern void ctanh(void);
extern void ctanhf(void);
extern void drem(void);
extern void dremf(void);
extern void erf(void);
extern void erfc(void);
extern void erfcf(void);
extern void erfcl(void);
extern void erff(void);
extern void erfl(void);
extern void exp2(void);
extern void exp2l(void);
extern void expl(void);
extern void expm1(void);
extern void expm1f(void);
extern void expm1l(void);
extern void fabs(void);
extern void fabsf(void);
extern void fabsl(void);
extern void fdim(void);
extern void fdimf(void);
extern void fdiml(void);
extern void feclearexcept(void);
extern void fegetenv(void);
extern void fegetexceptflag(void);
extern void fegetround(void);
extern void feholdexcept(void);
extern void feraiseexcept(void);
extern void fesetenv(void);
extern void fesetexceptflag(void);
extern void fesetround(void);
extern void fetestexcept(void);
extern void feupdateenv(void);
extern void finite(void);
extern void finitef(void);
extern void floor(void);
extern void floorf(void);
extern void floorl(void);
extern void fma(void);
extern void fmaf(void);
extern void fmal(void);
extern void fmaxf(void);
extern void fmaxl(void);
extern void fminf(void);
extern void fminl(void);
extern void fmodl(void);
extern void frexpf(void);
extern void frexpl(void);
extern void gamma(void);
extern void gamma_r(void);
extern void gammaf(void);
extern void gammaf_r(void);
extern void hypot(void);
extern void hypotf(void);
extern void hypotl(void);
extern void ilogb(void);
extern void ilogbf(void);
extern void ilogbl(void);
extern void j0(void);
extern void j0f(void);
extern void j1(void);
extern void j1f(void);
extern void jn(void);
extern void jnf(void);
extern void ldexpl(void);
extern void lgamma(void);
extern void lgamma_r(void);
extern void lgammaf(void);
extern void lgammaf_r(void);
extern void lgammal(void);
extern void lgammal_r(void);
extern void llrint(void);
extern void llrintf(void);
extern void llrintl(void);
extern void llround(void);
extern void llroundf(void);
extern void llroundl(void);
extern void log10l(void);
extern void log1p(void);
extern void log1pf(void);
extern void log1pl(void);
extern void log2(void);
extern void log2f(void);
extern void log2l(void);
extern void logb(void);
extern void logbf(void);
extern void logbl(void);
extern void logl(void);
extern void lrint(void);
extern void lrintf(void);
extern void lrintl(void);
extern void lroundf(void);
extern void lroundl(void);
extern void modff(void);
extern void modfl(void);
extern void nan(void);
extern void nanf(void);
extern void nanl(void);
extern void nearbyint(void);
extern void nearbyintf(void);
extern void nearbyintl(void);
extern void nextafter(void);
extern void nextafterf(void);
extern void nextafterl(void);
extern void nexttoward(void);
extern void nexttowardf(void);
extern void nexttowardl(void);
extern void powl(void);
extern void remainder(void);
extern void remainderf(void);
extern void remainderl(void);
extern void remquo(void);
extern void remquof(void);
extern void remquol(void);
extern void rint(void);
extern void rintf(void);
extern void rintl(void);
extern void round(void);
extern void roundf(void);
extern void roundl(void);
extern void scalb(void);
extern void scalbf(void);
extern void scalbln(void);
extern void scalblnf(void);
extern void scalblnl(void);
extern void scalbn(void);
extern void scalbnf(void);
extern void scalbnl(void);
extern void significand(void);
extern void significandf(void);
extern void sinh(void);
extern void sinhf(void);
extern void sinhl(void);
extern void sinl(void);
extern void sqrtl(void);
extern void tan(void);
extern void tanhl(void);
extern void tanl(void);
extern void tgamma(void);
extern void tgammaf(void);
extern void tgammal(void);
extern void trunc(void);
extern void truncf(void);
extern void truncl(void);
extern void y0(void);
extern void y0f(void);
extern void y1(void);
extern void y1f(void);
extern void yn(void);
extern void ynf(void);
extern void asctime_r(void);
extern void atol(void);
extern void atoll(void);
extern void div(void);
extern void drand48(void);
extern void erand48(void);
extern void ffs(void);
extern void imaxabs(void);
extern void imaxdiv(void);
extern void isblank(void);
extern void isdigit(void);
extern void isinf(void);
extern void isinff(void);
extern void isnan(void);
extern void isnanf(void);
extern void iswalnum(void);
extern void iswalpha(void);
extern void iswblank(void);
extern void iswcntrl(void);
extern void iswdigit(void);
extern void iswgraph(void);
extern void iswlower(void);
extern void iswprint(void);
extern void iswpunct(void);
extern void iswspace(void);
extern void iswupper(void);
extern void iswxdigit(void);
extern void jrand48(void);
extern void labs(void);
extern void lcong48(void);
extern void ldiv(void);
extern void llabs(void);
extern void lldiv(void);
extern void lrand48(void);
extern void mbsinit(void);
extern void mempcpy(void);
extern void mrand48(void);
extern void nrand48(void);
extern void rand_r(void);
extern void random(void);
extern void seed48(void);
extern void srand48(void);
extern void srandom(void);
extern void stpncpy(void);
extern void strsignal(void);
extern void strtoimax(void);
extern void strtoumax(void);
extern void toascii(void);
extern void tdelete(void);
extern void tdestroy(void);
extern void tfind(void);
extern void tsearch(void);
extern void twalk(void);
extern void wcpcpy(void);
extern void wcpncpy(void);
extern void wcscasecmp(void);
extern void wcscat(void);
extern void wcscspn(void);
extern void wcsdup(void);
extern void wcslcat(void);
extern void wcslcpy(void);
extern void wcsncasecmp(void);
extern void wcsncat(void);
extern void wcsnlen(void);
extern void wcspbrk(void);
extern void wcsrtombs(void);
extern void wcsspn(void);
extern void wcsstr(void);
extern void wcstoimax(void);
extern void wcstok(void);
extern void wcstoumax(void);
extern void wcswidth(void);
extern void wcwidth(void);
extern void wmempcpy(void);
extern void asprintf(void);
extern void setenv(void);
extern void unsetenv(void);
extern void putenv(void);
extern void isalnum_l(void);
extern void isalpha_l(void);
extern void isblank_l(void);
extern void iscntrl_l(void);
extern void isgraph_l(void);
extern void isprint_l(void);
extern void ispunct_l(void);
extern void isspace_l(void);
extern void iswalnum_l(void);
extern void iswctype_l(void);
extern void iswgraph_l(void);
extern void wctype_l(void);
extern void wcscasecmp_l(void);
extern void wcsncasecmp_l(void);
extern void wcstold_l(void);
extern void wcstoll_l(void);
extern void wcstoull_l(void);
extern void strcasecmp_l(void);
extern void strncasecmp_l(void);
extern void fmemopen(void);
extern void open_memstream(void);
extern void open_wmemstream(void);
extern void funopen(void);
extern void tmpfile(void);
extern void mkstemp(void);
extern void mkostemp(void);
extern void mkostemps(void);
extern void mkstemps(void);
extern void mktemp(void);
extern void tempnam(void);
extern void tmpnam(void);
extern void getopt(void);
extern void getopt_long(void);
extern void getopt_long_only(void);
extern void __fbufsize(void);
extern void __flbf(void);
extern void __fpending(void);
extern void __fpurge(void);
extern void __freadable(void);
extern void __fsetlocking(void);
extern void __fwritable(void);
extern void fpurge(void);
extern void fwide(void);
extern void vdprintf(void);
extern void dprintf(void);

DynLibFunction libc_functions[] = {
  { "__signbitf", (uintptr_t)&__signbitf },
  { "acosh", (uintptr_t)&acosh },
  { "acoshf", (uintptr_t)&acoshf },
  { "acoshl", (uintptr_t)&acoshl },
  { "acosl", (uintptr_t)&acosl },
  { "asin", (uintptr_t)&asin },
  { "asinhl", (uintptr_t)&asinhl },
  { "asinl", (uintptr_t)&asinl },
  { "atan", (uintptr_t)&atan },
  { "atan2l", (uintptr_t)&atan2l },
  { "atanh", (uintptr_t)&atanh },
  { "atanhf", (uintptr_t)&atanhf },
  { "atanhl", (uintptr_t)&atanhl },
  { "atanl", (uintptr_t)&atanl },
  { "cabs", (uintptr_t)&cabs },
  { "cabsf", (uintptr_t)&cabsf },
  { "cabsl", (uintptr_t)&cabsl },
  { "cacos", (uintptr_t)&cacos },
  { "cacosf", (uintptr_t)&cacosf },
  { "cacosh", (uintptr_t)&cacosh },
  { "cacoshf", (uintptr_t)&cacoshf },
  { "carg", (uintptr_t)&carg },
  { "cargf", (uintptr_t)&cargf },
  { "casin", (uintptr_t)&casin },
  { "casinf", (uintptr_t)&casinf },
  { "casinh", (uintptr_t)&casinh },
  { "casinhf", (uintptr_t)&casinhf },
  { "catan", (uintptr_t)&catan },
  { "catanf", (uintptr_t)&catanf },
  { "catanh", (uintptr_t)&catanh },
  { "catanhf", (uintptr_t)&catanhf },
  { "cbrt", (uintptr_t)&cbrt },
  { "cbrtf", (uintptr_t)&cbrtf },
  { "cbrtl", (uintptr_t)&cbrtl },
  { "ccos", (uintptr_t)&ccos },
  { "ccosf", (uintptr_t)&ccosf },
  { "ccosh", (uintptr_t)&ccosh },
  { "ccoshf", (uintptr_t)&ccoshf },
  { "ceil", (uintptr_t)&ceil },
  { "ceilf", (uintptr_t)&ceilf },
  { "ceill", (uintptr_t)&ceill },
  { "cexp", (uintptr_t)&cexp },
  { "cexpf", (uintptr_t)&cexpf },
  { "cimag", (uintptr_t)&cimag },
  { "cimagf", (uintptr_t)&cimagf },
  { "cimagl", (uintptr_t)&cimagl },
  { "conj", (uintptr_t)&conj },
  { "conjf", (uintptr_t)&conjf },
  { "conjl", (uintptr_t)&conjl },
  { "copysign", (uintptr_t)&copysign },
  { "copysignf", (uintptr_t)&copysignf },
  { "copysignl", (uintptr_t)&copysignl },
  { "cosh", (uintptr_t)&cosh },
  { "coshf", (uintptr_t)&coshf },
  { "coshl", (uintptr_t)&coshl },
  { "cosl", (uintptr_t)&cosl },
  { "cproj", (uintptr_t)&cproj },
  { "cprojf", (uintptr_t)&cprojf },
  { "creal", (uintptr_t)&creal },
  { "crealf", (uintptr_t)&crealf },
  { "creall", (uintptr_t)&creall },
  { "csin", (uintptr_t)&csin },
  { "csinf", (uintptr_t)&csinf },
  { "csinh", (uintptr_t)&csinh },
  { "csinhf", (uintptr_t)&csinhf },
  { "csqrt", (uintptr_t)&csqrt },
  { "csqrtf", (uintptr_t)&csqrtf },
  { "ctan", (uintptr_t)&ctan },
  { "ctanf", (uintptr_t)&ctanf },
  { "ctanh", (uintptr_t)&ctanh },
  { "ctanhf", (uintptr_t)&ctanhf },
  { "drem", (uintptr_t)&drem },
  { "dremf", (uintptr_t)&dremf },
  { "erf", (uintptr_t)&erf },
  { "erfc", (uintptr_t)&erfc },
  { "erfcf", (uintptr_t)&erfcf },
  { "erfcl", (uintptr_t)&erfcl },
  { "erff", (uintptr_t)&erff },
  { "erfl", (uintptr_t)&erfl },
  { "exp2", (uintptr_t)&exp2 },
  { "exp2l", (uintptr_t)&exp2l },
  { "expl", (uintptr_t)&expl },
  { "expm1", (uintptr_t)&expm1 },
  { "expm1f", (uintptr_t)&expm1f },
  { "expm1l", (uintptr_t)&expm1l },
  { "fabs", (uintptr_t)&fabs },
  { "fabsf", (uintptr_t)&fabsf },
  { "fabsl", (uintptr_t)&fabsl },
  { "fdim", (uintptr_t)&fdim },
  { "fdimf", (uintptr_t)&fdimf },
  { "fdiml", (uintptr_t)&fdiml },
  { "feclearexcept", (uintptr_t)&feclearexcept },
  { "fegetenv", (uintptr_t)&fegetenv },
  { "fegetexceptflag", (uintptr_t)&fegetexceptflag },
  { "fegetround", (uintptr_t)&fegetround },
  { "feholdexcept", (uintptr_t)&feholdexcept },
  { "feraiseexcept", (uintptr_t)&feraiseexcept },
  { "fesetenv", (uintptr_t)&fesetenv },
  { "fesetexceptflag", (uintptr_t)&fesetexceptflag },
  { "fesetround", (uintptr_t)&fesetround },
  { "fetestexcept", (uintptr_t)&fetestexcept },
  { "feupdateenv", (uintptr_t)&feupdateenv },
  { "finite", (uintptr_t)&finite },
  { "finitef", (uintptr_t)&finitef },
  { "floor", (uintptr_t)&floor },
  { "floorf", (uintptr_t)&floorf },
  { "floorl", (uintptr_t)&floorl },
  { "fma", (uintptr_t)&fma },
  { "fmaf", (uintptr_t)&fmaf },
  { "fmal", (uintptr_t)&fmal },
  { "fmaxf", (uintptr_t)&fmaxf },
  { "fmaxl", (uintptr_t)&fmaxl },
  { "fminf", (uintptr_t)&fminf },
  { "fminl", (uintptr_t)&fminl },
  { "fmodl", (uintptr_t)&fmodl },
  { "frexpf", (uintptr_t)&frexpf },
  { "frexpl", (uintptr_t)&frexpl },
  { "gamma", (uintptr_t)&gamma },
  { "gamma_r", (uintptr_t)&gamma_r },
  { "gammaf", (uintptr_t)&gammaf },
  { "gammaf_r", (uintptr_t)&gammaf_r },
  { "hypot", (uintptr_t)&hypot },
  { "hypotf", (uintptr_t)&hypotf },
  { "hypotl", (uintptr_t)&hypotl },
  { "ilogb", (uintptr_t)&ilogb },
  { "ilogbf", (uintptr_t)&ilogbf },
  { "ilogbl", (uintptr_t)&ilogbl },
  { "j0", (uintptr_t)&j0 },
  { "j0f", (uintptr_t)&j0f },
  { "j1", (uintptr_t)&j1 },
  { "j1f", (uintptr_t)&j1f },
  { "jn", (uintptr_t)&jn },
  { "jnf", (uintptr_t)&jnf },
  { "ldexpl", (uintptr_t)&ldexpl },
  { "lgamma", (uintptr_t)&lgamma },
  { "lgamma_r", (uintptr_t)&lgamma_r },
  { "lgammaf", (uintptr_t)&lgammaf },
  { "lgammaf_r", (uintptr_t)&lgammaf_r },
  { "lgammal", (uintptr_t)&lgammal },
  { "lgammal_r", (uintptr_t)&lgammal_r },
  { "llrint", (uintptr_t)&llrint },
  { "llrintf", (uintptr_t)&llrintf },
  { "llrintl", (uintptr_t)&llrintl },
  { "llround", (uintptr_t)&llround },
  { "llroundf", (uintptr_t)&llroundf },
  { "llroundl", (uintptr_t)&llroundl },
  { "log10l", (uintptr_t)&log10l },
  { "log1p", (uintptr_t)&log1p },
  { "log1pf", (uintptr_t)&log1pf },
  { "log1pl", (uintptr_t)&log1pl },
  { "log2", (uintptr_t)&log2 },
  { "log2f", (uintptr_t)&log2f },
  { "log2l", (uintptr_t)&log2l },
  { "logb", (uintptr_t)&logb },
  { "logbf", (uintptr_t)&logbf },
  { "logbl", (uintptr_t)&logbl },
  { "logl", (uintptr_t)&logl },
  { "lrint", (uintptr_t)&lrint },
  { "lrintf", (uintptr_t)&lrintf },
  { "lrintl", (uintptr_t)&lrintl },
  { "lroundf", (uintptr_t)&lroundf },
  { "lroundl", (uintptr_t)&lroundl },
  { "modff", (uintptr_t)&modff },
  { "modfl", (uintptr_t)&modfl },
  { "nan", (uintptr_t)&nan },
  { "nanf", (uintptr_t)&nanf },
  { "nanl", (uintptr_t)&nanl },
  { "nearbyint", (uintptr_t)&nearbyint },
  { "nearbyintf", (uintptr_t)&nearbyintf },
  { "nearbyintl", (uintptr_t)&nearbyintl },
  { "nextafter", (uintptr_t)&nextafter },
  { "nextafterf", (uintptr_t)&nextafterf },
  { "nextafterl", (uintptr_t)&nextafterl },
  { "nexttoward", (uintptr_t)&nexttoward },
  { "nexttowardf", (uintptr_t)&nexttowardf },
  { "nexttowardl", (uintptr_t)&nexttowardl },
  { "powl", (uintptr_t)&powl },
  { "remainder", (uintptr_t)&remainder },
  { "remainderf", (uintptr_t)&remainderf },
  { "remainderl", (uintptr_t)&remainderl },
  { "remquo", (uintptr_t)&remquo },
  { "remquof", (uintptr_t)&remquof },
  { "remquol", (uintptr_t)&remquol },
  { "rint", (uintptr_t)&rint },
  { "rintf", (uintptr_t)&rintf },
  { "rintl", (uintptr_t)&rintl },
  { "round", (uintptr_t)&round },
  { "roundf", (uintptr_t)&roundf },
  { "roundl", (uintptr_t)&roundl },
  { "scalb", (uintptr_t)&scalb },
  { "scalbf", (uintptr_t)&scalbf },
  { "scalbln", (uintptr_t)&scalbln },
  { "scalblnf", (uintptr_t)&scalblnf },
  { "scalblnl", (uintptr_t)&scalblnl },
  { "scalbn", (uintptr_t)&scalbn },
  { "scalbnf", (uintptr_t)&scalbnf },
  { "scalbnl", (uintptr_t)&scalbnl },
  { "significand", (uintptr_t)&significand },
  { "significandf", (uintptr_t)&significandf },
  { "sinh", (uintptr_t)&sinh },
  { "sinhf", (uintptr_t)&sinhf },
  { "sinhl", (uintptr_t)&sinhl },
  { "sinl", (uintptr_t)&sinl },
  { "sqrtl", (uintptr_t)&sqrtl },
  { "tan", (uintptr_t)&tan },
  { "tanhl", (uintptr_t)&tanhl },
  { "tanl", (uintptr_t)&tanl },
  { "tgamma", (uintptr_t)&tgamma },
  { "tgammaf", (uintptr_t)&tgammaf },
  { "tgammal", (uintptr_t)&tgammal },
  { "trunc", (uintptr_t)&trunc },
  { "truncf", (uintptr_t)&truncf },
  { "truncl", (uintptr_t)&truncl },
  { "y0", (uintptr_t)&y0 },
  { "y0f", (uintptr_t)&y0f },
  { "y1", (uintptr_t)&y1 },
  { "y1f", (uintptr_t)&y1f },
  { "yn", (uintptr_t)&yn },
  { "ynf", (uintptr_t)&ynf },
  { "asctime_r", (uintptr_t)&asctime_r },
  { "atol", (uintptr_t)&atol },
  { "atoll", (uintptr_t)&atoll },
  { "div", (uintptr_t)&div },
  { "drand48", (uintptr_t)&drand48 },
  { "erand48", (uintptr_t)&erand48 },
  { "ffs", (uintptr_t)&ffs },
  { "imaxabs", (uintptr_t)&imaxabs },
  { "imaxdiv", (uintptr_t)&imaxdiv },
  { "isblank", (uintptr_t)&isblank },
  { "isdigit", (uintptr_t)&isdigit },
  { "isinf", (uintptr_t)&isinf },
  { "isinff", (uintptr_t)&isinff },
  { "isnan", (uintptr_t)&isnan },
  { "isnanf", (uintptr_t)&isnanf },
  { "iswalnum", (uintptr_t)&iswalnum },
  { "iswalpha", (uintptr_t)&iswalpha },
  { "iswblank", (uintptr_t)&iswblank },
  { "iswcntrl", (uintptr_t)&iswcntrl },
  { "iswdigit", (uintptr_t)&iswdigit },
  { "iswgraph", (uintptr_t)&iswgraph },
  { "iswlower", (uintptr_t)&iswlower },
  { "iswprint", (uintptr_t)&iswprint },
  { "iswpunct", (uintptr_t)&iswpunct },
  { "iswspace", (uintptr_t)&iswspace },
  { "iswupper", (uintptr_t)&iswupper },
  { "iswxdigit", (uintptr_t)&iswxdigit },
  { "jrand48", (uintptr_t)&jrand48 },
  { "labs", (uintptr_t)&labs },
  { "lcong48", (uintptr_t)&lcong48 },
  { "ldiv", (uintptr_t)&ldiv },
  { "llabs", (uintptr_t)&llabs },
  { "lldiv", (uintptr_t)&lldiv },
  { "lrand48", (uintptr_t)&lrand48 },
  { "mbsinit", (uintptr_t)&mbsinit },
  { "mempcpy", (uintptr_t)&mempcpy },
  { "mrand48", (uintptr_t)&mrand48 },
  { "nrand48", (uintptr_t)&nrand48 },
  { "rand_r", (uintptr_t)&rand_r },
  { "random", (uintptr_t)&random },
  { "seed48", (uintptr_t)&seed48 },
  { "srand48", (uintptr_t)&srand48 },
  { "srandom", (uintptr_t)&srandom },
  { "stpncpy", (uintptr_t)&stpncpy },
  { "strsignal", (uintptr_t)&strsignal },
  { "strtoimax", (uintptr_t)&strtoimax },
  { "strtoumax", (uintptr_t)&strtoumax },
  { "toascii", (uintptr_t)&toascii },
  { "tdelete", (uintptr_t)&tdelete },
  { "tdestroy", (uintptr_t)&tdestroy },
  { "tfind", (uintptr_t)&tfind },
  { "tsearch", (uintptr_t)&tsearch },
  { "twalk", (uintptr_t)&twalk },
  { "wcpcpy", (uintptr_t)&wcpcpy },
  { "wcpncpy", (uintptr_t)&wcpncpy },
  { "wcscasecmp", (uintptr_t)&wcscasecmp },
  { "wcscat", (uintptr_t)&wcscat },
  { "wcscspn", (uintptr_t)&wcscspn },
  { "wcsdup", (uintptr_t)&wcsdup },
  { "wcslcat", (uintptr_t)&wcslcat },
  { "wcslcpy", (uintptr_t)&wcslcpy },
  { "wcsncasecmp", (uintptr_t)&wcsncasecmp },
  { "wcsncat", (uintptr_t)&wcsncat },
  { "wcsnlen", (uintptr_t)&wcsnlen },
  { "wcspbrk", (uintptr_t)&wcspbrk },
  { "wcsrtombs", (uintptr_t)&wcsrtombs },
  { "wcsspn", (uintptr_t)&wcsspn },
  { "wcsstr", (uintptr_t)&wcsstr },
  { "wcstoimax", (uintptr_t)&wcstoimax },
  { "wcstok", (uintptr_t)&wcstok },
  { "wcstoumax", (uintptr_t)&wcstoumax },
  { "wcswidth", (uintptr_t)&wcswidth },
  { "wcwidth", (uintptr_t)&wcwidth },
  { "wmempcpy", (uintptr_t)&wmempcpy },
  { "asprintf", (uintptr_t)&asprintf },
  { "setenv", (uintptr_t)&setenv },
  { "unsetenv", (uintptr_t)&unsetenv },
  { "putenv", (uintptr_t)&putenv },
  { "isalnum_l", (uintptr_t)&isalnum_l },
  { "isalpha_l", (uintptr_t)&isalpha_l },
  { "isblank_l", (uintptr_t)&isblank_l },
  { "iscntrl_l", (uintptr_t)&iscntrl_l },
  { "isgraph_l", (uintptr_t)&isgraph_l },
  { "isprint_l", (uintptr_t)&isprint_l },
  { "ispunct_l", (uintptr_t)&ispunct_l },
  { "isspace_l", (uintptr_t)&isspace_l },
  { "iswalnum_l", (uintptr_t)&iswalnum_l },
  { "iswctype_l", (uintptr_t)&iswctype_l },
  { "iswgraph_l", (uintptr_t)&iswgraph_l },
  { "wctype_l", (uintptr_t)&wctype_l },
  { "wcscasecmp_l", (uintptr_t)&wcscasecmp_l },
  { "wcsncasecmp_l", (uintptr_t)&wcsncasecmp_l },
  { "wcstold_l", (uintptr_t)&wcstold_l },
  { "wcstoll_l", (uintptr_t)&wcstoll_l },
  { "wcstoull_l", (uintptr_t)&wcstoull_l },
  { "strcasecmp_l", (uintptr_t)&strcasecmp_l },
  { "strncasecmp_l", (uintptr_t)&strncasecmp_l },
  { "fmemopen", (uintptr_t)&fmemopen },
  { "open_memstream", (uintptr_t)&open_memstream },
  { "open_wmemstream", (uintptr_t)&open_wmemstream },
  { "funopen", (uintptr_t)&funopen },
  { "mktemp", (uintptr_t)&mktemp },
  { "getopt", (uintptr_t)&getopt },
  { "getopt_long", (uintptr_t)&getopt_long },
  { "getopt_long_only", (uintptr_t)&getopt_long_only },
  { "__fbufsize", (uintptr_t)&__fbufsize },
  { "__flbf", (uintptr_t)&__flbf },
  { "__fpending", (uintptr_t)&__fpending },
  { "__fpurge", (uintptr_t)&__fpurge },
  { "__freadable", (uintptr_t)&__freadable },
  { "__fsetlocking", (uintptr_t)&__fsetlocking },
  { "__fwritable", (uintptr_t)&__fwritable },
  { "fpurge", (uintptr_t)&fpurge },
  { "fwide", (uintptr_t)&fwide },
  { "vdprintf", (uintptr_t)&vdprintf },
  { "dprintf", (uintptr_t)&dprintf },
};

const size_t libc_numfunctions = sizeof(libc_functions) / sizeof(*libc_functions);
