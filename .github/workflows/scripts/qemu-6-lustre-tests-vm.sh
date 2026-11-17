#!/usr/bin/env bash

######################################################################
# 6) Test if Lustre can still build against ZFS
######################################################################
set -e

# Build from the latest Lustre tag rather than the master branch.  We do this
# under the assumption that master is going to have a lot of churn thus will be
# more prone to breaking the build than a point release.  We don't want ZFS
# PR's reporting bad test results simply because upstream Lustre accidentally
# broke their build.
#
# Skip any RC tags, or any tags where the last version digit is 50 or more.
# Versions with 50 or more are development versions of Lustre.
repo=https://github.com/lustre/lustre-release.git
tag="$(git ls-remote --refs --exit-code --sort=version:refname --tags $repo | \
	awk -F '_' '/-RC/{next}; /refs\/tags\/v/{if ($NF < 50){print}}' | \
	tail -n 1 | sed 's/.*\///')"

echo "Cloning Lustre tag $tag"
git clone --depth 1 --branch "$tag" "$repo"

cd lustre-release

# Include Lustre patches to build against master/zfs-2.4.x and the latest
# EL kernels.  Once these patches are merged we can remove these lines.
#
# LU-19539 osd-zfs: use osd_dmu_write() wrapper for xattrs
# LU-19761 osd-zfs: Build against ZFS 2.4.0
# LU-19249 build: Compatibility updates for kernel v6.16
# LU-19249 osd-zfs: support kernel v6.16
# LU-19459 build: Compatibility updates for kernel v6.17
#
patches=('https://review.whamcloud.com/changes/fs%2Flustre-release~62101/revisions/2/patch?download'
	'https://review.whamcloud.com/changes/fs%2Flustre-release~63267/revisions/9/patch?download'
	'https://review.whamcloud.com/changes/fs%2Flustre-release~60619/revisions/13/patch?download'
	'https://review.whamcloud.com/changes/fs%2Flustre-release~65188/revisions/2/patch?download'
	'https://review.whamcloud.com/changes/fs%2Flustre-release~61767/revisions/14/patch?download')

# The LU-19249 and LU-19459 patches are made against Lustre master, which
# moved libcfs/libcfs/crypto/ to lustre_compat/fs/crypto/, and where
# ll_write_end() names two variables differently.  Adjust those paths and
# context lines so that every hunk applies to the release tag.
for p in "${patches[@]}" ; do
	curl $p | base64 -d | sed \
	    -e 's|lustre_compat/fs/crypto/|libcfs/libcfs/crypto/|g' \
	    -e 's/^\( \t*struct wbe_folio \*\)folio,/\1vmfolio,/' \
	    -e 's/^\( \tstruct cl_page \*\)cl_page;$/\1page;/' > patch
	patch -p1 < patch || true
done

# EL 9.9 kernels backport Linux 7.1 commit 2420067cecac, which makes
# the union holding dentry.d_alias anonymous.  Lustre master handles it
# (LU-20373), but in a patch whose configure changes do not apply to the
# release tag.  Drop this once the release tag no longer uses d_u.d_alias.
dcache_h="/lib/modules/$(uname -r)/source/include/linux/dcache.h"
if [ -f "$dcache_h" ] && ! grep -q '} d_u;' "$dcache_h" ; then
	grep -rl 'd_u\.d_alias' lustre | \
	    xargs -r sed -i 's/d_u\.d_alias/d_alias/g'
fi

# EL 10.3 kernels backport Linux 6.15 commit 88d5baf69082, where ->mkdir()
# returns a dentry, but not c54b386969a5, where vfs_mkdir() does too.  The
# release tag takes the form of ll_vfs_mkdir() from ->mkdir(), so use its
# int form while vfs_mkdir() returns int.  Lustre master is fixing it
# (LU-20739); drop this once the release tag has that fix.
fs_h="/lib/modules/$(uname -r)/source/include/linux/fs.h"
if [ -f "$fs_h" ] && ! grep -q 'struct dentry \*vfs_mkdir(' "$fs_h" ; then
	grep -rl --include='*.h' '^#define ll_vfs_mkdir' . | \
	    xargs -r sed -i 's/^#ifdef HAVE_IOPS_MKDIR_RETURNS_DENTRY$/#if 0/'
fi

echo "Configure Lustre"
./autogen.sh
# EL 9 needs '--disable-gss-keyring'
./configure --with-zfs --disable-gss-keyring
echo "Building Lustre RPMs"
make rpms
ls *.rpm

# There's only a handful of Lustre RPMs we actually need to install
lustrerpms="$(ls *.rpm | grep -E 'kmod-lustre-osd-zfs-[0-9]|kmod-lustre-[0-9]|lustre-osd-zfs-mount-[0-9]')"
echo "Installing: $lustrerpms"
sudo dnf -y install $lustrerpms
sudo modprobe -v lustre

# Should see some Lustre lines in dmesg
sudo dmesg | grep -Ei 'lnet|lustre'
