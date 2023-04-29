#!/usr/bin/env bash

set -e

TMP_PATH="/tmp"
DEST_PATH="output"

mkdir -p "${DEST_PATH}"

#if [ -f ../arpl/PLATFORMS ]; then
#  cp ../arpl/PLATFORMS PLATFORMS
#else
#  curl -sLO "https://github.com/fbelavenuto/arpl/raw/main/PLATFORMS"
#fi

function compileLkm() {
  PLATFORM=$1
  KVER=$2
  TOOLKIT_VER=$3
  OUT_PATH="${TMP_PATH}/${PLATFORM}-${KVER}"
  mkdir -p "${OUT_PATH}"
  sudo chmod 1777 "${OUT_PATH}"
  docker run -u 1000 --rm -t -v "${OUT_PATH}":/output -v "${PWD}":/input \
    fbelavenuto/syno-compiler:${TOOLKIT_VER} compile-lkm ${PLATFORM}
  mv "${OUT_PATH}/redpill-dev.ko" "${DEST_PATH}/rp-${PLATFORM}-${KVER}-dev.ko"
  rm -f "${DEST_PATH}/rp-${PLATFORM}-${KVER}-dev.ko.gz"
  gzip "${DEST_PATH}/rp-${PLATFORM}-${KVER}-dev.ko"
  mv "${OUT_PATH}/redpill-prod.ko" "${DEST_PATH}/rp-${PLATFORM}-${KVER}-prod.ko"
  rm -f "${DEST_PATH}/rp-${PLATFORM}-${KVER}-prod.ko.gz"
  gzip "${DEST_PATH}/rp-${PLATFORM}-${KVER}-prod.ko"
  rm -rf "${OUT_PATH}"
}

# Main
#docker pull fbelavenuto/syno-compiler:7.0
docker pull fbelavenuto/syno-compiler:7.1
docker pull fbelavenuto/syno-compiler:7.2

while read PLATFORM KVER TOOLKIT_VER; do
    compileLkm "${PLATFORM}" "${KVER}" "${TOOLKIT_VER}" &
done < PLATFORMS
wait
