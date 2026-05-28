if (WITH_SCRYPT_CHACHA)
    add_definitions(/DXMRIG_ALGO_SCRYPT_CHACHA)

    list(APPEND HEADERS_CRYPTO
        src/crypto/scrypt-chacha/Autotune.h
        src/crypto/scrypt-chacha/ScryptChachaCtx.h
        src/crypto/scrypt-chacha/scrypt-chacha.h
        src/crypto/scrypt-chacha/scrypt-chacha_test.h
        src/crypto/scrypt-chacha/scrypt-jane.h
    )

    list(APPEND SOURCES_CRYPTO
        src/crypto/scrypt-chacha/Autotune.cpp
        src/crypto/scrypt-chacha/ScryptChachaCtx.cpp
        src/crypto/scrypt-chacha/scrypt-chacha.cpp
        src/crypto/scrypt-chacha/scrypt-jane.c
    )

    # Self-contained golden-vector test for the scrypt-chacha CPU kernel.
    # Links only against the four source files that make up the kernel; no
    # xmrig runtime, no network, no thread machinery. Run with:
    #     make scrypt_chacha_test && ./scrypt_chacha_test
    add_executable(scrypt_chacha_test
        tests/scrypt_chacha_test.cpp
        src/crypto/scrypt-chacha/ScryptChachaCtx.cpp
        src/crypto/scrypt-chacha/scrypt-chacha.cpp
        src/crypto/scrypt-chacha/scrypt-jane.c
    )
    target_include_directories(scrypt_chacha_test PRIVATE src)
    target_compile_definitions(scrypt_chacha_test PRIVATE XMRIG_ALGO_SCRYPT_CHACHA)
else()
    remove_definitions(/DXMRIG_ALGO_SCRYPT_CHACHA)
endif()
