import os
from pathlib import Path

import pytest
from stormweaver.keyrings import available_keyrings

TDE_DIR = os.environ.get(
    "STORMWEAVER_PG_TDE_DIR", "/storage/tdework/inst/psp_rel_18_stable"
)

requires_pg_tde = pytest.mark.skipif(
    not (Path(TDE_DIR) / "bin" / "initdb").exists(),
    reason="no pg_tde-enabled postgres (set STORMWEAVER_PG_TDE_DIR)",
)

requires_kmip = pytest.mark.skipif(
    "kmip" not in available_keyrings(),
    reason="kmip keyring not available (no cosmian_kms binary/container)",
)
