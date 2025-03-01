"""Stress tests for RLlib (torch and tf).

Runs IMPALA on 4 GPUs and 100s of CPUs.
"""

import json
import os
from pathlib import Path

from ray.rllib.utils.test_utils import run_learning_tests_from_yaml

if __name__ == "__main__":
    # Get path of this very script to look for yaml files.
    abs_yaml_path = Path(__file__).parent
    print("abs_yaml_path={}".format(abs_yaml_path))

    # This pattern match is kind of hacky. Avoids cluster.yaml to get sucked
    # into this.
    yaml_files = abs_yaml_path.rglob("*tests.yaml")
    yaml_files = sorted(
        map(lambda path: str(path.absolute()), yaml_files), reverse=True)

    results = run_learning_tests_from_yaml(yaml_files, max_num_repeats=1)

    test_output_json = os.environ.get("TEST_OUTPUT_JSON",
                                      "/tmp/rllib_stress_test.json")
    with open(test_output_json, "wt") as f:
        json.dump(results, f)

    print("Ok.")
