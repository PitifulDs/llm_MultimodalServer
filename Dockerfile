FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=Asia/Shanghai

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    curl \
    git \
    gdb \
    libbsd-dev \
    libboost-all-dev \
    libgoogle-glog-dev \
    libssl-dev \
    libzmq3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . /app

RUN chmod +x /app/docker/build/install/eventpp/install_eventpp.sh \
    /app/docker/build/install/simdjson/install_simdjson.sh \
    && /app/docker/build/install/eventpp/install_eventpp.sh \
    && /app/docker/build/install/simdjson/install_simdjson.sh \
    && cmake -S /app/serving -B /app/serving/build \
    && cmake --build /app/serving/build -j"$(nproc)"

EXPOSE 8080
ENV HTTP_PORT=8080

CMD ["/app/serving/build/http/serving_http_server", "8080"]
