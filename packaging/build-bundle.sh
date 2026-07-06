#!/bin/bash
# builds a self-contained tarball/deb/rpm bundle: wheel + standalone python.
set -euo pipefail

: "${1:?usage: build-bundle.sh <wheel> <version>}"
: "${2:?usage: build-bundle.sh <wheel> <version>}"
WHEEL="$1"
VERSION="$2"

# python-build-standalone pin. freethreaded-install_only_stripped is the
# smallest asset that ships a full working runtime + pip (~36MB vs 125MB+
# for the pgo+lto-full builds) - good enough, we don't need pgo for this.
PBS_TAG="${PBS_TAG:-20260623}"
PBS_PY="${PBS_PY:-3.14.6}"
PBS_URL="https://github.com/astral-sh/python-build-standalone/releases/download/${PBS_TAG}/cpython-${PBS_PY}+${PBS_TAG}-x86_64-unknown-linux-gnu-freethreaded-install_only_stripped.tar.gz"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$ROOT/dist"

curl -fsSL "$PBS_URL" -o "$STAGE/python.tar.gz"
mkdir -p "$STAGE/stormweaver/runtime"
tar -xzf "$STAGE/python.tar.gz" -C "$STAGE/stormweaver/runtime" --strip-components=1
PYBIN="$STAGE/stormweaver/runtime/bin/python3.14t"

# guard: dies here already if the pinned asset isn't actually freethreaded
"$PYBIN" -c 'import sys; assert not sys._is_gil_enabled()'

"$PYBIN" -m pip install --no-cache-dir "$WHEEL"

# pip drops a console-script stub here with a stage-absolute shebang, dead
# on arrival once the stage dir is gone - kill it, our launcher is the
# real entry point and uses module invocation instead.
rm -f "$STAGE/stormweaver/runtime/bin/stormweaver"

mkdir -p "$STAGE/stormweaver/bin"
cat > "$STAGE/stormweaver/bin/stormweaver" <<'LAUNCHER'
#!/bin/sh
HERE="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
exec "$HERE/runtime/bin/python3.14t" -I -m stormweaver "$@"
LAUNCHER
chmod +x "$STAGE/stormweaver/bin/stormweaver"

tar -C "$STAGE" -czf "$ROOT/dist/stormweaver-${VERSION}.tar.gz" stormweaver

VERSION="$VERSION" STAGE="$STAGE" nfpm pkg -f "$ROOT/packaging/nfpm.yaml" -p deb -t "$ROOT/dist/"
VERSION="$VERSION" STAGE="$STAGE" nfpm pkg -f "$ROOT/packaging/nfpm.yaml" -p rpm -t "$ROOT/dist/"

ls -l "$ROOT/dist"
