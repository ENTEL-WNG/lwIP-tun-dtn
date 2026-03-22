FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    build-essential \
    git \
    cmake \
    iproute2 \
    net-tools \
    libpcap-dev \
    && rm -rf /var/lib/apt/lists/*

# Clone LwIP + contrib (contains the linux tap port)
RUN git clone --depth=1 https://git.savannah.gnu.org/git/lwip.git /lwip && \
    git clone --depth=1 https://git.savannah.gnu.org/git/lwip/lwip-contrib.git /lwip-contrib

WORKDIR /app
COPY main.c .
COPY CMakeLists.txt .

RUN cmake -B build . && cmake --build build

COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

ENTRYPOINT ["/entrypoint.sh"]