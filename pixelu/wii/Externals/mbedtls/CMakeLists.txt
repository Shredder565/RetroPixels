# Mbed TLS 2.28 (Apache-2.0, the copy in Dolphin's Externals): Wii SSL, AES/SHA and
# the ECDSA signatures. Built here from its library sources as mbed TLS's own
# CMake does it: three static libraries, crypto <- x509 <- tls, with the stock
# include/mbedtls/config.h (no Everest, no zlib, no pkcs11-helper).
project("mbed TLS" C)
set(src "${THIRD}/mbedtls")

set(crypto_parts
  aes aesni arc4 aria asn1parse asn1write base64 bignum blowfish camellia ccm
  chacha20 chachapoly cipher cipher_wrap constant_time cmac ctr_drbg des dhm ecdh
  ecdsa ecjpake ecp ecp_curves entropy entropy_poll error gcm havege hkdf hmac_drbg
  md md2 md4 md5 memory_buffer_alloc mps_reader mps_trace nist_kw oid padlock pem pk
  pk_wrap pkcs12 pkcs5 pkparse pkwrite platform platform_util poly1305 psa_crypto
  psa_crypto_aead psa_crypto_cipher psa_crypto_client psa_crypto_driver_wrappers
  psa_crypto_ecp psa_crypto_hash psa_crypto_mac psa_crypto_rsa psa_crypto_se
  psa_crypto_slot_management psa_crypto_storage psa_its_file ripemd160 rsa
  rsa_internal sha1 sha256 sha512 threading timing version version_features xtea
)
set(x509_parts certs pkcs11 x509 x509_create x509_crl x509_crt x509_csr x509write_crt x509write_csr)
set(tls_parts
  debug net_sockets ssl_cache ssl_ciphersuites ssl_cli ssl_cookie ssl_msg ssl_srv
  ssl_ticket ssl_tls ssl_tls13_keys
)

set(previous "")
foreach(lib crypto x509 tls)
  set(files ${${lib}_parts})
  list(TRANSFORM files PREPEND "${src}/library/")
  list(TRANSFORM files APPEND ".c")
  if(lib STREQUAL "tls")
    set(name mbedtls)
  else()
    set(name mbed${lib})
  endif()
  add_library(${name} STATIC ${files})
  add_library(MbedTLS::${name} ALIAS ${name})
  target_include_directories(${name} PUBLIC "${src}/include/" PRIVATE "${src}/library/")
  # Sockets (net_sockets.c) need Winsock.
  target_link_libraries(${name} PUBLIC ws2_32 ${previous})
  pixelu_third_party_target(${name})
  set(previous ${name})
endforeach()
