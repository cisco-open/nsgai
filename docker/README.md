# nsgai container test images

Each `Dockerfile.<platform>` here builds a full toolchain for that distro
and runs the same test suite via `scripts/run-container-tests.sh`. All
images build/verify c-ares the same way (see `scripts/install-cares.sh`):
use the distro package if it meets nsgai's minimum supported version
(currently 1.27.0), otherwise build the pinned upstream ref from source.

| `--platform` value | Dockerfile               | Base image      |
|---------------------|--------------------------|-----------------|
| `fedora`            | `Dockerfile.fedora`      | `fedora:44`     |
| `ubuntu22.04`       | `Dockerfile.ubuntu22.04` | `ubuntu:22.04`  |
| `ubuntu24.04`       | `Dockerfile.ubuntu24.04` | `ubuntu:24.04`  |
| `debian12`          | `Dockerfile.debian12`    | `debian:12`     |
| `rockylinux9`       | `Dockerfile.rockylinux9` | `rockylinux:9`  |

`rockylinux9` is representative of the RHEL 9 family (Rocky/Alma/RHEL).

## Architecture

All five Dockerfiles are architecture-agnostic: `dnf`/`apt` resolve the
right per-arch packages automatically, and the CoreDNS download branches
on `uname -m` (amd64/arm64/armv7 supported). Each image builds natively
for whatever architecture the container runtime's host/emulation targets
— there is nothing arch-specific to set inside the Dockerfiles themselves.

`scripts/run-container-tests.sh` tags images and names build directories
with the architecture (normalized to `amd64`/`arm64`) so that testing the
same platform on two architectures — e.g. two developer machines, or
emulation via `qemu-user-static` — never collides:

```sh
# Defaults to the host's own architecture
bash scripts/run-container-tests.sh --platform ubuntu22.04

# Explicit override (useful under binfmt/qemu emulation)
bash scripts/run-container-tests.sh --platform ubuntu22.04 --arch arm64
```

GitHub Actions builds/tests every platform on both `amd64` and `arm64`
hosted runners (see `.github/workflows/docker-matrix.yml`).

Usage:

```sh
# Single platform (default: fedora, host architecture)
bash scripts/run-container-tests.sh --platform ubuntu22.04

# Every platform in the matrix, one after another
bash scripts/run-container-tests.sh --all
```

Each platform+arch combination gets its own image tag
(`nsgai-test:<platform>-<arch>`) and its own container-owned build tree
(`build-container-<platform>-<arch>/`), so runs don't clobber each other.

