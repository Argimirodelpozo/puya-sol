"""Compare individual JUnit outcomes; JUnit counts non-strict XPASS as passed."""

import collections
import json
import sys
import xml.etree.ElementTree as ET


def outcomes(path):
    result = {}
    for case in ET.parse(path).iter("testcase"):
        key = case.get("classname"), case.get("name")
        assert key not in result, key
        result[key] = (
            "failed" if case.find("failure") is not None else
            "error" if case.find("error") is not None else
            "skipped" if case.find("skipped") is not None else "passed"
        )
    return result


baseline, current = (outcomes(path) for path in sys.argv[1:])
common = baseline.keys() & current.keys()
print(json.dumps({
    "baseline_cases": len(baseline), "current_cases": len(current),
    "common_cases": len(common),
    "current_junit_outcomes": dict(collections.Counter(current.values())),
    "changed_outcomes": [
        {"classname": key[0], "name": key[1], "before": baseline[key], "after": current[key]}
        for key in sorted(common) if baseline[key] != current[key]
    ],
    "added": [{"classname": key[0], "name": key[1], "outcome": current[key]}
              for key in sorted(current.keys() - baseline.keys())],
    "removed": [{"classname": key[0], "name": key[1], "outcome": baseline[key]}
                for key in sorted(baseline.keys() - current.keys())],
}, indent=2))
