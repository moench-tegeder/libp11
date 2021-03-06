/* libp11, a simple layer on to of PKCS#11 API
 * Copyright (C) 2005 Olaf Kirch <okir@lst.de>
 * Copyright (C) 2016 Michał Trojnara <Michal.Trojnara@stunnel.org>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 */

/*
 * This file implements the handling of RSA keys stored on a
 * PKCS11 token
 */

#include <config.h>
#include <string.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include "libp11-int.h"

/*
 * Get RSA key material
 */
static EVP_PKEY *pkcs11_get_evp_key_rsa(PKCS11_KEY * key)
{
	EVP_PKEY *pk;
	CK_BBOOL sensitive, extractable;
	RSA *rsa;

	pk = EVP_PKEY_new();
	if (pk == NULL)
		return NULL;

	rsa = RSA_new();
	if (rsa == NULL) {
		EVP_PKEY_free(pk);
		return NULL;
	}
	EVP_PKEY_set1_RSA(pk, rsa); /* Also increments the rsa ref count */

	if (key_getattr(key, CKA_SENSITIVE, &sensitive, sizeof(sensitive))
			|| key_getattr(key, CKA_EXTRACTABLE, &extractable, sizeof(extractable))) {
		EVP_PKEY_free(pk);
		RSA_free(rsa);
		return NULL;
	}

	if (key_getattr_bn(key, CKA_MODULUS, &rsa->n) ||
			key_getattr_bn(key, CKA_PUBLIC_EXPONENT, &rsa->e)) {
		EVP_PKEY_free(pk);
		RSA_free(rsa);
		return NULL;
	}

	if (BN_is_zero(rsa->e)) {
		PKCS11_TOKEN_private * tpriv = PRIVTOKEN(KEY2TOKEN(key));
		int ki;
		BIGNUM* pubmod = NULL;
		for(ki = 0; ki < tpriv->pub.num; ki++) {
			PKCS11_KEY* pubkey = &tpriv->pub.keys[ki];

			if (key_getattr_bn(pubkey, CKA_MODULUS, &pubmod)) {
				continue;
			}
			if (BN_cmp(rsa->n, pubmod)) { // Modulus not same -- this public from another key
				continue;
			}

			// If modulus are same -- we found required key, extract public exponent from it
			if (key_getattr_bn(pubkey, CKA_PUBLIC_EXPONENT, &rsa->e)) {
				continue;
			}
			if (BN_is_zero(rsa->e)) {
				continue;
			}
			break;
		}
		if (pubmod!=NULL)
			BN_free(pubmod);
	}

	/* If the key is not extractable, create a key object
	 * that will use the card's functions to sign & decrypt */
	if (sensitive || !extractable) {
		RSA_set_method(rsa, PKCS11_get_rsa_method());
	} else if (key->isPrivate) {
		/* TODO: Extract the RSA private key */
		/* In the meantime lets use the card anyway */
		RSA_set_method(rsa, PKCS11_get_rsa_method());
	}

	rsa->flags |= RSA_FLAG_SIGN_VER;
	RSA_set_app_data(rsa, key);
	RSA_free(rsa); /* drops our reference to it */
	return pk;
}

int PKCS11_get_key_modulus(PKCS11_KEY * key, BIGNUM **bn)
{
	if (pkcs11_getattr_bn(KEY2TOKEN(key), PRIVKEY(key)->object,
			CKA_MODULUS, bn))
		return 0;
	return 1;
}

int PKCS11_get_key_exponent(PKCS11_KEY * key, BIGNUM **bn)
{
	if (pkcs11_getattr_bn(KEY2TOKEN(key), PRIVKEY(key)->object,
			CKA_PUBLIC_EXPONENT, bn))
		return 0;
	return 1;
}

int PKCS11_get_key_size(const PKCS11_KEY * key)
{
	BIGNUM *n = NULL;
	int numbytes = 0;
	if (key_getattr_bn(key, CKA_MODULUS, &n))
		return 0;
	numbytes = BN_num_bytes(n);
	BN_free(n);
	return numbytes;
}

static int pkcs11_rsa_decrypt(int flen, const unsigned char *from,
		unsigned char *to, RSA * rsa, int padding)
{

	return PKCS11_private_decrypt(flen, from, to, (PKCS11_KEY *) RSA_get_app_data(rsa), padding);
}

static int pkcs11_rsa_encrypt(int flen, const unsigned char *from,
		unsigned char *to, RSA * rsa, int padding)
{
	return PKCS11_private_encrypt(flen, from, to, (PKCS11_KEY *) RSA_get_app_data(rsa), padding);
}

static int pkcs11_rsa_sign(int type, const unsigned char *m, unsigned int m_len,
		unsigned char *sigret, unsigned int *siglen, const RSA * rsa)
{
	
	return PKCS11_sign(type, m, m_len, sigret, siglen, (PKCS11_KEY *) RSA_get_app_data(rsa));
}

/* Lousy hack alert. If RSA_verify detects that the key has the
 * RSA_FLAG_SIGN_VER flags set, it will assume that verification
 * is implemented externally as well.
 * We work around this by temporarily cleaning the flag, and
 * calling RSA_verify once more.
 */
static int
pkcs11_rsa_verify(int type, const unsigned char *m, unsigned int m_len,
		const unsigned char *signature, unsigned int siglen, const RSA * rsa)
{
	RSA *r = (RSA *) rsa;	/* Ugly hack to get rid of compiler warning */
	int res;

	if (r->flags & RSA_FLAG_SIGN_VER) {
		r->flags &= ~RSA_FLAG_SIGN_VER;
		res = RSA_verify(type, m, m_len, signature, siglen, r);
		r->flags |= RSA_FLAG_SIGN_VER;
	} else {
		PKCS11err(PKCS11_F_PKCS11_RSA_VERIFY, PKCS11_NOT_SUPPORTED);
		res = 0;
	}
	return res;
}

/*
 * Overload the default OpenSSL methods for RSA
 */
RSA_METHOD *PKCS11_get_rsa_method(void)
{
	static RSA_METHOD ops;

	if (!ops.rsa_priv_enc) {
		ops = *RSA_get_default_method();
		ops.rsa_priv_enc = pkcs11_rsa_encrypt;
		ops.rsa_priv_dec = pkcs11_rsa_decrypt;
		ops.rsa_sign = pkcs11_rsa_sign;
		ops.rsa_verify = pkcs11_rsa_verify;
	}
	return &ops;
}

PKCS11_KEY_ops pkcs11_rsa_ops = {
	EVP_PKEY_RSA,
	pkcs11_get_evp_key_rsa
};
