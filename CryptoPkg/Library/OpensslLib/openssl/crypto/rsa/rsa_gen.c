/*
 * Copyright 1995-2018 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the OpenSSL license (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

/*
 * NB: these functions have been "upgraded", the deprecated versions (which
 * are compatibility wrappers using these functions) are in rsa_depr.c. -
 * Geoff
 */

/* for secure_getenv */
#define _GNU_SOURCE
#include <stdio.h>
#include <time.h>
#include "internal/cryptlib.h"
#include <openssl/bn.h>
#include "rsa_locl.h"
#ifdef OPENSSL_FIPS
# include <openssl/fips.h>
# include "internal/fips_int.h"

int fips_check_rsa(RSA *rsa)
{
    const unsigned char tbs[] = "RSA Pairwise Check Data";
    unsigned char *ctbuf = NULL, *ptbuf = NULL;
    int len, ret = 0;
    EVP_PKEY *pk;

    if ((pk = EVP_PKEY_new()) == NULL)
        goto err;

    EVP_PKEY_set1_RSA(pk, rsa);

    /* Perform pairwise consistency signature test */
    if (!fips_pkey_signature_test(pk, tbs, -1,
                                  NULL, 0, EVP_sha1(),
                                  EVP_MD_CTX_FLAG_PAD_PKCS1, NULL)
        || !fips_pkey_signature_test(pk, tbs, -1, NULL, 0, EVP_sha1(),
                                     EVP_MD_CTX_FLAG_PAD_X931, NULL)
        || !fips_pkey_signature_test(pk, tbs, -1, NULL, 0, EVP_sha1(),
                                     EVP_MD_CTX_FLAG_PAD_PSS, NULL))
        goto err;
    /* Now perform pairwise consistency encrypt/decrypt test */
    ctbuf = OPENSSL_malloc(RSA_size(rsa));
    if (!ctbuf)
        goto err;

    len =
        RSA_public_encrypt(sizeof(tbs) - 1, tbs, ctbuf, rsa,
                           RSA_PKCS1_PADDING);
    if (len <= 0)
        goto err;
    /* Check ciphertext doesn't match plaintext */
    if ((len == (sizeof(tbs) - 1)) && !memcmp(tbs, ctbuf, len))
        goto err;
    ptbuf = OPENSSL_malloc(RSA_size(rsa));

    if (!ptbuf)
        goto err;
    len = RSA_private_decrypt(len, ctbuf, ptbuf, rsa, RSA_PKCS1_PADDING);
    if (len != (sizeof(tbs) - 1))
        goto err;
    if (memcmp(ptbuf, tbs, len))
        goto err;

    ret = 1;

    if (!ptbuf)
        goto err;

 err:
    if (ret == 0) {
        fips_set_selftest_fail();
        FIPSerr(FIPS_F_FIPS_CHECK_RSA, FIPS_R_PAIRWISE_TEST_FAILED);
    }

    if (ctbuf)
        OPENSSL_free(ctbuf);
    if (ptbuf)
        OPENSSL_free(ptbuf);
    if (pk)
        EVP_PKEY_free(pk);

    return ret;
}

static int fips_rsa_builtin_keygen(RSA *rsa, int bits, BIGNUM *e_value,
                                   BN_GENCB *cb);
#endif

static int rsa_builtin_keygen(RSA *rsa, int bits, BIGNUM *e_value,
                              BN_GENCB *cb);

/*
 * NB: this wrapper would normally be placed in rsa_lib.c and the static
 * implementation would probably be in rsa_eay.c. Nonetheless, is kept here
 * so that we don't introduce a new linker dependency. Eg. any application
 * that wasn't previously linking object code related to key-generation won't
 * have to now just because key-generation is part of RSA_METHOD.
 */
int RSA_generate_key_ex(RSA *rsa, int bits, BIGNUM *e_value, BN_GENCB *cb)
{
#ifdef OPENSSL_FIPS
    if (FIPS_mode() && !(rsa->meth->flags & RSA_FLAG_FIPS_METHOD)
        && !(rsa->flags & RSA_FLAG_NON_FIPS_ALLOW)) {
        RSAerr(RSA_F_RSA_GENERATE_KEY_EX, RSA_R_NON_FIPS_RSA_METHOD);
        return 0;
    }
#endif
    if (rsa->meth->rsa_keygen)
        return rsa->meth->rsa_keygen(rsa, bits, e_value, cb);
    return rsa_builtin_keygen(rsa, bits, e_value, cb);
}

#ifdef OPENSSL_FIPS
static int fips_rsa_builtin_keygen(RSA *rsa, int bits, BIGNUM *e_value,
                                   BN_GENCB *cb)
{
    BIGNUM *r0 = NULL, *r1 = NULL, *r2 = NULL, *r3 = NULL, *tmp;
    BN_CTX *ctx = NULL;
    int ok = -1;
    int i;
    int n = 0;
    int test = 0;
    int pbits = bits / 2;
    unsigned long error = 0;

    if (FIPS_selftest_failed()) {
        FIPSerr(FIPS_F_FIPS_RSA_BUILTIN_KEYGEN, FIPS_R_FIPS_SELFTEST_FAILED);
        return 0;
    }

    if (bits < OPENSSL_RSA_FIPS_MIN_MODULUS_BITS
        || (secure_getenv("OPENSSL_ENFORCE_MODULUS_BITS") && bits < 2048)) {
        FIPSerr(FIPS_F_FIPS_RSA_BUILTIN_KEYGEN, FIPS_R_KEY_TOO_SHORT);
        return 0;
    }
    if ((pbits & 0xFF) != 0) {
        FIPSerr(FIPS_F_FIPS_RSA_BUILTIN_KEYGEN, FIPS_R_INVALID_KEY_LENGTH);
        return 0;
    }

    ctx = BN_CTX_new();
    if (ctx == NULL)
        goto err;
    BN_CTX_start(ctx);
    r0 = BN_CTX_get(ctx);
    r1 = BN_CTX_get(ctx);
    r2 = BN_CTX_get(ctx);
    r3 = BN_CTX_get(ctx);

    if (r3 == NULL)
        goto err;

    /* We need the RSA components non-NULL */
    if (!rsa->n && ((rsa->n = BN_new()) == NULL))
        goto err;
    if (!rsa->d && ((rsa->d = BN_secure_new()) == NULL))
        goto err;
    if (!rsa->e && ((rsa->e = BN_new()) == NULL))
        goto err;
    if (!rsa->p && ((rsa->p = BN_secure_new()) == NULL))
        goto err;
    if (!rsa->q && ((rsa->q = BN_secure_new()) == NULL))
        goto err;
    if (!rsa->dmp1 && ((rsa->dmp1 = BN_secure_new()) == NULL))
        goto err;
    if (!rsa->dmq1 && ((rsa->dmq1 = BN_secure_new()) == NULL))
        goto err;
    if (!rsa->iqmp && ((rsa->iqmp = BN_secure_new()) == NULL))
        goto err;

    if (!BN_set_word(r0, RSA_F4))
        goto err;
    if (BN_cmp(e_value, r0) < 0 || BN_num_bits(e_value) > 256) {
        ok = 0;                 /* we set our own err */
        RSAerr(RSA_F_FIPS_RSA_BUILTIN_KEYGEN, RSA_R_BAD_E_VALUE);
        goto err;
    }

    /* prepare approximate minimum p and q */
    if (!BN_set_word(r0, 0xB504F334))
        goto err;
    if (!BN_lshift(r0, r0, pbits - 32))
        goto err;

    /* prepare minimum p and q difference */
    if (!BN_one(r3))
        goto err;
    if (!BN_lshift(r3, r3, pbits - 100))
        goto err;

    BN_copy(rsa->e, e_value);

    if (!BN_is_zero(rsa->p) && !BN_is_zero(rsa->q))
        test = 1;

    BN_set_flags(r0, BN_FLG_CONSTTIME);
    BN_set_flags(r1, BN_FLG_CONSTTIME);
    BN_set_flags(r2, BN_FLG_CONSTTIME);
    BN_set_flags(rsa->p, BN_FLG_CONSTTIME);
    BN_set_flags(rsa->q, BN_FLG_CONSTTIME);

 retry:
    /* generate p and q */
    for (i = 0; i < 5 * pbits; i++) {
 ploop:
        if (!test)
            if (!BN_rand(rsa->p, pbits, 0, 1))
                goto err;
        if (BN_cmp(rsa->p, r0) < 0) {
            if (test)
                goto err;
            goto ploop;
        }

        if (!BN_sub(r2, rsa->p, BN_value_one()))
            goto err;
        ERR_set_mark();
        if (BN_mod_inverse(r1, r2, rsa->e, ctx) != NULL) {
            /* GCD == 1 since inverse exists */
            int r;
            r = BN_is_prime_fasttest_ex(rsa->p, pbits > 1024 ? 4 : 5, ctx, 0,
                                        cb);
            if (r == -1 || (test && r <= 0))
                goto err;
            if (r > 0)
                break;
        } else {
            error = ERR_peek_last_error();
            if (ERR_GET_LIB(error) == ERR_LIB_BN
                && ERR_GET_REASON(error) == BN_R_NO_INVERSE) {
                /* GCD != 1 */
                ERR_pop_to_mark();
            } else {
                goto err;
            }
        }
        if (!BN_GENCB_call(cb, 2, n++))
            goto err;
    }

    if (!BN_GENCB_call(cb, 3, 0))
        goto err;

    if (i >= 5 * pbits)
        /* prime not found */
        goto err;

    for (i = 0; i < 5 * pbits; i++) {
 qloop:
        if (!test)
            if (!BN_rand(rsa->q, pbits, 0, 1))
                goto err;
        if (BN_cmp(rsa->q, r0) < 0) {
            if (test)
                goto err;
            goto qloop;
        }
        if (!BN_sub(r2, rsa->q, rsa->p))
            goto err;
        if (BN_ucmp(r2, r3) <= 0) {
            if (test)
                goto err;
            goto qloop;
        }

        if (!BN_sub(r2, rsa->q, BN_value_one()))
            goto err;
        ERR_set_mark();
        if (BN_mod_inverse(r1, r2, rsa->e, ctx) != NULL) {
            /* GCD == 1 since inverse exists */
            int r;
            r = BN_is_prime_fasttest_ex(rsa->q, pbits > 1024 ? 4 : 5, ctx, 0,
                                        cb);
            if (r == -1 || (test && r <= 0))
                goto err;
            if (r > 0)
                break;
        } else {
            error = ERR_peek_last_error();
            if (ERR_GET_LIB(error) == ERR_LIB_BN
                && ERR_GET_REASON(error) == BN_R_NO_INVERSE) {
                /* GCD != 1 */
                ERR_pop_to_mark();
            } else {
                goto err;
            }
        }
        if (!BN_GENCB_call(cb, 2, n++))
            goto err;
    }

    if (!BN_GENCB_call(cb, 3, 1))
        goto err;

    if (i >= 5 * pbits)
        /* prime not found */
        goto err;

    if (test) {
        /* do not try to calculate the remaining key values */
        BN_clear(rsa->n);
        ok = 1;
        goto err;
    }

    if (BN_cmp(rsa->p, rsa->q) < 0) {
        tmp = rsa->p;
        rsa->p = rsa->q;
        rsa->q = tmp;
    }

    /* calculate n */
    if (!BN_mul(rsa->n, rsa->p, rsa->q, ctx))
        goto err;

    /* calculate d */
    if (!BN_sub(r1, rsa->p, BN_value_one()))
        goto err;               /* p-1 */
    if (!BN_sub(r2, rsa->q, BN_value_one()))
        goto err;               /* q-1 */

    /* note that computing gcd is not safe to timing attacks */
    if (!BN_gcd(r0, r1, r2, ctx))
        goto err;

    {
        if (!BN_div(r0, NULL, r1, r0, ctx))
            goto err;

        if (!BN_mul(r0, r0, r2, ctx)) /* lcm(p-1, q-1) */
            goto err;

        if (!BN_mod_inverse(rsa->d, rsa->e, r0, ctx)) /* d */
            goto err;
    }

    if (BN_num_bits(rsa->d) < pbits)
        goto retry;             /* d is too small */

    {
        BIGNUM *d = BN_new();

        if (d == NULL)
            goto err;
        BN_with_flags(d, rsa->d, BN_FLG_CONSTTIME);

        if (/* calculate d mod (p-1) */
            !BN_mod(rsa->dmp1, d, r1, ctx)
            /* calculate d mod (q-1) */
            || !BN_mod(rsa->dmq1, d, r2, ctx)) {
            BN_free(d);
            goto err;
        }
        /* We MUST free d before any further use of rsa->d */
        BN_free(d);
    }

    /* calculate inverse of q mod p */
    if (!BN_mod_inverse(rsa->iqmp, rsa->q, rsa->p, ctx))
        goto err;

    if (!fips_check_rsa(rsa))
        goto err;

    ok = 1;
 err:
    if (ok == -1) {
        RSAerr(RSA_F_FIPS_RSA_BUILTIN_KEYGEN, ERR_LIB_BN);
        ok = 0;
    }
    if (ctx != NULL) {
        BN_CTX_end(ctx);
        BN_CTX_free(ctx);
    }

    return ok;
}
#endif

static int rsa_builtin_keygen(RSA *rsa, int bits, BIGNUM *e_value,
                              BN_GENCB *cb)
{
    BIGNUM *r0 = NULL, *r1 = NULL, *r2 = NULL, *r3 = NULL, *tmp;
    int bitsp, bitsq, ok = -1, n = 0;
    BN_CTX *ctx = NULL;
    unsigned long error = 0;

    /*
     * When generating ridiculously small keys, we can get stuck
     * continually regenerating the same prime values.
     */
    if (bits < 16) {
        ok = 0;             /* we set our own err */
        RSAerr(RSA_F_RSA_BUILTIN_KEYGEN, RSA_R_KEY_SIZE_TOO_SMALL);
        goto err;
    }

    ctx = BN_CTX_new();
    if (ctx == NULL)
        goto err;
    BN_CTX_start(ctx);
    r0 = BN_CTX_get(ctx);
    r1 = BN_CTX_get(ctx);
    r2 = BN_CTX_get(ctx);
    r3 = BN_CTX_get(ctx);
    if (r3 == NULL)
        goto err;

    bitsp = (bits + 1) / 2;
    bitsq = bits - bitsp;

    /* We need the RSA components non-NULL */
    if (!rsa->n && ((rsa->n = BN_new()) == NULL))
        goto err;
    if (!rsa->d && ((rsa->d = BN_secure_new()) == NULL))
        goto err;
    if (!rsa->e && ((rsa->e = BN_new()) == NULL))
        goto err;
    if (!rsa->p && ((rsa->p = BN_secure_new()) == NULL))
        goto err;
    if (!rsa->q && ((rsa->q = BN_secure_new()) == NULL))
        goto err;
    if (!rsa->dmp1 && ((rsa->dmp1 = BN_secure_new()) == NULL))
        goto err;
    if (!rsa->dmq1 && ((rsa->dmq1 = BN_secure_new()) == NULL))
        goto err;
    if (!rsa->iqmp && ((rsa->iqmp = BN_secure_new()) == NULL))
        goto err;

    if (BN_copy(rsa->e, e_value) == NULL)
        goto err;

    BN_set_flags(r2, BN_FLG_CONSTTIME);
    /* generate p and q */
    for (;;) {
        if (!BN_generate_prime_ex(rsa->p, bitsp, 0, NULL, NULL, cb))
            goto err;
        if (!BN_sub(r2, rsa->p, BN_value_one()))
            goto err;
        ERR_set_mark();
        if (BN_mod_inverse(r1, r2, rsa->e, ctx) != NULL) {
            /* GCD == 1 since inverse exists */
            break;
        }
        error = ERR_peek_last_error();
        if (ERR_GET_LIB(error) == ERR_LIB_BN
            && ERR_GET_REASON(error) == BN_R_NO_INVERSE) {
            /* GCD != 1 */
            ERR_pop_to_mark();
        } else {
            goto err;
        }
        if (!BN_GENCB_call(cb, 2, n++))
            goto err;
    }
    if (!BN_GENCB_call(cb, 3, 0))
        goto err;
    for (;;) {
        do {
            if (!BN_generate_prime_ex(rsa->q, bitsq, 0, NULL, NULL, cb))
                goto err;
        } while (BN_cmp(rsa->p, rsa->q) == 0);
        if (!BN_sub(r2, rsa->q, BN_value_one()))
            goto err;
        ERR_set_mark();
        if (BN_mod_inverse(r1, r2, rsa->e, ctx) != NULL) {
            /* GCD == 1 since inverse exists */
            break;
        }
        error = ERR_peek_last_error();
        if (ERR_GET_LIB(error) == ERR_LIB_BN
            && ERR_GET_REASON(error) == BN_R_NO_INVERSE) {
            /* GCD != 1 */
            ERR_pop_to_mark();
        } else {
            goto err;
        }
        if (!BN_GENCB_call(cb, 2, n++))
            goto err;
    }
    if (!BN_GENCB_call(cb, 3, 1))
        goto err;
    if (BN_cmp(rsa->p, rsa->q) < 0) {
        tmp = rsa->p;
        rsa->p = rsa->q;
        rsa->q = tmp;
    }

    /* calculate n */
    if (!BN_mul(rsa->n, rsa->p, rsa->q, ctx))
        goto err;

    /* calculate d */
    if (!BN_sub(r1, rsa->p, BN_value_one()))
        goto err;               /* p-1 */
    if (!BN_sub(r2, rsa->q, BN_value_one()))
        goto err;               /* q-1 */
    if (!BN_mul(r0, r1, r2, ctx))
        goto err;               /* (p-1)(q-1) */
    {
        BIGNUM *pr0 = BN_new();

        if (pr0 == NULL)
            goto err;
        BN_with_flags(pr0, r0, BN_FLG_CONSTTIME);
        if (!BN_mod_inverse(rsa->d, rsa->e, pr0, ctx)) {
            BN_free(pr0);
            goto err;               /* d */
        }
        /* We MUST free pr0 before any further use of r0 */
        BN_free(pr0);
    }

    {
        BIGNUM *d = BN_new();

        if (d == NULL)
            goto err;
        BN_with_flags(d, rsa->d, BN_FLG_CONSTTIME);

        if (   /* calculate d mod (p-1) */
               !BN_mod(rsa->dmp1, d, r1, ctx)
               /* calculate d mod (q-1) */
            || !BN_mod(rsa->dmq1, d, r2, ctx)) {
            BN_free(d);
            goto err;
        }
        /* We MUST free d before any further use of rsa->d */
        BN_free(d);
    }

    {
        BIGNUM *p = BN_new();

        if (p == NULL)
            goto err;
        BN_with_flags(p, rsa->p, BN_FLG_CONSTTIME);

        /* calculate inverse of q mod p */
        if (!BN_mod_inverse(rsa->iqmp, rsa->q, p, ctx)) {
            BN_free(p);
            goto err;
        }
        /* We MUST free p before any further use of rsa->p */
        BN_free(p);
    }

    ok = 1;
 err:
    if (ok == -1) {
        RSAerr(RSA_F_RSA_BUILTIN_KEYGEN, ERR_LIB_BN);
        ok = 0;
    }
    if (ctx != NULL)
        BN_CTX_end(ctx);
    BN_CTX_free(ctx);

    return ok;
}
