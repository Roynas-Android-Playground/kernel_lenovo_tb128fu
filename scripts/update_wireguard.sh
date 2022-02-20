#!/bin/bash

git clone git://git.zx2c4.com/wireguard-linux-compat
rm -fr drivers/net/wireguard/*

perl -i -ne 'BEGIN{$print=1;} $print = 0 if m/cat/; print $_ if $print;' wireguard-linux-compat/kernel-tree-scripts/create-patch.sh
sed -i 's|net/wireguard|drivers/net/wireguard|g' wireguard-linux-compat/kernel-tree-scripts/create-patch.sh

dos2unix wireguard-linux-compat/kernel-tree-scripts/create-patch.sh

wireguard-linux-compat/kernel-tree-scripts/create-patch.sh | patch -p1

rm -fr wireguard-linux-compat
