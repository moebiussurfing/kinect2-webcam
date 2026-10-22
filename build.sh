#!/usr/bin/env bash
# Build helper for kinect2-webcam.
#   ./build.sh         compile ./kinect2-webcam (needs libfreenect2 in ~/.local/freenect2)
#   ./build.sh test    compile and run the effects tests (no Kinect needed)
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
FN2="${FREENECT2_PREFIX:-$HOME/.local/freenect2}"
FLAGS=(-std=c++20 -O3 -fopenmp -Wall -Wextra)

case "${1:-build}" in
  test)
    g++ "${FLAGS[@]}" test_effects.cpp -o /tmp/test_effects
    /tmp/test_effects
    ;;
  build)
    g++ "${FLAGS[@]}" -I"$FN2/include" kinect2-webcam.cpp -o kinect2-webcam \
      -L"$FN2/lib" -lfreenect2 -Wl,-rpath,"$FN2/lib"
    ;;
  *)
    echo "usage: $0 [build|test]" >&2
    exit 2
    ;;
esac
