# syntax=docker/dockerfile:1
FROM debian:bookworm-slim AS builder
ARG THREADS=4
ARG BUILD_COMMIT=unknown
# Optional public CA mount for managed environments; never baked into the image.
RUN --mount=type=secret,id=system_ca \
    if [ -f /run/secrets/system_ca ]; then export SSL_CERT_FILE=/run/secrets/system_ca; fi; \
    apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates git g++ make cmake pkg-config python3 curl libcurl4-openssl-dev \
      libpcre2-dev && rm -rf /var/lib/apt/lists/*
COPY scripts/dependencies.sh /deps/dependencies.sh
WORKDIR /deps
RUN --mount=type=secret,id=system_ca \
    if [ -f /run/secrets/system_ca ]; then export GIT_SSL_CAINFO=/run/secrets/system_ca; fi; \
    bash -euc '. ./dependencies.sh; \
      checkout_dependency https://github.com/ftk/quickjspp quickjspp "$QUICKJSPP_REF"; \
      checkout_dependency https://github.com/PerMalmberg/libcron libcron "$LIBCRON_REF"; \
      checkout_dependency https://github.com/ToruNiina/toml11 toml11 "$TOML11_REF"; \
      checkout_dependency https://github.com/jbeder/yaml-cpp yaml-cpp "$YAML_CPP_REF"; \
      checkout_dependency https://github.com/Tencent/rapidjson rapidjson "$RAPIDJSON_REF"'
RUN cmake -S quickjspp -B quickjspp -DCMAKE_BUILD_TYPE=Release && \
    cmake --build quickjspp --target quickjs -j "$THREADS" && \
    install -d /usr/local/lib/quickjs /usr/local/include/quickjs && \
    install -m644 quickjspp/quickjs/libquickjs.a /usr/local/lib/quickjs/ && \
    install -m644 quickjspp/quickjs/quickjs.h quickjspp/quickjs/quickjs-libc.h /usr/local/include/quickjs/ && \
    install -m644 quickjspp/quickjspp.hpp /usr/local/include/ && \
    cmake -S libcron -B libcron -DCMAKE_BUILD_TYPE=Release && \
    cmake --build libcron --target libcron install -j "$THREADS" && \
    cmake -S toml11 -B toml11 -DCMAKE_CXX_STANDARD=11 && \
    cmake --install toml11 && \
    cmake -S yaml-cpp -B yaml-cpp -DCMAKE_BUILD_TYPE=Release -DYAML_BUILD_SHARED_LIBS=OFF -DYAML_CPP_BUILD_TESTS=OFF -DYAML_CPP_BUILD_TOOLS=OFF && \
    cmake --build yaml-cpp -j "$THREADS" && cmake --install yaml-cpp && \
    cmake -S rapidjson -B rapidjson -DRAPIDJSON_BUILD_DOC=OFF -DRAPIDJSON_BUILD_EXAMPLES=OFF -DRAPIDJSON_BUILD_TESTS=OFF && \
    cmake --install rapidjson
# Source and bundled rules come exclusively from this checkout.
COPY . /source
RUN cmake -S /source -B /build -DCMAKE_BUILD_TYPE=Release -DSUBCONVERTER_BUILD_COMMIT="$BUILD_COMMIT" && \
    cmake --build /build -j "$THREADS" && \
    python3 /source/tests/integration.py --binary /build/subconverter --base /source/base && \
    dpkg-query -W > /build/build-dependencies.txt

FROM debian:bookworm-slim
ARG BUILD_COMMIT=unknown
LABEL org.opencontainers.image.revision="$BUILD_COMMIT"
RUN --mount=type=secret,id=system_ca \
    if [ -f /run/secrets/system_ca ]; then export SSL_CERT_FILE=/run/secrets/system_ca; fi; \
    apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates libcurl4 libpcre2-8-0 && rm -rf /var/lib/apt/lists/*
COPY --from=builder /build/subconverter /opt/subconverter/subconverter
COPY --from=builder /build/build-dependencies.txt /opt/subconverter/build-dependencies.txt
COPY --from=builder /source/base /opt/subconverter/
RUN sed -i 's/listen = "127.0.0.1"/listen = "0.0.0.0"/' /opt/subconverter/pref.example.toml
WORKDIR /opt/subconverter
EXPOSE 25500
CMD ["./subconverter"]
