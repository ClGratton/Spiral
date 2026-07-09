#!/usr/bin/env python3

import json
import os
import sys
from datetime import datetime, timezone
from pathlib import Path


def github_env(name, fallback):
    return os.environ.get(name, fallback)


def build_job_metadata():
    repository = github_env("GITHUB_REPOSITORY", "local/Spiral")
    server_url = github_env("GITHUB_SERVER_URL", "https://github.com")
    run_id = github_env("GITHUB_RUN_ID", "local")
    run_attempt = github_env("GITHUB_RUN_ATTEMPT", "1")
    workflow = github_env("GITHUB_WORKFLOW", "local")
    job = github_env("GITHUB_JOB", "dependency-submission")
    correlator = f"{workflow}:{job}:{run_id}:{run_attempt}"

    return {
        "correlator": correlator,
        "id": job,
        "html_url": f"{server_url}/{repository}/actions/runs/{run_id}"
    }


def build_snapshot(source):
    manifest = source["manifest"]
    resolved = {}
    for dependency in source["dependencies"]:
        resolved[dependency["name"]] = {
            "package_url": dependency["package_url"],
            "relationship": dependency.get("relationship", "direct"),
            "scope": dependency.get("scope", "runtime"),
            "dependencies": dependency.get("dependencies", []),
            "metadata": {
                "name": dependency["name"],
                "source_location": dependency.get("source_location", ""),
                "license": dependency.get("license", "")
            }
        }

    source_location = manifest["source_location"]
    return {
        "version": 0,
        "sha": github_env("GITHUB_SHA", "0000000000000000000000000000000000000000"),
        "ref": github_env("GITHUB_REF", "refs/heads/local"),
        "job": build_job_metadata(),
        "detector": source["detector"],
        "scanned": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
        "manifests": {
            source_location: {
                "name": manifest["name"],
                "file": {
                    "source_location": source_location
                },
                "resolved": resolved
            }
        }
    }


def main():
    if len(sys.argv) != 3:
        print("usage: GenerateDependencySnapshot.py <vendor-dependencies.json> <snapshot.json>", file=sys.stderr)
        return 2

    source_path = Path(sys.argv[1])
    output_path = Path(sys.argv[2])
    with source_path.open("r", encoding="utf-8") as source_file:
        source = json.load(source_file)

    snapshot = build_snapshot(source)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", encoding="utf-8", newline="\n") as output_file:
        json.dump(snapshot, output_file, indent=2)
        output_file.write("\n")

    return 0


if __name__ == "__main__":
    sys.exit(main())
