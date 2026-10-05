#!/bin/bash
set -xeuo pipefail
source scripts/dependencies.sh

checkout_dependency https://github.com/curl/curl curl "$CURL_REF"
cd curl
cmake -DCMAKE_BUILD_TYPE=Release -DHTTP_ONLY=ON -DCURL_USE_SCHANNEL=ON -DBUILD_SHARED_LIBS=OFF -DBUILD_CURL_EXE=OFF -DCMAKE_INSTALL_PREFIX="$MINGW_PREFIX" -G "Unix Makefiles" -DCURL_USE_LIBSSH2=OFF -DUSE_WIN32_IDN=ON -DCURL_USE_LIBPSL=OFF -DCURL_ZSTD=ON -DCURL_BROTLI=ON -DUSE_NGHTTP2=OFF .
make install -j4
cd ..

checkout_dependency https://github.com/jbeder/yaml-cpp yaml-cpp "$YAML_CPP_REF"
cd yaml-cpp
cmake -DCMAKE_BUILD_TYPE=Release -DYAML_CPP_BUILD_TESTS=OFF -DYAML_CPP_BUILD_TOOLS=OFF -DCMAKE_INSTALL_PREFIX="$MINGW_PREFIX" -G "Unix Makefiles" .
make install -j4
cd ..

checkout_dependency https://github.com/ftk/quickjspp quickjspp "$QUICKJSPP_REF"
cd quickjspp
patch quickjs/quickjs-libc.c -i ../scripts/patches/0001-quickjs-libc-add-realpath-for-Windows.patch
cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release .
make quickjs -j4
install -d "$MINGW_PREFIX/lib/quickjs/"
install -m644 quickjs/libquickjs.a "$MINGW_PREFIX/lib/quickjs/"
install -d "$MINGW_PREFIX/include/quickjs"
install -m644 quickjs/quickjs.h quickjs/quickjs-libc.h "$MINGW_PREFIX/include/quickjs/"
install -m644 quickjspp.hpp "$MINGW_PREFIX/include/"
cd ..

checkout_dependency https://github.com/PerMalmberg/libcron libcron "$LIBCRON_REF"
cd libcron
git submodule update --init
cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$MINGW_PREFIX" .
make libcron install -j4
cd ..

checkout_dependency https://github.com/Tencent/rapidjson rapidjson "$RAPIDJSON_REF"
cd rapidjson
cmake -DRAPIDJSON_BUILD_DOC=OFF -DRAPIDJSON_BUILD_EXAMPLES=OFF -DRAPIDJSON_BUILD_TESTS=OFF -DCMAKE_INSTALL_PREFIX="$MINGW_PREFIX" -G "Unix Makefiles" .
make install -j4
cd ..

checkout_dependency https://github.com/ToruNiina/toml11 toml11 "$TOML11_REF"
cd toml11
cmake -DCMAKE_INSTALL_PREFIX="$MINGW_PREFIX" -G "Unix Makefiles" -DCMAKE_CXX_STANDARD=11 .
make install -j4
cd ..

rm -f C:/Strawberry/perl/bin/pkg-config C:/Strawberry/perl/bin/pkg-config.bat
cmake -DCMAKE_BUILD_TYPE=Release -G "Unix Makefiles" . -DSUBCONVERTER_BUILD_COMMIT="$(git rev-parse HEAD)"
make -j4
rm subconverter.exe
# shellcheck disable=SC2046
g++ $(find CMakeFiles/subconverter.dir/src -name "*.obj") curl/lib/libcurl.a -o base/subconverter.exe -static -lpcre2-8 -l:quickjs/libquickjs.a -llibcron -lyaml-cpp -lbrotlidec -lbrotlicommon -lzstd -lz -liphlpapi -lsecur32 -lcrypt32 -lbcrypt -lws2_32 -lwsock32 -s

# Bundled rules are kept from this source checkout for traceable releases.

python tests/integration.py --binary base/subconverter.exe --base base
python tests/check_build.py --binary base/subconverter.exe --commit "$(git rev-parse HEAD)"
mv base subconverter
