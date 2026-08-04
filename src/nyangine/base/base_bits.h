/**
 * @file base_bits.h
 *
 * Convenience macros for named bits and bitmasks.
 * Bit and flag macros are supposed to be used with the bit defines only.
 * */
#pragma once

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * NAMED BITS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define BIT1  (1 << 0)
#define BIT2  (1 << 1)
#define BIT3  (1 << 2)
#define BIT4  (1 << 3)
#define BIT5  (1 << 4)
#define BIT6  (1 << 5)
#define BIT7  (1 << 6)
#define BIT8  (1 << 7)
#define BIT9  (1 << 8)
#define BIT10 (1 << 9)
#define BIT11 (1 << 10)
#define BIT12 (1 << 11)
#define BIT13 (1 << 12)
#define BIT14 (1 << 13)
#define BIT15 (1 << 14)
#define BIT16 (1 << 15)
#define BIT17 (1 << 16)
#define BIT18 (1 << 17)
#define BIT19 (1 << 18)
#define BIT20 (1 << 19)
#define BIT21 (1 << 20)
#define BIT22 (1 << 21)
#define BIT23 (1 << 22)
#define BIT24 (1 << 23)
#define BIT25 (1 << 24)
#define BIT26 (1 << 25)
#define BIT27 (1 << 26)
#define BIT28 (1 << 27)
#define BIT29 (1 << 28)
#define BIT30 (1 << 29)
#define BIT31 (1 << 30)
#define BIT32 (1U << 31)
#define BIT33 (1ULL << 32)
#define BIT34 (1ULL << 33)
#define BIT35 (1ULL << 34)
#define BIT36 (1ULL << 35)
#define BIT37 (1ULL << 36)
#define BIT38 (1ULL << 37)
#define BIT39 (1ULL << 38)
#define BIT40 (1ULL << 39)
#define BIT41 (1ULL << 40)
#define BIT42 (1ULL << 41)
#define BIT43 (1ULL << 42)
#define BIT44 (1ULL << 43)
#define BIT45 (1ULL << 44)
#define BIT46 (1ULL << 45)
#define BIT47 (1ULL << 46)
#define BIT48 (1ULL << 47)
#define BIT49 (1ULL << 48)
#define BIT50 (1ULL << 49)
#define BIT51 (1ULL << 50)
#define BIT52 (1ULL << 51)
#define BIT53 (1ULL << 52)
#define BIT54 (1ULL << 53)
#define BIT55 (1ULL << 54)
#define BIT56 (1ULL << 55)
#define BIT57 (1ULL << 56)
#define BIT58 (1ULL << 57)
#define BIT59 (1ULL << 58)
#define BIT60 (1ULL << 59)
#define BIT61 (1ULL << 60)
#define BIT62 (1ULL << 61)
#define BIT63 (1ULL << 62)
#define BIT64 (1ULL << 63)

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * NAMED BITMASKS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define BITMASK1  (0x00000001)
#define BITMASK2  (0x00000003)
#define BITMASK3  (0x00000007)
#define BITMASK4  (0x0000000F)
#define BITMASK5  (0x0000001F)
#define BITMASK6  (0x0000003F)
#define BITMASK7  (0x0000007F)
#define BITMASK8  (0x000000FF)
#define BITMASK9  (0x000001FF)
#define BITMASK10 (0x000003FF)
#define BITMASK11 (0x000007FF)
#define BITMASK12 (0x00000FFF)
#define BITMASK13 (0x00001FFF)
#define BITMASK14 (0x00003FFF)
#define BITMASK15 (0x00007FFF)
#define BITMASK16 (0x0000FFFF)
#define BITMASK17 (0x0001FFFF)
#define BITMASK18 (0x0003FFFF)
#define BITMASK19 (0x0007FFFF)
#define BITMASK20 (0x000FFFFF)
#define BITMASK21 (0x001FFFFF)
#define BITMASK22 (0x003FFFFF)
#define BITMASK23 (0x007FFFFF)
#define BITMASK24 (0x00FFFFFF)
#define BITMASK25 (0x01FFFFFF)
#define BITMASK26 (0x03FFFFFF)
#define BITMASK27 (0x07FFFFFF)
#define BITMASK28 (0x0FFFFFFF)
#define BITMASK29 (0x1FFFFFFF)
#define BITMASK30 (0x3FFFFFFF)
#define BITMASK31 (0x7FFFFFFF)
#define BITMASK32 (0xFFFFFFFFU)
#define BITMASK33 (0x00000001FFFFFFFFULL)
#define BITMASK34 (0x00000003FFFFFFFFULL)
#define BITMASK35 (0x00000007FFFFFFFFULL)
#define BITMASK36 (0x0000000FFFFFFFFFULL)
#define BITMASK37 (0x0000001FFFFFFFFFULL)
#define BITMASK38 (0x0000003FFFFFFFFFULL)
#define BITMASK39 (0x0000007FFFFFFFFFULL)
#define BITMASK40 (0x000000FFFFFFFFFFULL)
#define BITMASK41 (0x000001FFFFFFFFFFULL)
#define BITMASK42 (0x000003FFFFFFFFFFULL)
#define BITMASK43 (0x000007FFFFFFFFFFULL)
#define BITMASK44 (0x00000FFFFFFFFFFFULL)
#define BITMASK45 (0x00001FFFFFFFFFFFULL)
#define BITMASK46 (0x00003FFFFFFFFFFFULL)
#define BITMASK47 (0x00007FFFFFFFFFFFULL)
#define BITMASK48 (0x0000FFFFFFFFFFFFULL)
#define BITMASK49 (0x0001FFFFFFFFFFFFULL)
#define BITMASK50 (0x0003FFFFFFFFFFFFULL)
#define BITMASK51 (0x0007FFFFFFFFFFFFULL)
#define BITMASK52 (0x000FFFFFFFFFFFFFULL)
#define BITMASK53 (0x001FFFFFFFFFFFFFULL)
#define BITMASK54 (0x003FFFFFFFFFFFFFULL)
#define BITMASK55 (0x007FFFFFFFFFFFFFULL)
#define BITMASK56 (0x00FFFFFFFFFFFFFFULL)
#define BITMASK57 (0x01FFFFFFFFFFFFFFULL)
#define BITMASK58 (0x03FFFFFFFFFFFFFFULL)
#define BITMASK59 (0x07FFFFFFFFFFFFFFULL)
#define BITMASK60 (0x0FFFFFFFFFFFFFFFULL)
#define BITMASK61 (0x1FFFFFFFFFFFFFFFULL)
#define BITMASK62 (0x3FFFFFFFFFFFFFFFULL)
#define BITMASK63 (0x7FFFFFFFFFFFFFFFULL)
#define BITMASK64 (0xFFFFFFFFFFFFFFFFULL)

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * BIT UTILITIES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define nya_bit_set(val, bit)            ((val) |= (bit))
#define nya_bit_unset(val, bit)          ((val) &= ~(bit))
#define nya_bit_check(val, bit)          ((val) & (bit))
#define nya_bit_toggle(val, bit)         ((val) ^= (bit))
#define nya_bitmask_set(val, bitmask)    ((val) |= (bitmask))
#define nya_bitmask_unset(val, bitmask)  ((val) &= ~(bitmask))
#define nya_bitmask_check(val, bitmask)  ((val) & (bitmask))
#define nya_bitmask_toggle(val, bitmask) ((val) ^= (bitmask))
#define nya_bits_count_u32(val)          (__builtin_popcount(val))
#define nya_bits_count_u64(val)          (__builtin_popcountll(val))

/*
 * Leading and trailing zero counts. **Undefined for zero.**
 *
 * There is no bit to count from in a zero word, and the underlying builtins say so: the result is
 * undefined rather than 32 or 64. In practice it is whatever the instruction leaves behind, which
 * differs between machines and between optimisation levels, so a caller that can see zero has to
 * check first.
 *
 * Left as the bare builtins deliberately. Adding an assertion would make the trap loud, but
 * assertions are compiled into shipping builds here, and these are single instructions meant for
 * hot code. Nothing in the engine calls them yet; whoever first does should decide.
 * */
#define nya_bits_clz_u32(val)            (__builtin_clz(val))
#define nya_bits_clz_u64(val)            (__builtin_clzll(val))
#define nya_bits_ctz_u32(val)            (__builtin_ctz(val))
#define nya_bits_ctz_u64(val)            (__builtin_ctzll(val))

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * BITFLAG UTILITIES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define nya_flag_set(flags, flag)    ((flags) |= (flag))
#define nya_flag_unset(flags, flag)  ((flags) &= ~(flag))
#define nya_flag_toggle(flags, flag) ((flags) ^= (flag))
#define nya_flag_check(flags, flag)  ((flags) & (flag))
#define nya_flag_equals(flags, flag) ((flags) == (flag))
