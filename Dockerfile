FROM ubuntu:latest AS builder
USER root
WORKDIR /src
RUN apt-get update
RUN apt-get -y install gcc g++ g++-mingw-w64 git make nasm python3 uuid-dev

RUN git clone --depth=1 https://github.com/tianocore/edk2
WORKDIR /src/edk2
RUN git submodule update --init --depth=1
RUN make -C /src/edk2/BaseTools/Source/C

RUN git clone https://github.com/utoni/EfiGuard ./EfiGuardPkg
WORKDIR /src/edk2/EfiGuardPkg
RUN git submodule update --init --recursive --depth=1
RUN make all
RUN mkdir /out
RUN make install DESTDIR=/out
RUN ls -lha /out
