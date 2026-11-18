# Builds the Lichii DLSS module. The base is the oldest Ubuntu LTS whose
# toolchain still builds it (glibc 2.35), so the module loads on any distro
# the Lichii Linux release supports; the image is pinned by digest. The
# NVIDIA NGX SDK (proprietary) is fetched inside the build stage from
# NVIDIA's DLSS-tos repository and checked against pinned SHA-256 sums; it
# never leaves the build stage, and NVIDIA's licence terms for its object
# code ship beside the module as NVIDIA-LICENSE.txt.
ARG UBUNTU_DIGEST=sha256:45e9ddc6895604674618a409ca9f73d301da8fdb75fe330e8e9a27b3b219e4ad
FROM ubuntu:22.04@${UBUNTU_DIGEST} AS build
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates cmake curl g++ libvulkan-dev make \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /ngx-sdk
COPY ngx_files.sha256 ./
RUN set -eu \
 && base="https://raw.githubusercontent.com/NVIDIA/DLSS/9981c6350dc58c0ce21f024fd65e31e851c31689" \
 && grep -v '^#' ngx_files.sha256 | while read -r sha path; do \
        case "$path" in \
            lib/*) repo="lib/Linux_x86_64/$(basename "$path")" ;; \
            *) repo="$path" ;; \
        esac; \
        mkdir -p "$(dirname "$path")"; \
        echo "fetching $repo"; \
        curl -fsSL --retry 3 -o "$path" "$base/$repo"; \
        echo "$sha  $path" | sha256sum -c -; \
    done

WORKDIR /src
COPY . .
RUN cmake -B build -DNGX_SDK_DIR=/ngx-sdk \
 && cmake --build build --parallel 2

FROM ubuntu:22.04@${UBUNTU_DIGEST} AS artifacts
COPY --from=build /src/build/lichii-dlss-module.so /artifacts/lichii-dlss-module.so
COPY --from=build /ngx-sdk/LICENSE.txt /artifacts/NVIDIA-LICENSE.txt
COPY LICENSE /artifacts/LICENSE
