#!/usr/bin/env bash
# Initial devcontainer setup.
set -euo pipefail

sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  build-essential \
  autoconf automake \
  python3 python3-pip \
  file \
  shellcheck

# musl cross toolchains for local static builds (`build.sh --musl`). build.sh
# needs <triple>-gcc/<triple>-strip, which Ubuntu's musl-tools doesn't provide
# (it only ships musl-gcc), so every architecture comes from musl.cc. The
# SHA-512 values are musl.cc's published SHA512SUMS. CI doesn't use these: it
# builds in Alpine containers (.github/workflows/build-transcoder.yml), since
# musl.cc doesn't answer GitHub's runners.
declare -A MUSL_CROSS_SHA512=(
  [x86_64-linux-musl]=52abd1a56e670952116e35d1a62e048a9b6160471d988e16fa0e1611923dd108a581d2e00874af5eb04e4968b1ba32e0eb449a1f15c3e4d5240ebe09caf5a9f3
  [aarch64-linux-musl]=8695ff86979cdf30fbbcd33061711f5b1ebc3c48a87822b9ca56cde6d3a22abd4dab30fdcd1789ac27c6febbaeb9e5bde59d79d66552fae53d54cc1377a19272
  [arm-linux-musleabihf]=fe006d9176cedb453fd817f892f61f6bac273c15879f9c537e22c75b8da4995991211f6d23b0c0c97a87121fe55cf9f9f29cc3d1cf9376804535f07b6c017729
)
profile_path=""
for triple in x86_64-linux-musl aarch64-linux-musl arm-linux-musleabihf; do
  dir="/opt/musl-cross/${triple}-cross"
  if [ ! -d "$dir" ]; then
    tgz="$(mktemp)"
    curl -sSfL -o "$tgz" "https://musl.cc/${triple}-cross.tgz"
    echo "${MUSL_CROSS_SHA512[$triple]}  $tgz" | sha512sum -c -
    sudo mkdir -p /opt/musl-cross
    sudo tar xzf "$tgz" -C /opt/musl-cross
    rm -f "$tgz"
  fi
  profile_path="${dir}/bin:${profile_path}"
done
echo "export PATH=\"${profile_path}\$PATH\"" | sudo tee /etc/profile.d/musl-cross.sh >/dev/null

npm install --prefix "$(dirname "${BASH_SOURCE[0]}")/.." || true

echo "devcontainer setup complete."
echo "  - Native build of transcoder: transcoder/scripts/build.sh"
echo "  - musl static build: transcoder/scripts/build.sh --musl [--arch arm64]"
echo "  - Regenerate the conversion tables: python3 tools/gen-tables/gen_tables.py"
echo "  - Tests: python3 tests/test_encodings.py"
