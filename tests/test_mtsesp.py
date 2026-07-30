"""
Tests for tuneBfree

Uses the mtsespy Python wrapper available at
https://github.com/narenratan/mtsespy
"""
import subprocess
from pathlib import Path
from sys import platform

import pytest

import mtsespy as mts
from tuning_library import scala_files_to_frequencies

REPO_DIR = Path(__file__).parents[1]

SCL_FILES = sorted(
    str(x) for x in (REPO_DIR / "tests/scala_scale_archive/scl/").glob("*.scl")
)[::20]

mts.reinitialize()


def test_scala_files_found():
    """
    Check that the scale archive files have been found.

    Makes sure tests fail if scale archive submodule hasn't been cloned.
    """
    assert SCL_FILES


def test_lib_mts_found():
    """
    Check that the MTS-ESP shared library is installed.

    This is to make sure the Scala tunings are picked up; if the library is not
    installed the default 12TET tuning is used.
    """
    if platform == "linux":
        lib_path = Path("/usr/local/lib/libMTS.so")
    elif platform == "darwin":
        lib_path = Path("/Library/Application Support/MTS-ESP/libMTS.dylib")
    elif platform == "win32":
        lib_path = Path("/c/Program Files/Common Files/MTS-ESP/LIBMTS.dll")
    else:
        raise ValueError("Unexpected platform", platform)
    assert lib_path.exists()


@pytest.mark.parametrize("scl_file", SCL_FILES)
def test_cli(scl_file):
    """
    Test that running the CLI doesn't crash.

    Run CLI under an MTS-ESP master with tunings from different scala
    files. Check that the CLI process doesn't crash.
    """
    freqs = scala_files_to_frequencies(scl_file)
    with mts.Master():
        mts.set_note_tunings(freqs)
        subprocess.check_output(REPO_DIR / "build/tuneBfree", env={"NO_JACK": "TRUE"})
