/*
 * OpenSSL key utilities.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef SSL_KEY_HXX
#define SSL_KEY_HXX

#include "Unique.hxx"

#include "util/Compiler.h"

#include <openssl/ossl_typ.h>

template<typename T> struct ConstBuffer;

UniqueEVP_PKEY
GenerateRsaKey();

/**
 * Decode a private key encoded with DER.  It is a wrapper for
 * d2i_AutoPrivateKey().
 *
 * Throws SslError on error.
 */
UniqueEVP_PKEY
DecodeDerKey(ConstBuffer<void> der);

gcc_pure
bool
MatchModulus(EVP_PKEY &key1, EVP_PKEY &key2);

gcc_pure
bool
MatchModulus(X509 &cert, EVP_PKEY &key);

#endif