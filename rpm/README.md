## Building an RPM (Fedora / rpm-ostree / Bazzite)

Building an RPM is the recommended approach for immutable systems (Bazzite, Silverblue, etc.)
where layered packages survive OS updates.

On Bazzite and other rpm-ostree hosts `dnf` and `rpmbuild` do not run directly, so do the build
inside a Fedora toolbox or distrobox of the same release as the host (`rpm -E %fedora` prints it)
and skip the tmpfs mount there. Your home directory is shared with the container, so afterwards
layer the RPM from the host.

### Prerequisites: RPM Fusion + ffmpeg

```sh
sudo dnf install -y \
    https://mirrors.rpmfusion.org/free/fedora/rpmfusion-free-release-$(rpm -E %fedora).noarch.rpm \
    https://mirrors.rpmfusion.org/nonfree/fedora/rpmfusion-nonfree-release-$(rpm -E %fedora).noarch.rpm

sudo dnf swap -y ffmpeg-free ffmpeg --allowerasing
sudo dnf install -y ffmpeg-devel --allowerasing
```

### Build steps

```sh
git clone https://github.com/RainyPixel/wallpaper-engine-kde-plugin.git
cd wallpaper-engine-kde-plugin

# Install all build dependencies declared in the spec
sudo dnf builddep ./rpm/wek.spec

# Initialise submodules
git submodule update --init --force --recursive

# Use tmpfs to speed up the build and avoid wearing disk
sudo mount -t tmpfs tmpfs ~/rpmbuild/BUILD

rpmbuild --define="commit $(git rev-parse HEAD)" \
    --define="reporoot $(pwd)" \
    --define="glslang_ver 11.8.0" \
    --undefine=_disable_source_fetch \
    -ba ./rpm/wek.spec

sudo umount ~/rpmbuild/BUILD
```

### Install

Standard Fedora:
```sh
sudo dnf install ~/rpmbuild/RPMS/x86_64/wallpaper-engine-kde-plugin-qt6-*.rpm
```

rpm-ostree / Bazzite:
```sh
rpm-ostree install ~/rpmbuild/RPMS/x86_64/wallpaper-engine-kde-plugin-qt6-*.rpm
```
