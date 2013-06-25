#ifndef __FP_MATH_H__
#define __FP_MATH_H__

#include <linux/math64.h>

#ifndef __KERNEL__
#include <stdint.h>
#define abs(x) (((x) < 0) ? -(x) : x)
#endif

// Use 64-bit because we want to track things at the nanosecond scale.
// This can lead to very large numbers.
typedef int64_t fpbuf_t;
typedef struct
{
	fpbuf_t val;
} fp_t;

#define FP_SHIFT 10
#define ROUND_BIT (FP_SHIFT - 1)

#define _fp(x) ((fp_t) {x})

#ifdef __KERNEL__
static const fp_t LITMUS_FP_ZERO = {.val = 0};
static const fp_t LITMUS_FP_ONE = {.val = (1 << FP_SHIFT)};
#endif

static inline fp_t FP(fpbuf_t x)
{
	return _fp(((fpbuf_t) x) << FP_SHIFT);
}

/* divide two integers to obtain a fixed point value  */
static inline fp_t _frac(fpbuf_t a, fpbuf_t b)
{
	return _fp(div64_s64(FP(a).val, (b)));
}

static inline fpbuf_t _point(fp_t x)
{
	return (x.val % (1 << FP_SHIFT));

}

#define fp2str(x) x.val
/*(x.val >> FP_SHIFT), (x.val % (1 << FP_SHIFT)) */
#define _FP_  "%ld/1024"

static inline fpbuf_t _floor(fp_t x)
{
	return x.val >> FP_SHIFT;
}

/* FIXME: negative rounding */
static inline fpbuf_t _round(fp_t x)
{
	return _floor(x) + ((x.val >> ROUND_BIT) & 1);
}

/* multiply two fixed point values */
static inline fp_t _mul(fp_t a, fp_t b)
{
	return _fp((a.val * b.val) >> FP_SHIFT);
}

static inline fp_t _div(fp_t a, fp_t b)
{
#if !defined(__KERNEL__) && !defined(unlikely)
#define unlikely(x) (x)
#define DO_UNDEF_UNLIKELY
#endif
	/* try not to overflow */
	if (unlikely(  a.val > (2l << ((sizeof(fpbuf_t)*8) - FP_SHIFT)) ))
		return _fp((a.val / b.val) << FP_SHIFT);
	else
		return _fp((a.val << FP_SHIFT) / b.val);
#ifdef DO_UNDEF_UNLIKELY
#undef unlikely
#undef DO_UNDEF_UNLIKELY
#endif
}

static inline fp_t _add(fp_t a, fp_t b)
{
	return _fp(a.val + b.val);
}

static inline fp_t _sub(fp_t a, fp_t b)
{
	return _fp(a.val - b.val);
}

static inline fp_t _neg(fp_t x)
{
	return _fp(-x.val);
}

static inline fp_t _abs(fp_t x)
{
	return _fp(abs(x.val));
}

/* works the same as casting float/double to integer */
static inline fpbuf_t _fp_to_integer(fp_t x)
{
	return _floor(_abs(x)) * ((x.val > 0) ? 1 : -1);
}

static inline fp_t _integer_to_fp(fpbuf_t x)
{
	return _frac(x,1);
}

static inline int _leq(fp_t a, fp_t b)
{
	return a.val <= b.val;
}

static inline int _geq(fp_t a, fp_t b)
{
	return a.val >= b.val;
}

static inline int _lt(fp_t a, fp_t b)
{
	return a.val < b.val;
}

static inline int _gt(fp_t a, fp_t b)
{
	return a.val > b.val;
}

static inline int _eq(fp_t a, fp_t b)
{
	return a.val == b.val;
}

static inline fp_t _max(fp_t a, fp_t b)
{
	if (a.val < b.val)
		return b;
	else
		return a;
}
#endif
