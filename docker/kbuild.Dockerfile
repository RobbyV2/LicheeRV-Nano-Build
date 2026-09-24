# Builder for LicheeRV-Nano-Build (U-Boot, OpenSBI, FSBL, Linux 5.10, Buildroot).
# Package list trimmed from LicheeRV-Nano-Build/.github/workflows (ubuntu-22.04).
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential autoconf automake autotools-dev ninja-build make help2man \
      libncurses-dev libncurses5-dev gawk flex bison openssl libssl-dev wget curl cmake \
      libtool-bin bc ca-certificates cpio file git locales python3 python3-dev python3-pip \
      python3-distutils python3-magic python3-pexpect python3-jinja2 rsync unzip pkg-config \
      slib squashfs-tools android-sdk-libsparse-utils jq tclsh scons device-tree-compiler \
      fakeroot texinfo chrpath xz-utils zstd liblz4-tool lz4 parted erofs-utils genext2fs \
      mtools dosfstools e2fsprogs kmod libelf-dev u-boot-tools diffstat bzip2 patch perl \
      libconfuse-dev gosu util-linux sudo python-is-python3 \
    && locale-gen en_US.UTF-8 \
    && rm -rf /var/lib/apt/lists/*

RUN wget -q -O /tmp/host-tools.tar.gz https://sophon-file.sophon.cn/sophon-prod-s3/drive/23/03/07/16/host-tools.tar.gz \
    && tar -C /usr/local -zxf /tmp/host-tools.tar.gz \
    && rm -f /tmp/host-tools.tar.gz

# kvm/merge_nanokvm_app.sh mounts the rootfs with the SDK's prebuilt host/fuse2fs.
RUN apt-get update && apt-get install -y --no-install-recommends libfuse2 fuse \
    && rm -rf /var/lib/apt/lists/*

ARG UID=1000
ARG GID=1000
RUN groupadd -g $GID build && useradd -m -u $UID -g $GID -s /bin/bash build
ENV LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8
USER build
WORKDIR /work
