# SPDX source SBOM

`nsgai-source.spdx.json.in` is the checked-in SPDX 2.3 source-SBOM
template. It describes the source release's declared dependencies; it does
not describe a built or deployed artifact.

Configure the project and generate the SBOM with:

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/c-ares
cmake --build build --target source-sbom
```

The configured output is
`build/sbom/nsgai-<project-version>-source.spdx.json`. The target does not
compile project sources.

For a release, configure an otherwise clean build directory with stable,
release-specific metadata:

```sh
cmake -S . -B build-release \
  -DCMAKE_PREFIX_PATH=/path/to/c-ares \
  -DNSGAI_SPDX_NAMESPACE_BASE=https://example.org/nsgai/spdx \
  -DNSGAI_SPDX_SOURCE_DOWNLOAD_LOCATION=https://example.org/nsgai/archive/v0.2.1.tar.gz \
  -DNSGAI_SPDX_CREATED=2026-09-22T00:00:00Z
cmake --build build-release --target source-sbom
```

The `NSGAI_SPDX_CREATED` default is the CMake configuration time. Release
automation must set it explicitly so repeated builds can reproduce the same
SBOM. Update `NSGAI_SPDX_NAMESPACE_BASE` to the final public repository URL
before the first public release. Attach the configured JSON alongside the
source archive; the archive itself contains this template and README.