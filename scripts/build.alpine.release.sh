#!/bin/bash
set -xeuo pipefail
source scripts/dependencies.sh

# Git must trust this known mounted checkout, and source identification must succeed.
git config --global --add safe.directory "$(pwd -P)"
BUILD_COMMIT=$(git rev-parse --verify HEAD)
test -n "$BUILD_COMMIT"

apk add gcc g++ build-base linux-headers cmake make autoconf automake libtool python3 py3-pip
apk add mbedtls-dev mbedtls-static zlib-dev zlib-static pcre2-dev pcre2-static brotli-dev brotli-static zstd-dev zstd-static libpsl-dev libpsl-static

checkout_dependency https://github.com/curl/curl curl "$CURL_REF"
cd curl
cmake -DCURL_USE_MBEDTLS=ON -DHTTP_ONLY=ON -DBUILD_TESTING=OFF -DBUILD_SHARED_LIBS=OFF -DCURL_USE_LIBSSH2=OFF -DBUILD_CURL_EXE=OFF -DCURL_ZSTD=ON -DCURL_BROTLI=ON -DUSE_NGHTTP2=OFF -DUSE_LIBIDN2=OFF -DCURL_USE_LIBPSL=OFF . > /dev/null
make install -j2 > /dev/null
cd ..

checkout_dependency https://github.com/Tencent/rapidjson rapidjson "$RAPIDJSON_REF"
cmake -S rapidjson -B rapidjson -DRAPIDJSON_BUILD_DOC=OFF -DRAPIDJSON_BUILD_EXAMPLES=OFF -DRAPIDJSON_BUILD_TESTS=OFF
cmake --install rapidjson

checkout_dependency https://github.com/jbeder/yaml-cpp yaml-cpp "$YAML_CPP_REF"
cd yaml-cpp
cmake -DCMAKE_BUILD_TYPE=Release -DYAML_CPP_BUILD_TESTS=OFF -DYAML_CPP_BUILD_TOOLS=OFF . > /dev/null
make install -j3 > /dev/null
cd ..

checkout_dependency https://github.com/ftk/quickjspp quickjspp "$QUICKJSPP_REF"
cd quickjspp
cmake -DCMAKE_BUILD_TYPE=Release .
make quickjs -j3 > /dev/null
install -d /usr/lib/quickjs/
install -m644 quickjs/libquickjs.a /usr/lib/quickjs/
install -d /usr/include/quickjs/
install -m644 quickjs/quickjs.h quickjs/quickjs-libc.h /usr/include/quickjs/
install -m644 quickjspp.hpp /usr/include/
cd ..

checkout_dependency https://github.com/PerMalmberg/libcron libcron "$LIBCRON_REF"
cd libcron
git submodule update --init
cmake -DCMAKE_BUILD_TYPE=Release .
make libcron install -j3
cd ..

checkout_dependency https://github.com/ToruNiina/toml11 toml11 "$TOML11_REF"
cd toml11
cmake -DCMAKE_CXX_STANDARD=11 .
make install -j4
cd ..

export PKG_CONFIG_PATH=/usr/lib64/pkgconfig
cmake -DCMAKE_BUILD_TYPE=Release . -DSUBCONVERTER_BUILD_COMMIT="$BUILD_COMMIT"
make -j3
rm subconverter
# shellcheck disable=SC2046
g++ -o base/subconverter $(find CMakeFiles/subconverter.dir/src/ -name "*.o")  -static -lpcre2-8 -lyaml-cpp -L/usr/lib64 -lcurl -lmbedtls -lmbedcrypto -lmbedx509 -lz -lbrotlidec -lbrotlicommon -lzstd -l:quickjs/libquickjs.a -llibcron -O3 -s

# Bundled rules are kept from this source checkout for traceable releases.

cd base
chmod +rx subconverter
chmod +r ./*
cd ..
python3 tests/integration.py --binary base/subconverter --base base
python3 tests/check_build.py --binary base/subconverter --commit "$BUILD_COMMIT"
mv base subconverter
