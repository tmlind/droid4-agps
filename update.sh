#!/bin/sh

tmp_file=$(mktemp)

echo "Injecting time.."
./droid4-agps --inject-time

echo "Downloading xtra2.bin to ${tmp_file}.."
wget -O ${tmp_file} http://xtrapath6.izatcloud.net/xtra2.bin

echo "Uplading xtra2.bin ${tmp_file} to mdm66oo.."
./droid4-agps --upload-only=${tmp_file}
