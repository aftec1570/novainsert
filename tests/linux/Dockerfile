# NovaInsert Linux x86_64 test rig: build deps, REAPER headless under Xvfb.
# Build:  docker build --platform linux/amd64 -t novainsert-linux tests/linux
FROM --platform=linux/amd64 ubuntu:22.04

ARG REAPER_URL=https://www.reaper.fm/files/7.x/reaper785_linux_x86_64.tar.xz
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl gpg sudo xz-utils \
    && curl -fsSL https://apt.kitware.com/keys/kitware-archive-latest.asc | gpg --dearmor -o /usr/share/keyrings/kitware.gpg \
    && echo "deb [signed-by=/usr/share/keyrings/kitware.gpg] https://apt.kitware.com/ubuntu/ jammy main" > /etc/apt/sources.list.d/kitware.list \
    && apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git ninja-build python3 xvfb xauth alsa-utils kmod unzip jackd2 \
        libasound2-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxext-dev \
        libfreetype6-dev libfontconfig1-dev libgl1-mesa-dev \
        libgtk-3-0 libxss1 \
    && rm -rf /var/lib/apt/lists/*

RUN mkdir -p /opt/reaper && curl -fsSL "$REAPER_URL" | tar -xJ -C /opt/reaper --strip-components=1 \
    && ls -l /opt/reaper/REAPER/reaper

# pluginval (tier A), pinned release
ARG PLUGINVAL_URL=https://github.com/Tracktion/pluginval/releases/download/v1.0.4/pluginval_Linux.zip
RUN curl -fsSL -o /tmp/pv.zip "$PLUGINVAL_URL" && unzip -q /tmp/pv.zip -d /opt/pluginval && rm /tmp/pv.zip \
    && chmod +x /opt/pluginval/pluginval && ln -s /opt/pluginval/pluginval /usr/local/bin/pluginval

# f3_6 desktop agent on Linux (tests/f3/apollo/x11_ui.py): close/inspect REAPER's render window under Xvfb
RUN apt-get update && apt-get install -y --no-install-recommends xdotool x11-utils && rm -rf /var/lib/apt/lists/*

RUN useradd -m -s /bin/bash ni && echo "ni ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/ni
USER ni
ENV HOME=/home/ni
WORKDIR /work
