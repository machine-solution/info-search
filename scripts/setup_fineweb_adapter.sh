#!/usr/bin/env bash
set -euo pipefail

venv_dir="${1:-.venv-fineweb}"
python3 -m venv "$venv_dir"
"$venv_dir/bin/python" -m pip install --upgrade pip
"$venv_dir/bin/python" -m pip install 'pyarrow==22.0.0'
echo "FineWeb adapter ready in $venv_dir" >&2
