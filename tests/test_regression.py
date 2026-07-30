"""
Test that the oscilator debug files remain the same.
"""
import filecmp
import os
import subprocess
from pathlib import Path

import pytest

import mtsespy as mts
from tuning_library import scala_files_to_frequencies

REPO_DIR = Path(__file__).parents[1]

TEST_DIRS = sorted((REPO_DIR / "tests/regression_test_data").glob("*/"))


def same(filename1, filename2):
    return filecmp.cmp(filename1, filename2, shallow=False)


mts.reinitialize()


@pytest.mark.parametrize("test_dir", TEST_DIRS, ids=lambda x: x.name)
def test_osc_files(test_dir, tmp_path):
    """
    Test that the oscillator debug files remain the same.

    For each test directory under tests/regression_test_data, the tuning in the scl
    file in that directory is set with an MTS-ESP master and the oscillator debug
    files generated. The files are then compared to the expected files in the test
    directory. Note that the debug files are only produced if the build was run
    with the DEBUG_TONEGEN_OSC env var set.
    """
    orig_dir = Path.cwd()
    try:
        os.chdir(tmp_path)
        print(test_dir)
        scl_files = sorted(test_dir.glob("*scl"))
        if scl_files:
            assert len(scl_files) == 1
            scl_file = scl_files[0]
            freqs = scala_files_to_frequencies(str(scl_file))
            with mts.Master():
                mts.set_note_tunings(freqs)
                subprocess.check_output(
                    REPO_DIR / "build/tuneBfree", env={"NO_JACK": "TRUE"}
                )
        else:
            subprocess.check_output(
                REPO_DIR / "build/tuneBfree", env={"NO_JACK": "TRUE"}
            )
        for filename in ["osc.txt", "osc_cfglists.txt", "osc_runtime.txt"]:
            expected_file = str(test_dir / filename)
            output_file = str(tmp_path / filename)
            assert same(expected_file, output_file)
    finally:
        os.chdir(orig_dir)
