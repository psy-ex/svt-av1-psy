FROM buildpack-deps:bookworm AS builder

WORKDIR /app

RUN apt-get update && apt-get install -y \
    cmake \
    yasm \
    && rm -rf /var/lib/apt/lists/*

ENV LDFLAGS="-static -static-libgcc"

RUN git clone --branch v2.3.0 --depth 1 https://github.com/gianni-rosato/svt-av1-psy && \
    cd ./svt-av1-psy/Build/linux && \
    ./build.sh static native enable-lto release



FROM alpine AS release

COPY --from=builder /app/svt-av1-psy/Bin/Release/SvtAv1EncApp /app/

ENTRYPOINT ["/app/SvtAv1EncApp"]
CMD [ "--help" ]
