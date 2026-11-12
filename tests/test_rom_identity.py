"""tools/rom_identity.py: rom_identity.txt, a project's single source of truth
for its ROM digests, becomes snesrecomp_rom_identity.h."""

import pathlib
import shutil
import subprocess
import sys
import tempfile

import pytest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
EMITTER = REPO_ROOT / "tools" / "rom_identity.py"

IDENTITY = """\
# a comment, and a blank line follow

display_name    = Fixture Quest
rom_file        = fixture.sfc
expected_crc32  = 6852cf05
expected_sha256 = 4400876a57dc43a85021732727b673def55fd2008c7329ce4fcfc6792ac5ce70
mapping         = lorom
region          = JPN
game_id         = fixture-jp
"""


def _python_header(tmp: pathlib.Path, identity: pathlib.Path) -> str:
    out = tmp / "py"
    subprocess.run(
        [sys.executable, str(EMITTER), str(identity), "--header", str(out)],
        check=True, capture_output=True, text=True)
    return (out / "snesrecomp_rom_identity.h").read_text(encoding="utf-8")


def test_values_reach_the_header():
    with tempfile.TemporaryDirectory() as directory:
        tmp = pathlib.Path(directory)
        identity = tmp / "rom_identity.txt"
        identity.write_text(IDENTITY, encoding="utf-8")
        header = _python_header(tmp, identity)
        assert '"6852cf05"' in header
        assert ('"4400876a57dc43a85021732727b673def55fd2008c7329ce4fcfc6792ac'
                '5ce70"') in header
        assert '"fixture.sfc"' in header
        assert '#define SNESRECOMP_ROM_GAME_ID         "fixture-jp"' in header


def test_missing_field_is_empty_not_absent():
    """A consumer reads "" as 'cannot verify'. The macro must still exist, or
    every host fails to compile against an older project file."""
    with tempfile.TemporaryDirectory() as directory:
        tmp = pathlib.Path(directory)
        identity = tmp / "rom_identity.txt"
        identity.write_text("display_name = Only This\n", encoding="utf-8")
        header = _python_header(tmp, identity)
        assert '#define SNESRECOMP_ROM_EXPECTED_SHA256 ""' in header
        assert '#define SNESRECOMP_ROM_EXPECTED_CRC32  ""' in header


def test_quoted_values_are_unwrapped():
    with tempfile.TemporaryDirectory() as directory:
        tmp = pathlib.Path(directory)
        identity = tmp / "rom_identity.txt"
        identity.write_text('rom_file = "Game (USA).sfc"\n', encoding="utf-8")
        header = _python_header(tmp, identity)
        assert '#define SNESRECOMP_ROM_FILE            "Game (USA).sfc"' in header
