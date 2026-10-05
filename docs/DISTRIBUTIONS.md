# Preparing distributions / 准备发行文件

The manual `Prepare distributions` workflow builds downloadable wheels and a
standalone C++ SDK/CLI. It uploads Actions artifacts for review; it does not
create a GitHub Release or publish to PyPI. The existing CI remains separate.

手动工作流准备可下载的 wheel 和独立 C++ SDK/CLI，保存为 Actions 产物供审核。
它不会创建 GitHub Release，也不会发布到 PyPI。原有 CI 独立保留。

## Build targets / 构建目标

| Artifact | Target | Runtime requirement |
| --- | --- | --- |
| Python wheels | Linux x86_64, manylinux_2_28 | glibc 2.28+, matching CPython 3.12, 3.13 or 3.14 |
| Python wheels | macOS arm64 | macOS 14+, matching CPython 3.12, 3.13 or 3.14 |
| Python wheels | Windows AMD64 | Matching 64-bit CPython 3.12, 3.13 or 3.14 |
| C++ SDK and CLI | Native Linux x86_64, macOS arm64, Windows AMD64 runners | Native toolchain/platform ABI; CLI needs no Python |

These describe configured build targets, not a claim that every target has been
validated. The ordinary CI's Linux wheel uses its native runner and is not a
manylinux portability promise. The distribution workflow uses
[cibuildwheel](https://cibuildwheel.pypa.io/en/stable/) for the Linux wheel baseline.
Alpine/musl, macOS Intel, Windows ARM and free-threaded Python are not covered by
this matrix. Native Linux SDK builds do not share the manylinux wheel baseline.

表格描述构建配置，不代表每个平台已验收。普通 CI 的 Linux wheel 不承诺跨发行版兼容；
发行工作流用 cibuildwheel 构建 manylinux wheel。当前矩阵不覆盖 Alpine/musl、
Intel Mac、Windows ARM 或自由线程 Python。Linux SDK 的 ABI 取决于原生构建环境。

The existing core's floating-point formatting API requires macOS 13.3. However,
macOS 11+ wheel tags encode major versions with minor 0. The wheel baseline is
therefore 14.0 to avoid promising compatibility with 13.0; a separately built
C++ SDK can target 13.3. Renaming a wheel cannot change binary compatibility.
See [PyPA's compatibility tags](https://packaging.python.org/en/latest/specifications/platform-compatibility-tags/).

现有浮点格式化接口需要 macOS 13.3，但 macOS 11 之后的 wheel 标签只按大版本表示。
因此 wheel 设为 14.0，避免误承诺 13.0 兼容；独立 C++ SDK 可设为 13.3。
改名不会改变二进制兼容性。

## Local preparation / 本地准备

Build outside the checkout. `FATCAT_BUILD_TESTS=OFF` excludes test executables;
the commands below prepare artifacts without running automated tests.

在源码目录外构建。下列命令关闭测试目标，只准备发行文件。

```bash
cmake -S cpp -B /tmp/fatcat-build -DFATCAT_BUILD_PYTHON=OFF \
  -DFATCAT_BUILD_TESTS=OFF -DFATCAT_INSTALL_CPP=ON -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=13.3
cmake --build /tmp/fatcat-build --config Release --parallel 2
cmake --install /tmp/fatcat-build --config Release --prefix /tmp/fatcat-sdk
python scripts/package_distribution.py --sdk-prefix /tmp/fatcat-sdk \
  --output-dir /tmp/fatcat-distribution
```

The SDK includes `bin/fatcat`, public headers, static libraries, CMake package
files, bundled data and license notices. Consumer projects need a compatible C++17
toolchain; dependencies obtained from system packages at build time must also be
available to the consumer. Read [the C++ guide](../cpp/README.md) before linking.

SDK 包含命令、公共头文件、静态库、CMake 配置、数据及许可声明。链接 SDK 仍需兼容的
C++17 工具链；构建时使用的系统依赖也需在消费方提供。

For a matching local wheel:

```bash
MACOSX_DEPLOYMENT_TARGET=14.0 python -m pip wheel . --no-deps --wheel-dir /tmp/fatcat-wheels
python scripts/package_distribution.py --wheel-dir /tmp/fatcat-wheels \
  --output-dir /tmp/fatcat-distribution
```

On Windows, set the environment variable using your shell's syntax or omit this
macOS-only setting. Use an output directory outside both the checkout and SDK
prefix. Building a source wheel requires a compiler; installing a matching
prebuilt wheel does not. [FIRST_USE.md](FIRST_USE.md) describes the consumer flow.

Windows 请使用相应 shell 设置环境变量，或省略 macOS 专用设置。输出目录应放在源码和
SDK 目录之外。源码构建需要编译器，安装匹配的预编译 wheel 不需要。

## Identity and receipts / 来源与清单

`distribution.json` records the packaging checkout's version, source revision,
dirty status, supported target manifest, wheel tags and each file's SHA-256.
SDK provenance is compared with the executable before packaging. Wheel receipts
describe the packaging run; they do not independently certify which source an
arbitrary supplied wheel was built from. Supply only wheels from that build.

清单记录打包时的版本、源码 SHA、改动状态、目标快照、wheel 标签及文件哈希。
SDK 会核对命令内嵌来源。wheel 清单记录打包过程，不会独立证明任意传入 wheel 的源码来源；
请只传入本轮构建的 wheel。

Git checkouts embed their actual revision and dirty flag. Git-free build inputs
are unknown by default. The container workflow supplies `FATCAT_BUILD_REVISION`
and `FATCAT_BUILD_DIRTY` from its checked-out revision; these values are declared
build provenance, not a cryptographic source attestation.

有 Git 信息时记录实际 SHA 和改动状态。无 Git 信息默认记为未知；容器工作流可通过环境
变量传入已检出的 SHA 和状态。这是构建来源声明，不是源码的密码学认证。

Prepared SDK filenames include the revision and `-dirty` where applicable.
For a public release, use a reviewed clean commit and retain the receipt beside
the artifacts. Keep package version 0.1.0 during this pre-release phase. Do not
silently replace a published artifact under the same filename/version; decide
the release/version policy before publication. Actions artifacts expire after
90 days and are not a durable public distribution channel.

公开发行应来自审核后的干净提交，并同时保留清单。当前预发布阶段版本保持 0.1.0；
正式发布前再决定版本策略，不应同名覆盖已发布文件。Actions 产物 90 天后过期，
不能代替长期公开发行渠道。

Hashes and a successful build do not establish slicer GUI acceptance, slicing
success, or printer compatibility. License terms remain unchanged; publication
and license decisions are separate from this artifact preparation.

哈希与构建成功不等于切片软件打开、切片或实体兼容验收。许可条款保持不变；
正式发布和许可决策需要另行处理。
