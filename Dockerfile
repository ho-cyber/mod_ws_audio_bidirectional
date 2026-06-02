# ============================================================
# Stage 1: Extract mod_audio_stream from the pre-built debian package
# ============================================================
FROM debian:12-slim AS extractor

RUN apt-get update && apt-get install -y wget && rm -rf /var/lib/apt/lists/*

WORKDIR /tmp

# Download the pre-built Debian 12 package from GitHub Releases
RUN wget -O mod-audio-stream.deb https://github.com/amigniter/mod_audio_stream/releases/download/v1.0.3/mod-audio-stream_1.0.3_amd64.deb

# Extract the debian package contents
RUN dpkg-deb -x mod-audio-stream.deb extracted

# ============================================================
# Stage 2: Clean final image — only copy the compiled .so
# ============================================================
FROM drachtio/drachtio-freeswitch-mrf:latest

# Install runtime dependencies (mod_audio_stream requires libevent and libevent_pthreads)
RUN apt-get update && apt-get install -y \
    libevent-2.1-7 \
    libevent-pthreads-2.1-7 \
    && rm -rf /var/lib/apt/lists/*

# Drop the compiled module in
COPY --from=extractor \
    /tmp/extracted/usr/lib/freeswitch/mod/mod_audio_stream.so \
    /usr/local/freeswitch/mod/mod_audio_stream.so

# Patch modules.conf.xml to load the new module on startup.
# The file lives at the standard FreeSWITCH autoload path.
RUN sed -i \
    's|<load module="mod_audio_fork"/>|<load module="mod_audio_fork"/>\n    <load module="mod_audio_stream"/>|' \
    /usr/local/freeswitch/conf/autoload_configs/modules.conf.xml \
    || echo "<load module=\"mod_audio_stream\"/>" \
       >> /usr/local/freeswitch/conf/autoload_configs/modules.conf.xml
