#!/bin/sh
# Build an ufbt SDK whose API version matches the Flipper's firmware exactly:
# the Momentum mntm-012 SDK with api_symbols.csv taken from the firmware commit
# the Flipper runs. Usage: tools/make_sdk.sh <firmware commit> <work dir>
# Then: ufbt update --local=<work dir>/sdk-matched.zip --hw-target=f7
set -eu
commit=${1:-93ac669b}
work=${2:?work dir}
mkdir -p "$work"
cd "$work"
[ -f sdk-mntm-012.zip ] || curl -sfL -o sdk-mntm-012.zip \
  https://up.momentum-fw.dev/builds/firmware/mntm-012/flipper-z-f7-sdk-mntm-012.zip
gh api "repos/Next-Flip/Momentum-Firmware/contents/targets/f7/api_symbols.csv?ref=$commit" \
  -H 'Accept: application/vnd.github.raw' > api_symbols.csv
grep '^Version' api_symbols.csv
rm -rf x && mkdir x && cd x
unzip -q ../sdk-mntm-012.zip
cp ../api_symbols.csv sdk_headers/f7_sdk/targets/f7/api_symbols.csv
rm -f ../sdk-matched.zip
zip -qr ../sdk-matched.zip .
echo "$work/sdk-matched.zip"
